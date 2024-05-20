#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

struct rlz_encoding_packet {
  char * outbuf;
  char * inbuf;
  size_t inbyte;
  size_t outbyte;
};

char * progname;
unsigned int maxthrd_count = 4;
size_t blksize = sizeof(size_t) + sizeof(char);

ssize_t filesize(FILE *);
ssize_t rlz_encode(char *, char *, size_t);
void * rlz_encode_subroutine(void *);
size_t rlz_decode(FILE *, char *, size_t);
void errexit(const char *restrict, ...) __attribute__ ((__noreturn__));

int main(int argc, char * argv[])
{
  bool decomp;
  char * outfname, * infname, * strp, * inbuf, * outbuf;
  int opt_index, option;
  size_t nbyte;
  ssize_t finsize, foutsize;
  FILE * fpin, * fpout;

  static struct option long_options[] = {
    {"compress", no_argument, NULL, 'c'},
    {"decompress", no_argument, NULL, 'd'},
    {"jobs", required_argument, NULL, 'j'},
    {"help", no_argument, NULL, 'h'},
    {NULL, 0, NULL, 0}
  };

  outfname = NULL;
  decomp = false;
  progname = argv[0];
  errno = 0;
  opterr = 0;
  opt_index = 0;

  if (argc == 1)
    errexit("%s: invalid number of arguments\n", argv[0]);

  while ((option = getopt_long(argc, argv, "dj:", long_options, &opt_index)) != -1)
  {
    switch (option)
    {
      case 'h':
        printf("Usage: %s [OPTION]... [FILE]\n", argv[0]);
        exit(EXIT_SUCCESS);
        break;
      case 'c':
        break;
      case 'd':
        decomp = true;
        break;
      case 'j':
        maxthrd_count = strtol(optarg, &strp, 0);
        if (*strp != '\0')
          errexit("%s: invalid argument - '%s'\n", progname, optarg);
        break;
      case '?':
        errexit("%s: unknown option: '%s'\n", progname, argv[optind - 1]);
      default:
        errexit("%s: unexpected error occurred\n", progname);
    }
  }

  if (optind == argc)
    errexit("%s: missing input file\n", progname);

  infname = argv[optind];

  outfname = malloc(NAME_MAX + 1);
  if (outfname == NULL)
    errexit("%s: %s\n", progname, strerror(errno));

  if (decomp)
  {
    outfname[NAME_MAX] = '\0';
    strp = stpncpy(outfname, infname, NAME_MAX);

    if ((outfname + 4 >= strp) || (strcmp(strp - 4, ".rlz") != 0))
      errexit("%s: invalid input file \n", progname);

    *(strp - 4) = '\0';
  }
  else
  {
    strp = stpncpy(outfname, argv[1], NAME_MAX - 4);
    strcpy(strp, ".rlz");
  }

  fpin = fopen(infname, "r");
  if (fpin == NULL)
    errexit("%s: %s\n", progname, strerror(errno));

  fpout = fopen(outfname, "w");
  if (fpout == NULL)
    errexit("%s: %s\n", progname, strerror(errno));

  free(outfname);

  finsize = filesize(fpin);
  if (finsize == -1)
    errexit("%s: %s\n", progname, strerror(errno));
  foutsize = (!decomp) ? blksize * finsize : 0;

  if (finsize == 0)
    goto cleanup;

  inbuf = malloc(finsize + foutsize);
  if (inbuf == NULL)
    errexit("%s: %s\n", progname, strerror(errno));

  outbuf = inbuf + finsize;

  nbyte = fread(inbuf, 1, finsize, fpin);

  if (!decomp)
  {
    foutsize = rlz_encode(outbuf, inbuf, nbyte);
    if (foutsize == -1)
      errexit("%s: unexpected error occurred\n", progname);

    nbyte = (size_t) foutsize;
    fwrite(outbuf, 1, nbyte, fpout);
  }
  else
  {
    rlz_decode(fpout, inbuf, nbyte);
  }

  free(inbuf);

cleanup:
  fclose(fpin);
  fclose(fpout);

  return 0;
}

ssize_t filesize(FILE * fp)
{
  struct stat sb;

  if (fstat(fileno(fp), &sb) == -1)
    return -1;

  return sb.st_size;
}

