/*
https://github.com/noporpoise/seq_file
Isaac Turner <turner.isaac@gmail.com>
Jan 2014, Public Domain
*/

// request decent POSIX version
#define _XOPEN_SOURCE 700
#define _BSD_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h> // INT_MAX
#include <ctype.h> // toupper tolower

#include <time.h>
#include <sys/time.h> // for seeding random
#include <unistd.h> // getpid()

#include "seq_file.h"

#define OPS_UPPERCASE   1
#define OPS_LOWERCASE   2
#define OPS_REVERSE     4
#define OPS_COMPLEMENT  8
#define OPS_MASK       16

static struct option longopts[] =
{
// General options
  {"help",       no_argument,       NULL, 'h'},
  {"fasta",      no_argument,       NULL, 'f'},
  {"fastq",      no_argument,       NULL, 'q'},
  {"plain",      no_argument,       NULL, 'p'},
  {"wrap",       required_argument, NULL, 'w'},
  {"uppercase",  no_argument,       NULL, 'u'},
  {"lowercase",  no_argument,       NULL, 'l'},
  {"revcmp",     no_argument,       NULL, 'r'},
  {"reverse",    no_argument,       NULL, 'R'},
  {"complement", no_argument,       NULL, 'C'},
  {"interleave", no_argument,       NULL, 'i'},
  {"mask",       no_argument,       NULL, 'm'},
  {"rand",       required_argument, NULL, 'n'},
  {NULL, 0, NULL, 0}
};

const char shortopts[] = "hfqpw:ulrRCimn:";
const char *cmdstr;

const char bases[] = "ACGT";

char parse_entire_size(const char *str, size_t *result)
{
  char *strtol_last_char_ptr = NULL;
  if(*str < '0' || *str > '9') return 0;
  unsigned long tmp = strtoul(str, &strtol_last_char_ptr, 10);
  if(tmp > SIZE_MAX) return 0;
  if(strtol_last_char_ptr == NULL || *strtol_last_char_ptr != '\0') return 0;
  *result = (size_t)tmp;
  return 1;
}

void print_usage(const char *err, ...)
__attribute__((noreturn))
__attribute__((format(printf, 1, 2)));


void print_usage(const char *err, ...)
{
  if(err != NULL) {
    fputc('\n', stderr);
    va_list argptr;
    fprintf(stderr, "%s Error: ", cmdstr);
    va_start(argptr, err);
    vfprintf(stderr, err, argptr);
    va_end(argptr);
    fputc('\n', stderr);
    fputc('\n', stderr);
  }

  fprintf(stderr, "Usage: %s [OPTIONS] <file1> [file2] ..\n", cmdstr);
  fprintf(stderr, "  Read and manipulate dna sequence.\n"
#ifdef _USESAM
"  Compiled with SAM/BAM support.\n"
#endif
"\n"
"  -h,--help        show this help text\n"
"  -f,--fasta       print in FASTA format\n"
"  -q,--fastq       print in FASTQ format\n"
"  -p,--plain       print in plain format\n"
"  -w,--wrap <n>    wrap lines by <n> characters [default: 0 (off)]\n"
"  -u,--uppercase   convert sequence to uppercase\n"
"  -l,--lowercase   convert sequence to lowercase\n"
"  -r,--revcmp      reverse complement sequence [i.e. -R and -C]\n"
"  -R,--reverse     reverse sequence\n"
"  -C,--complement  complement sequence\n"
"  -i,--interleave  interleave input files\n"
"  -m,--mask        mask lowercase bases\n"
"  -n,--rand <n>    print <n> random bases AFTER reading files\n");

  exit(EXIT_FAILURE);
}

// 2 ops per byte h = strhash_fast_mix(h,x)
#define strhash_fast_mix(h,x) ((h) * 37 + (x))

static void seed_random()
{
  struct timeval now;
  gettimeofday(&now, NULL);

  uint32_t h;
  h = strhash_fast_mix(0, (uint32_t)now.tv_sec);
  h = strhash_fast_mix(h, (uint32_t)now.tv_usec);
  h = strhash_fast_mix(h, (uint32_t)getpid());
  srand(h);
}

// Returns format used
static uint8_t read_print(seq_file_t *sf, read_t *r,
                          uint8_t fmt, uint8_t ops, size_t linewrap)
{
  size_t i;
  if(ops & OPS_UPPERCASE)  seq_read_to_uppercase(r);
  if(ops & OPS_LOWERCASE)  seq_read_to_lowercase(r);
  if((ops & OPS_REVERSE) && (ops & OPS_COMPLEMENT)) seq_read_reverse_complement(r);
  else if(ops & OPS_REVERSE)    seq_read_reverse(r);
  else if(ops & OPS_COMPLEMENT) seq_read_complement(r);

  if(ops & OPS_MASK) {
    for(i = 0; i < r->seq.end; i++)
      if(islower(r->seq.b[i]))
        r->seq.b[i] = 'N';
  }

  if(fmt == 0) {
    if(seq_is_plain(sf)) fmt = SEQ_FMT_PLAIN;
    else if(seq_is_fasta(sf)) fmt = SEQ_FMT_FASTA;
    else fmt = SEQ_FMT_FASTQ;
  }

  switch(fmt) {
    case SEQ_FMT_FASTA: seq_print_fasta(r, stdout, linewrap); break;
    case SEQ_FMT_FASTQ: seq_print_fastq(r, stdout, linewrap); break;
    case SEQ_FMT_PLAIN: fputs(r->seq.b, stdout); fputc('\n', stdout); break;
    default: fprintf(stderr, "Got value: %i\n", (int)fmt); abort();
  }

  return fmt;
}

