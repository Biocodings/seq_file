/*
 seq_reader.c
 project: seq_reader
 author: Isaac Turner <turner.isaac@gmail.com>
 Copyright (C) 20-June-2012
 
 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "seq_reader.h"
#include "sam.h"

#define is_base_char(x) ((x) == 'a' || (x) == 'A' || \
                         (x) == 'c' || (x) == 'C' || \
                         (x) == 'g' || (x) == 'G' || \
                         (x) == 't' || (x) == 'T' || \
                         (x) == 'n' || (x) == 'N')

int8_t seq_comp_table[16] = {0,8,4,12,2,10,9,14,1,6,5,13,3,11,7,15};

const char* seq_file_types[6]
  = {"Unknown", "FASTA", "FASTQ", "Plain", "SAM", "BAM"};

struct SequenceFile
{
  const char *path;

  gzFile *gz_file;

  // For reading sam/bams
  samfile_t *sam_file;
  bam1_t *bam;

  enum SequenceFileType file_type;
};

struct SequenceKmerReader
{
  SequenceFile* file;
  int kmer_size;

  // entry offset
  int offset;

  // have we seen a '>' at the start of a line in a fasta file?
  char read_line_start;

  // need to read in whole entry for sam/bam and fastq
  Sequence* whole_entry;
};

// For creating/destroying struct for result
Sequence* seq_init()
{
  Sequence* sequence = (Sequence*) malloc(sizeof(Sequence));

  sequence->name = string_buff_init(100);
  sequence->seq = string_buff_init(100);
  sequence->qual = string_buff_init(100);

  sequence->offset = 0;
  sequence->valid_qual = 0;

  return sequence;
}

void seq_destroy(Sequence* sequence)
{
  string_buff_free(sequence->name);
  string_buff_free(sequence->seq);
  string_buff_free(sequence->qual);
}

void _set_seq_filetype(SequenceFile* file)
{
  size_t path_len = strlen(file->path);

  if(path_len > 4)
  {
    if(strcasecmp(".sam", file->path+path_len - 4) == 0)
    {
      file->sam_file = samopen(file->path, "r", 0);
      file->bam = bam_init1();
      file->file_type = SEQ_SAM;
      return;
    }
    else if(strcasecmp(".bam", file->path+path_len - 4) == 0)
    {
      file->sam_file = samopen(file->path, "rb", 0);
      file->bam = bam_init1();
      file->file_type = SEQ_BAM;
      return;
    }
  }

  // Open file for the first time
  if(strcmp(file->path, "-") == 0)
  {
    file->gz_file = gzdopen(fileno(stdin), "r");
  }
  else
  {
    file->gz_file = gzopen(file->path, "r");
  }

  int first_char;

  do
  {
    first_char = gzgetc(file->gz_file);
  } while (first_char != -1 && (first_char == '\n' || first_char == '\r'));

  if(first_char == -1)
  {
    fprintf(stderr, "seq_reader warning: empty sequence file\n");
    return;
  }
  else if(first_char == '>')
  {
    // Reading FASTA
    file->file_type = SEQ_FASTA;
  }
  else if(first_char == '@')
  {
    // Reading FASTQ
    file->file_type = SEQ_FASTQ;
  }
  else if(is_base_char(first_char))
  {
    file->file_type = SEQ_PLAIN;
  }
  else
  {
    fprintf(stderr, "seq_reader warning: unknown filetype starting '%c'\n",
            first_char);
  }

  // Put char back
  if(gzungetc(first_char, file->gz_file) == -1)
  {
    fprintf(stderr, "seq_reader warning: error recovering from filetype check\n");
  }
}

SequenceFile* seq_file_open(const char* file_path)
{
  SequenceFile* file = (SequenceFile*) malloc(sizeof(SequenceFile));
  file->path = file_path;
  file->gz_file = NULL;
  file->file_type = SEQ_UNKNOWN;
  file->sam_file = NULL;
  file->bam = NULL;

  _set_seq_filetype(file);

  return file;
}

SequenceFile* seq_file_open_filetype(const char* file_path,
                                     const SequenceFileType file_type)
{
  SequenceFile* file = (SequenceFile*) malloc(sizeof(SequenceFile));
  file->path = file_path;
  file->gz_file = NULL;
  file->file_type = file_type;
  file->sam_file = NULL;
  file->bam = NULL;

  switch(file_type)
  {
    case SEQ_SAM:
      file->sam_file = samopen(file->path, "r", 0);
      file->bam = bam_init1();
      break;
    case SEQ_BAM:
      file->sam_file = samopen(file->path, "rb", 0);
      file->bam = bam_init1();
      break;
    case SEQ_FASTA:
    case SEQ_FASTQ:
    case SEQ_PLAIN:
      if(strcmp(file->path, "-") == 0)
      {
        file->gz_file = gzdopen(fileno(stdin), "r");
      }
      else
      {
        file->gz_file = gzopen(file->path, "r");
      }
      break;
    default:
      fprintf(stderr, "seq_reader.c - seq_file_open_filetype(): "
                      "invalid SequenceFileType\n");
      free(file);
      return NULL;
  }

  return file;
}


void seq_file_close(SequenceFile* file)
{
  if(file->gz_file != NULL)
  {
    gzclose(file->gz_file);
  }

  if(file->file_type == SEQ_SAM || file->file_type == SEQ_BAM)
  {
    bam_destroy1(file->bam);
    samclose(file->sam_file);
  }

  free(file);
}

SequenceFileType seq_file_get_type(const SequenceFile* file)
{
  return file->file_type;
}

const char* seq_file_get_type_str(const SequenceFile* file)
{
  return seq_file_types[file->file_type];
}

// Get a pointer to the file path
const char* seq_file_get_path(const SequenceFile* file)
{
  return file->path;
}

// Read an entry from a FASTQ file
// Returns 1 if a header is read, 0 otherwise
// Prints errors to stderr if there are syntax issues
char _read_fastq_entry(SequenceFile* file, Sequence* sequence)
{
  // Read until we have a header line
  int c;
  
  do
  {
    c = gzgetc(file->gz_file);
  } while(c != -1 && (c == '\n' || c == '\r'));

  if(c == -1)
  {
    return 0;
  }
  else if(c != '@')
  {
    fprintf(stderr, "Warning: FASTQ header does not begin with '@'\n");
  }

  int read_len;

  // Read name
  if((read_len = string_buff_gzreadline(sequence->name, file->gz_file)) == 0)
  {
    fprintf(stderr, "Warning: FASTQ missing name line (header: '%s')\n",
            sequence->name->buff);
  }

  string_buff_chomp(sequence->name);

  // Read sequence
  if((read_len = string_buff_gzreadline(sequence->seq, file->gz_file)) == 0)
  {
    fprintf(stderr, "Warning: FASTQ missing sequence line (header: '%s')\n",
            sequence->seq->buff);
  }

  string_buff_chomp(sequence->seq);

  // Read skip line ('+')
  // Use quality buffer as temp buff to read in skip line
  if((read_len = string_buff_gzreadline(sequence->qual, file->gz_file)) == 0 ||
     string_buff_get_char(sequence->qual, 0) != '+')
  {
    string_buff_chomp(sequence->qual);
    fprintf(stderr, "Warning: FASTQ skip line does not begin with '+' "
                    "(read name: '%s', skip line: '%s')\n",
            sequence->name->buff, sequence->qual->buff);
  }

  string_buff_reset(sequence->qual);

  // Now read quality line
  read_len = string_buff_gzreadline(sequence->qual, file->gz_file);
  string_buff_chomp(sequence->qual);

  if(read_len == 0)
  {
    fprintf(stderr, "Warning: FASTQ is missing a quality line\n");
  }
  else if(string_buff_strlen(sequence->qual) !=
          string_buff_strlen(sequence->seq))
  {
    fprintf(stderr, "Warning: FASTQ sequence and quality lines are not "
                    "the same length (read name: '%s')\n",
                    sequence->name->buff);
    fprintf(stderr, "Sequence: '%s'\n", sequence->seq->buff);
    fprintf(stderr, "Quality : '%s'\n", sequence->qual->buff);
  }
  else
  {
    sequence->valid_qual = 1;
  }

  return 1;
}

// Read an entry from a FASTA file
// Returns 1 if a header is read, 0 otherwise
char _read_fasta_entry(SequenceFile* file, Sequence* sequence)
{
  // Read until we have a header line
  int c;
  
  do
  {
    c = gzgetc(file->gz_file);
  } while(c != -1 && (c == '\n' || c == '\r'));

  if(c == -1)
  {
    return 0;
  }
  else if(c != '>')
  {
    fprintf(stderr, "seq_reader.c warning: FASTA header does not begin "
                    "with '>' (read name: '%s')\n", sequence->name->buff);
  }

  string_buff_gzreadline(sequence->name, file->gz_file);
  string_buff_chomp(sequence->name);

  while(1)
  {
    // Check line doesn't begin with '>'
    int first_char = gzgetc(file->gz_file);

    if(first_char == -1)
    {
      break;
    }
    else if(first_char == '>')
    {
      // Push char back onto buffer
      gzungetc(first_char, file->gz_file);

      // Done
      break;
    }
    else
    {
      // Push char onto string
      string_buff_append_char(sequence->seq, first_char);
      string_buff_chomp(sequence->seq);

      if(first_char != '\n' && first_char != '\r')
      {
        // Read the rest of the line
        if(string_buff_gzreadline(sequence->seq, file->gz_file) == 0)
        {
          break;
        }

        string_buff_chomp(sequence->seq);
      }
    }
  }

  sequence->valid_qual = 0;

  return 1;
}

// Read an entry from a SAM/BAM file
// Returns 1 if an entry is read, 0 otherwise
char _read_bam_entry(SequenceFile* file, Sequence* sequence)
{
  if(samread(file->sam_file, file->bam) >= 0)
  {
    uint8_t *seq; // samtools gives us seq pointer
    char* str;
    int i;

    int qlen = file->bam->core.l_qseq;

    // Get reverse
    char is_reversed = file->bam->core.flag & 16;

    //
    // Get name
    //
    string_buff_append_str(sequence->name, bam1_qname(file->bam));

    //
    // Get sequence
    //
    string_buff_ensure_capacity(sequence->seq, qlen);
    sequence->seq->len = qlen;
    str = sequence->seq->buff;
    str[qlen] = '\0';

    seq = bam1_seq(file->bam);

    // read in and complement (if needed)
    for(i = 0; i < qlen; i++)
    {
      int8_t b = bam1_seqi(seq, i);
      str[i] = bam_nt16_rev_table[is_reversed ? seq_comp_table[b] : b];
    }

    // reverse
    if(is_reversed)
    {
      string_buff_reverse(sequence->seq);
    }

    //
    // Get quality
    //
    string_buff_ensure_capacity(sequence->qual, qlen);
    sequence->qual->len = qlen;
    str = sequence->qual->buff;
    str[qlen] = '\0';

    seq = bam1_qual(file->bam);

    if(is_reversed)
    {
      // Read-in backwards to do reverse
      for (i = 0; i < qlen; i++)
        str[qlen - i - 1] = 33 + seq[i];
    }
    else
    {
      // Read-in forward
      for (i = 0; i < qlen; i++)
        str[i] = 33 + seq[i];
    }

    sequence->valid_qual = 0;

    // Check if the quality string has any values set    
    for (i = 0; i < qlen; i++)
    {
      if(str[i] != '?')
      {
        sequence->valid_qual = 1;
        break;
      }
    }

    return 1;
  }

  return 0;
}

// Read an entry from a SAM/BAM file
// Returns 1 if a line is read, 0 otherwise
char _read_plain_entry(SequenceFile* file, Sequence* sequence)
{
  t_buf_pos chars_read = string_buff_gzreadline(sequence->seq, file->gz_file);
  string_buff_chomp(sequence->seq);
  return (chars_read > 0);
}

// Returns 0 if at end of file; 1 otherwise
char seq_file_read(SequenceFile* file, Sequence* sequence)
{
  string_buff_reset(sequence->name);
  string_buff_reset(sequence->seq);
  string_buff_reset(sequence->qual);

  if(file->gz_file != NULL && gzeof(file->gz_file))
  {
    return 0;
  }

  char success;

  switch(file->file_type)
  {
    case SEQ_SAM:
    case SEQ_BAM:
      success = _read_bam_entry(file, sequence);
      break;
    case SEQ_FASTA:
      success = _read_fasta_entry(file, sequence);
      break;
    case SEQ_FASTQ:
      success = _read_fastq_entry(file, sequence);
      break;
    case SEQ_PLAIN:
      success = _read_plain_entry(file, sequence);
      break;
    default:
      fprintf(stderr, "seq_reader.c warning: "
                      "tried to read from unknown filetype\n");
      return 0;
  }

  sequence->offset = 0;

  return success;
}

// Kmer reader
SequenceKmerReader* seq_file_get_kmer_reader(SequenceFile* file, int kmer_size)
{
  SequenceKmerReader* reader
    = (SequenceKmerReader*) malloc(sizeof(SequenceKmerReader));

  reader->file = file;
  reader->kmer_size = kmer_size;
  reader->offset = 0;
  reader->read_line_start = 0;

  switch(file->file_type)
  {
    case SEQ_SAM:
    case SEQ_BAM:
    case SEQ_FASTQ:
      reader->whole_entry = seq_init();
    default:
      break;
  }

  return reader;
}

void _reset_sequence(Sequence* sequence)
{
  string_buff_reset(sequence->name);
  string_buff_reset(sequence->seq);
  string_buff_reset(sequence->qual);
  sequence->offset = 0;
}

void _shift_insert_char(STRING_BUFFER* str, char c)
{
  memmove(str->buff, str->buff+1, str->len-1);
  str->buff[str->len-1] = c;
}

// Read an entry from a FASTA file
// Returns 1 if a header is read, 0 otherwise
char _read_fasta_entry_kmer(SequenceKmerReader* reader, Sequence* sequence)
{
  size_t kmer_size = reader->kmer_size;
  int c;

  if(reader->read_line_start == 1)
  {
    c = '>';
  }
  else
  {
    while((c = gzgetc(reader->file->gz_file)) != -1 && (c == '\n' || c == '\r'));

    if(c == -1)
    {
      _reset_sequence(sequence);
      return 0;
    }
  }

  if(string_buff_strlen(sequence->seq) == kmer_size && c != '>')
  {
    // Append
    _shift_insert_char(sequence->seq, (char)c);
    reader->offset++;
  }
  else
  {
    // Read new
    if(c != '>')
    {
      fprintf(stderr, "seq_reader.c - read_fasta_entry_kmer(): expected "
                      "line to start '>'");
      _reset_sequence(sequence);
      return 0;
    }

    _reset_sequence(sequence);

    reader->offset = 0;
    reader->read_line_start = 0;

    // Read name
    string_buff_gzreadline(sequence->name, reader->file->gz_file);
    string_buff_chomp(sequence->name);

    // Read sequence
    c = gzgetc(reader->file->gz_file);
    
    if(c == -1)
    {
      // No sequence with name, but still success
      return 1;
    }
    else if(c == '>')
    {
      // No sequence with name, but still success
      reader->read_line_start = 1;
      return 1;
    }
    else
    {
      string_buff_append_char(sequence->seq, c);
    }

    t_buf_pos remaining_bases;

    while((remaining_bases = kmer_size - string_buff_strlen(sequence->seq)) > 0)
    {
      string_buff_gzread(sequence->seq, reader->file->gz_file, remaining_bases);
      string_buff_chomp(sequence->name);
    }
  }

  sequence->offset = reader->offset;

  return 1;
}

// Read an entry from a SAM/BAM file
// Returns 1 if a line is read, 0 otherwise
char _read_plain_entry_kmer(SequenceKmerReader* reader, Sequence* sequence)
{
  // Plain is one per sequence
  // Read a single char
  int c;
  
  if((c = gzgetc(reader->file->gz_file)) != -1)
  {
    _reset_sequence(sequence);
    return 0;
  }

  if(string_buff_strlen(sequence->seq) == reader->kmer_size &&
     c != '\n' && c != '\r')
  {
    // Append
    _shift_insert_char(sequence->seq, (char)c);
    reader->offset++;
  }
  else
  {
    // Read new
    _reset_sequence(sequence);
    reader->offset = 0;

    while(string_buff_strlen(sequence->seq) < reader->kmer_size)
    {
      if(string_buff_gzread(sequence->seq,
                            reader->file->gz_file,
                            reader->kmer_size) == 0)
      {
        // No characters read
        _reset_sequence(sequence);
        return 0;
      }

      string_buff_chomp(sequence->seq);
    }
  }

  sequence->offset = reader->offset;

  return 1;
}

// Read SAM/BAM or FASTQ
char _read_whole_entry_kmer(SequenceKmerReader* reader, Sequence* sequence)
{
  // Need to read in a whole sequence for these ones
  char read_new_entry = 0;

  if(string_buff_strlen(reader->whole_entry->seq) > 0)
  {
    // If we've read in a sequence, advance the offset
    reader->offset++;
  }

  while(reader->offset + reader->kmer_size >
        string_buff_strlen(reader->whole_entry->seq))
  {
    _reset_sequence(reader->whole_entry);

    reader->offset = 0;

    if(reader->file->file_type == SEQ_FASTQ)
    {
      read_new_entry = _read_fastq_entry(reader->file, reader->whole_entry);
    }
    else
    {
      // SAM or BAM
      read_new_entry = _read_bam_entry(reader->file, reader->whole_entry);
    }

    if(!read_new_entry)
    {
      _reset_sequence(sequence);
      return 0;
    }
  }

  if(read_new_entry)
  {
    _reset_sequence(sequence);

    // copy name
    string_buff_copy(sequence->name, 0, reader->whole_entry->name, 0,
                     string_buff_strlen(reader->whole_entry->name));
    // copy sequence
    string_buff_copy(sequence->seq, 0, reader->whole_entry->seq, 0,
                     reader->kmer_size);
    // copy quality
    if(string_buff_strlen(reader->whole_entry->qual) > 0)
    {
      string_buff_copy(sequence->qual, 0, reader->whole_entry->qual, 0,
                       reader->kmer_size);
    }
    return 1;
  }
  else
  {
    // append
    t_buf_pos pos = reader->offset + reader->kmer_size - 1;
    char next;

    next = string_buff_get_char(reader->whole_entry->seq, pos);
    _shift_insert_char(sequence->seq, next);

    next = string_buff_get_char(reader->whole_entry->qual, pos);
    _shift_insert_char(sequence->qual, next);

    sequence->offset = reader->offset;
    return 1;
  }
}

// Returns 0 if at end of file; 1 otherwise
char seq_file_read_kmer(SequenceKmerReader* reader, Sequence* sequence)
{
  SequenceFileType file_type = reader->file->file_type;

  if(file_type == SEQ_SAM || file_type == SEQ_BAM || file_type == SEQ_FASTQ)
  {
    return _read_whole_entry_kmer(reader, sequence);
  }
  else if(file_type == SEQ_FASTA)
  {
    return _read_fasta_entry_kmer(reader, sequence);
  }
  else if(file_type == SEQ_PLAIN)
  {
    return _read_plain_entry_kmer(reader, sequence);
  }
  else
  {
    fprintf(stderr, "seq_reader.c warning: "
                      "tried to read from unknown filetype\n");
    return 0;
  }
}