ssize_t rlz_encode(char * outbuf, char * inbuf, size_t inbyte)
{
  char * curr;
  int status;
  unsigned int i;
  pthread_t * thread_arena;
  size_t chunk_size;
  ptrdiff_t nbyte;
  struct rlz_encoding_packet * packets;

  thread_arena = malloc(sizeof(pthread_t) * maxthrd_count);
  if (thread_arena == NULL)
    return -1;

  packets = malloc(sizeof(struct rlz_encoding_packet) * maxthrd_count);
  if (packets == NULL)
  {
    free(thread_arena);
    return -1;
  }

  chunk_size = inbyte / maxthrd_count;

  for (i = 0; i < maxthrd_count - 1; i++)
  {
    packets[i].inbuf = inbuf + chunk_size * i;
    packets[i].inbyte = chunk_size;
    packets[i].outbuf = outbuf + blksize * chunk_size * i;
  }

  // for the last remaining packet
  packets[i].inbuf = inbuf + chunk_size * i;
  packets[i].inbyte = inbyte - chunk_size * i;
  packets[i].outbuf = outbuf + blksize * chunk_size * i;

  for (i = 0; i < maxthrd_count; i++)
  {
    status = pthread_create(&thread_arena[i], NULL, rlz_encode_subroutine, &packets[i]);
    if (status != 0)    // error critical
    {
      free(thread_arena);
      free(packets);
      return -1;
    }
  }

  for (i = 0; i < maxthrd_count; i++)
  {
    status = pthread_join(thread_arena[i], NULL);
    if (status != 0)    // error critical
    {
      free(thread_arena);
      free(packets);
      return -1;
    }
  }

  // the hard part
  curr = outbuf + packets[0].outbyte;
  for (i = 0; i < maxthrd_count - 1; i++)
  {
    if (*(curr - 1) != *(curr + sizeof(size_t)))
    {
      memmove(curr, packets[i + 1].outbuf, packets[i + 1].outbyte);
      curr += packets[i + 1].outbyte;
      continue;
    }

    *(size_t *)(curr - blksize) += *(size_t *)(packets[i + 1].outbuf);

    if (blksize != packets[i + 1].outbyte)
    {
      // type of ... a || a b a |
      memmove(curr, packets[i + 1].outbuf + blksize, packets[i + 1].outbyte - blksize);
      curr += packets[i + 1].outbyte - blksize;
    }
  }

  nbyte = curr - packets[0].outbuf;

  free(thread_arena);
  free(packets);

  return (ssize_t) nbyte;
}

void * rlz_encode_subroutine(void * arg)
{
  char cur, * outbuf, * inbuf;
  size_t inbyte, outbyte, i, count;
  struct rlz_encoding_packet * packet;

  packet = (struct rlz_encoding_packet *) arg;

  inbuf = packet->inbuf;
  outbuf = packet->outbuf;
  inbyte = packet->inbyte;

  outbyte = 0;
  cur = inbuf[0];
  count = 1;

  for (i = 1; i < inbyte; i++)
  {
    if (inbuf[i] != cur)
    {
      *((size_t *) outbuf) = count;
      *(outbuf + sizeof(size_t)) = cur;

      outbuf = outbuf + blksize;
      outbyte += blksize;

      cur = inbuf[i];
      count = 1;
      continue;
    }

    count++;
  }

  *((size_t *) outbuf) = count;
  *(outbuf + sizeof(size_t)) = cur;
  outbyte += blksize;

  packet->outbyte = outbyte;

  return (void *) outbyte;
}

size_t rlz_decode(FILE * fp, char * inbuf, size_t inbyte)
{
  char cur;
  size_t nelem, i, count;

  nelem = inbyte / blksize;

  for (i = 0; i < nelem; i++)
  {
    count = *((size_t *) inbuf);
    cur = *(inbuf + sizeof(size_t));
    inbuf += blksize;

    while (count--)
      putc(cur, fp);
  }

  return nelem;
}

void errexit(const char *restrict format, ...)
{
  va_list ap;

  va_start(ap, format);
  vfprintf(stderr, format, ap);
  va_end(ap);

  exit(EXIT_FAILURE);
}