static inline void _print_rnd_entries(const size_t *lens, size_t nentries,
                                      uint8_t fmt, size_t linewrap)
{
  size_t i, j, k, rnd = 0;
  for(i = 0; i < nentries; i++) {
    if(fmt&SEQ_FMT_FASTA) printf(">rand%zu\n", i);
    else if(fmt&SEQ_FMT_FASTQ) printf("@rand%zu\n", i);

    for(j = k = 0; j < lens[i]; j++, k++) {
      if(linewrap && k == linewrap) { k = 0; fputc('\n', stdout); }
      // use 2 bits per iteration, 32 bits in rand(), update every 16 iterations
      if((j & 15) == 0) rnd = (size_t)rand();
      fputc(bases[rnd&3], stdout);
      rnd >>= 2;
    }
    if(fmt&SEQ_FMT_FASTQ) { /* quality scores */
      fputs("\n+\n", stdout);
      for(j = k = 0; j < lens[i]; j++, k++) {
        if(linewrap && k == linewrap) { k = 0; fputc('\n', stdout); }
        fputc('.', stdout);
      }
    }
    fputc('\n', stdout);
  }
}

static void vector_push(size_t **ptr, size_t *len, size_t *cap, size_t x)
{
  if(!*ptr || *len >= *cap) {
    if(!*cap) { *cap = 16; }
    else { while(*len >= *cap) { *cap *= 2; }}
    *ptr = realloc(*ptr, *cap * sizeof(size_t));
  }
  (*ptr)[(*len)++] = x;
}

int main(int argc, char **argv)
{
  cmdstr = argv[0];

  bool interleave = false;
  uint8_t ops = 0, fmt = SEQ_FMT_UNKNOWN;
  size_t i, linewrap = 0;

  size_t *nrand = NULL, nrand_len = 0, nrand_cap = 0, tmprnd = 0;

  if(argc == 1) print_usage(NULL);

  // Arg parsing
  int c;
  opterr = 0; // silence getopt error messages

  while((c = getopt_long_only(argc, argv, shortopts, longopts, NULL)) != -1) {
    switch(c) {
      case 0: /* flag set */ break;
      case 'h': print_usage(NULL); break;
      case 'f': fmt |= SEQ_FMT_FASTA; break;
      case 'q': fmt |= SEQ_FMT_FASTQ; break;
      case 'p': fmt |= SEQ_FMT_PLAIN; break;
      case 'w':
        if(!parse_entire_size(optarg, &linewrap))
          print_usage("Bad -w argument: %s\n", optarg);
        break;
      case 'u': ops |= OPS_UPPERCASE; break;
      case 'l': ops |= OPS_LOWERCASE; break;
      case 'r': ops |= OPS_REVERSE | OPS_COMPLEMENT; break;
      case 'R': ops |= OPS_REVERSE; break;
      case 'C': ops |= OPS_COMPLEMENT; break;
      case 'm': ops |= OPS_MASK; break;
      case 'n':
        if(!parse_entire_size(optarg, &tmprnd))
          print_usage("Bad -n argument: %s\n", optarg);
        vector_push(&nrand, &nrand_len, &nrand_cap, tmprnd);
        break;
      case 'i': interleave = true; break;
      case ':': /* BADARG */
      case '?': /* BADCH getopt_long has already printed error */
        print_usage("Bad option: %s\n", argv[optind-1]);
      default: abort();
    }
  }

  if(!!(fmt&SEQ_FMT_FASTA) + !!(fmt&SEQ_FMT_FASTQ) + !!(fmt&SEQ_FMT_PLAIN) > 1)
    print_usage("Please specify only one output format (-f,-q,-p)\n");

  size_t num_inputs = argc - optind;
  char **input_paths = argv + optind;

  if(!nrand_len && !num_inputs)
    print_usage("Please specify at least one input file\n");

  // Default to plain format for random output
  if(!num_inputs && !fmt) fmt = SEQ_FMT_PLAIN;

  if(linewrap && (fmt & SEQ_FMT_PLAIN))
    print_usage("Bad idea to use linewrap with plain output (specify -f or -q)");

  if(nrand_len) seed_random();

  read_t r;
  seq_read_alloc(&r);

  seq_file_t *inputs[num_inputs];

  for(i = 0; i < num_inputs; i++) {
    if((inputs[i] = seq_open(input_paths[i])) == NULL)
      print_usage("Couldn't read file: %s\n", input_paths[i]);
  }

  if(interleave) {
    // read one entry from each file
    size_t waiting_files = num_inputs;
    while(waiting_files) {
      for(i = 0; i < num_inputs; i++) {
        if(inputs[i] != NULL) {
          if(seq_read(inputs[i],&r) > 0)
            fmt = read_print(inputs[i], &r, fmt, ops, linewrap);
          else { seq_close(inputs[i]); inputs[i] = NULL; waiting_files--; }
        }
      }
    }
  }
  else {
    for(i = 0; i < num_inputs; i++) {
      while(seq_read(inputs[i],&r) > 0)
        fmt = read_print(inputs[i], &r, fmt, ops, linewrap);
      seq_close(inputs[i]);
    }
  }

  seq_read_dealloc(&r);

  // Print random entries
  _print_rnd_entries(nrand, nrand_len, fmt, linewrap);
  free(nrand);

  return EXIT_SUCCESS;
}
