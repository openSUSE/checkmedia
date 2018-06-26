#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdint.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "md5.h"
#include "sha1.h"
#include "sha256.h"
#include "sha512.h"

// exported symbol - all others are not exported by the library
#define API_SYM __attribute__((visibility("default")))

#define MAX_DIGEST_SIZE SHA512_DIGEST_SIZE

typedef enum {
  digest_none, digest_md5, digest_sha1, digest_sha224, digest_sha256, digest_sha384, digest_sha512
} digest_type_t;

typedef union {
  struct md5_ctx md5;
  struct sha1_ctx sha1;
  struct sha256_ctx sha224;
  struct sha256_ctx sha256;
  struct sha512_ctx sha384;
  struct sha512_ctx sha512;
} digest_ctx_t;

typedef struct mediacheck_digest_s {
  digest_type_t type;				/* digest type */
  char *name;					/* digest name */
  int size;					/* (binary) digest size, not bigger than MAX_DIGEST_SIZE */
  unsigned valid:1;				/* struct holds valid digest data */
  unsigned ok:1;				/* data[] and ref[] match */
  unsigned ctx_init:1;				/* ctx has been initialized */
  unsigned finished:1;				/* digest_finish() has been callled */
  digest_ctx_t ctx;				/* digest context */
  unsigned char data[MAX_DIGEST_SIZE];		/* binary digest */
  char hex[MAX_DIGEST_SIZE*2 + 1];		/* hex digest */
  unsigned char ref[MAX_DIGEST_SIZE];		/* expected binary digest */
  char hex_ref[MAX_DIGEST_SIZE*2 + 1];		/* expected hex digest */
} mediacheck_digest_t;

typedef struct {
  unsigned start, blocks;
} chunk_region_t;

#include "mediacheck.h"

static void digest_ctx_init(mediacheck_digest_t *digest);
static void digest_finish(mediacheck_digest_t *digest);
static void digest_data_to_hex(mediacheck_digest_t *digest);
static void get_info(mediacheck_t *media);
static int sanitize_data(char *data, int length);
static char *no_extra_spaces(char *str);
static void update_progress(mediacheck_t *media, unsigned blocks);
static void process_chunk(mediacheck_digest_t *digest, chunk_region_t *region, unsigned chunk, unsigned chunk_blocks, unsigned char *buffer);


/*
 * Read image file and gather info about it.
 *
 * Returns pointer to mediacheck_t structure filled with data.
 *
 * Use mediacheck_done() to free it.
 */
API_SYM mediacheck_t * mediacheck_init(char *file_name, mediacheck_progress_t progress)
{
  mediacheck_t *media = calloc(1, sizeof *media);

  media->last_percent = -1;
  media->file_name = file_name;
  media->progress = progress;

  media->digest.iso = calloc(1, sizeof *media->digest.iso);
  media->digest.part = calloc(1, sizeof *media->digest.part);
  media->digest.full = calloc(1, sizeof *media->digest.full);

  get_info(media);

  return media;
}


/*
 * Free mediacheck_t structure.
 */
API_SYM void mediacheck_done(mediacheck_t *media)
{
  int i;

  if(!media) return;

  for(i = 0; i < sizeof media->tags / sizeof *media->tags; i++) {
    if(!media->tags[i].key) break;
    free(media->tags[i].key);
    free(media->tags[i].value);
  }

  free(media->digest.iso);
  free(media->digest.part);
  free(media->digest.full);

  free(media);
}


/*
 * Calculate digest over image.
 *
 * Call mediacheck_init() before doing this.
 *
 * Normal digest, except that we assume
 *   - 0x0000 - 0x01ff is filled with zeros (0)
 *   - 0x8373 - 0x8572 is filled with spaces (' ').
 */
API_SYM void mediacheck_calculate_digest(mediacheck_t *media)
{
  unsigned char buffer[64 << 10];	/* arbitrary, but at least 36 kB! */
  unsigned chunk_size = sizeof buffer;
  unsigned chunk_blocks = chunk_size >> 9;
  unsigned last_chunk, last_chunk_blocks;
  unsigned chunk;
  int fd;

  chunk_region_t full_region = { 0, media->full_blocks } ;
  chunk_region_t iso_region = { 0, media->iso_blocks - media->pad_blocks } ;
  chunk_region_t part_region = { media->part_start, media->part_blocks } ;

  last_chunk = media->full_blocks / chunk_blocks;
  last_chunk_blocks = media->full_blocks % chunk_blocks;

  if(!media || !media->file_name) return;

  if((fd = open(media->file_name, O_RDONLY | O_LARGEFILE)) == -1) return;

  update_progress(media, 0);

  mediacheck_digest_init(media->digest.full, media->digest.iso->name ?: media->digest.part->name, NULL);

  for(chunk = 0; chunk <= last_chunk; chunk++) {
    unsigned u, size = chunk_size;

    if(media->abort) break;

    if(chunk == last_chunk) size = last_chunk_blocks << 9;

    if((u = read(fd, buffer, size)) != size) {
      media->err = 1;
      if(u > size) u = 0 ;
      media->err_ofs = (u >> 9) + chunk * chunk_blocks;
      break;
    };

    /*
     * The full digest should give the digest over the real file, without
     * any adjustments. So do it before manipulating the buffer.
     */
    process_chunk(media->digest.full, &full_region, chunk, chunk_blocks, buffer);

    if(chunk == 0) {
      memset(buffer, 0, 0x200);
      memset(buffer + 0x8373, ' ', 0x200);
    }

    process_chunk(media->digest.iso, &iso_region, chunk, chunk_blocks, buffer);
    process_chunk(media->digest.part, &part_region, chunk, chunk_blocks, buffer);

    update_progress(media, (chunk + 1) * chunk_blocks);
  }

  if(!media->err && !media->abort) {
    unsigned u;

    memset(buffer, 0, 1 << 9);		/* 0.5 kB */
    for(u = 0; u < media->pad_blocks; u++) {
      mediacheck_digest_process(media->digest.iso, buffer, 1 << 9);
    }
  }

  if(!media->abort) update_progress(media, media->full_blocks);

  if(media->err) {
    media->digest.iso->valid = 0;
    media->digest.part->valid = 0;
    media->digest.full->valid = 0;
  }

  close(fd);
}


/*
 * Initialize digest struct with hex value.
 *
 * digest: digest struct
 * digest_name: digest name
 * digest_value: expected digest value as hex string
 *
 * return: 1 if valid, else 0
 *
 * If digest_name is NULL it is inferred from digest_value length.
 * If digest_value is NULL, no value is set.
 */
API_SYM int mediacheck_digest_init(mediacheck_digest_t *digest, char *digest_name, char *digest_value)
{
  unsigned u;
  int i, digest_by_name = 0, digest_by_size = 0;

  static struct {
    digest_type_t type;
    char *name;
    int size;
  } digests[] = {
    { digest_none, "", 0 },
    { digest_md5, "md5", MD5_DIGEST_SIZE },
    { digest_sha1, "sha1", SHA1_DIGEST_SIZE },
    { digest_sha224, "sha224", SHA224_DIGEST_SIZE },
    { digest_sha256, "sha256", SHA256_DIGEST_SIZE },
    { digest_sha384, "sha384", SHA384_DIGEST_SIZE },
    { digest_sha512, "sha512", SHA512_DIGEST_SIZE },
  };

  if(!digest) return 0;

  memset(digest, 0, sizeof *digest);

  if(digest_name) {
    for(i = 0; i < sizeof digests / sizeof *digests; i++) {
      if(!strcasecmp(digests[i].name, digest_name)) {
        digest_by_name = i;
        break;
      }
    }
  }

  if(digest_value) {
    int size = strlen(digest_value);

    for(i = 0; i < sizeof digests / sizeof *digests; i++) {
      if(size == digests[i].size * 2) {
        digest_by_size = i;
        break;
      }
    }
  }

  // invalid
  if(!digest_by_name && !digest_by_size) return 0;

  // inconsistency
  if(digest_by_name && digest_by_size && digest_by_name != digest_by_size) return 0;

  i = digest_by_name ?: digest_by_size;

  digest->type = digests[i].type;
  digest->size = digests[i].size;
  digest->name = digests[i].name;

  if(digest_value) {
    for(i = 0; i < digest->size; i++, digest_value += 2) {
      if(sscanf(digest_value, "%2x", &u) == 1) {
        digest->ref[i] = u;
      }
      else {
        return 0;
      }
    }
  }

  digest->valid = 1;

  digest_data_to_hex(digest);

  return 1;
}


/*
 * This function does the actual digest calculation.
 */
API_SYM void mediacheck_digest_process(mediacheck_digest_t *digest, unsigned char *buffer, unsigned len)
{
  if(!digest->ctx_init) digest_ctx_init(digest);

  switch(digest->type) {
    case digest_md5:
      md5_process_bytes(buffer, len, &digest->ctx.md5);
      break;
    case digest_sha1:
      sha1_process_bytes(buffer, len, &digest->ctx.sha1);
      break;
    case digest_sha224:
      sha256_process_bytes(buffer, len, &digest->ctx.sha224);
      break;
    case digest_sha256:
      sha256_process_bytes(buffer, len, &digest->ctx.sha256);
      break;
    case digest_sha384:
      sha512_process_bytes(buffer, len, &digest->ctx.sha384);
      break;
    case digest_sha512:
      sha512_process_bytes(buffer, len, &digest->ctx.sha512);
      break;
    default:
      break;
  }
}


/*
 * Check if digest holds valid data.
 */
API_SYM int mediacheck_digest_valid(mediacheck_digest_t *digest)
{
  return digest ? digest->valid : 0;
}


/*
 * Check if digest matches expected value.
 */
API_SYM int mediacheck_digest_ok(mediacheck_digest_t *digest)
{
  if(!digest) return 0;

  if(!digest->finished) digest_finish(digest);

  return digest->ok;
}


/*
 * Return digest name.
 */
API_SYM char *mediacheck_digest_name(mediacheck_digest_t *digest)
{
  return digest ? digest->name : "";
}


/*
 * Return digest as hex string.
 */
API_SYM char *mediacheck_digest_hex(mediacheck_digest_t *digest)
{
  if(!digest) return "";

  if(!digest->finished) digest_finish(digest);

  return digest->hex;
}


/*
 * Return expected digest as hex string.
 */
API_SYM char *mediacheck_digest_hex_ref(mediacheck_digest_t *digest)
{
  return digest ? digest->hex_ref : "";
}


/*
 * Free digest resources.
 *
 * (Does nothing currently.)
 */
API_SYM void mediacheck_digest_done(mediacheck_digest_t *digest)
{
  return;
}


/*
 * This function must be called to start the digest calculation.
 *
 * It is implicitly called in digest_process() if needed.
 */
void digest_ctx_init(mediacheck_digest_t *digest)
{
  switch(digest->type) {
    case digest_md5:
      md5_init_ctx(&digest->ctx.md5);
      break;
    case digest_sha1:
      sha1_init_ctx(&digest->ctx.sha1);
      break;
    case digest_sha224:
      sha224_init_ctx(&digest->ctx.sha224);
      break;
    case digest_sha256:
      sha256_init_ctx(&digest->ctx.sha256);
      break;
    case digest_sha384:
      sha384_init_ctx(&digest->ctx.sha384);
      break;
    case digest_sha512:
      sha512_init_ctx(&digest->ctx.sha512);
      break;
    default:
      break;
  }

  digest->ctx_init = 1;
}


/*
 * This function must be called to finalize the digest calculation.
 *
 * It is implicitly called if needed (the digest value is accessed).
 *
 * If a reference value has been provided the digest is compared with it.
 */
void digest_finish(mediacheck_digest_t *digest)
{
  if(!digest->ctx_init) digest_ctx_init(digest);

  switch(digest->type) {
    case digest_md5:
      md5_finish_ctx(&digest->ctx.md5, digest->data);
      break;
    case digest_sha1:
      sha1_finish_ctx(&digest->ctx.sha1, digest->data);
      break;
    case digest_sha224:
      sha224_finish_ctx(&digest->ctx.sha224, digest->data);
      break;
    case digest_sha256:
      sha256_finish_ctx(&digest->ctx.sha256, digest->data);
      break;
    case digest_sha384:
      sha384_finish_ctx(&digest->ctx.sha384, digest->data);
      break;
    case digest_sha512:
      sha512_finish_ctx(&digest->ctx.sha512, digest->data);
      break;
    default:
      break;
  }

  digest->ctx_init = 0;

  if(digest->type != digest_none) {
    digest->ok = memcmp(digest->data, digest->ref, digest->size) ? 0 : 1;
  }

  digest_data_to_hex(digest);

  digest->finished = 1;
}


/*
 * Convert digest binary data to hex string.
 */
void digest_data_to_hex(mediacheck_digest_t *digest)
{
  int i;

  if(!digest) return;

  digest->hex[0] = 0;

  if(digest->type == digest_none || !digest->valid) return;

  for(i = 0; i < digest->size; i++) {
    sprintf(digest->hex + 2*i, "%02x", digest->data[i]);
    sprintf(digest->hex_ref + 2*i, "%02x", digest->ref[i]);
  }

  digest->hex[2*i] = 0;
  digest->hex_ref[2*i] = 0;
}


/*
 * Read iso header and fill global iso struct.
 *
 * Note: checksum info is kept in media->app_data[].
 *
 * If in doubt about the ISO9660 layout, check http://alumnus.caltech.edu/~pje/iso9660.html
 */
void get_info(mediacheck_t *media)
{
  int fd, ok = 0, tag_count = 0, is_iso = 0;
  unsigned char buf[8];
  char *key, *value, *next;
  struct stat sb;

  media->err = 1;

  if(!media->file_name) return;

  if((fd = open(media->file_name, O_RDONLY | O_LARGEFILE)) == -1) return;

  if(!fstat(fd, &sb) && S_ISREG(sb.st_mode)) {
    media->full_blocks = sb.st_size >> 9;
  }

  /*
   * Check for ISO9660 magic.
   */
  if(
    lseek(fd, 0x8000, SEEK_SET) == 0x8000 &&
    read(fd, buf, 8) == 8
  ) {
    // yes, 8 bytes
    if(!memcmp(buf, "\001CD001\001", 8)) is_iso = 1;
  }

  /*
   * ISO size is stored as both little- and big-endian values.
   *
   * Read both and compare as consistency check.
   */
  if(
    lseek(fd, 0x8050, SEEK_SET) == 0x8050 &&
    read(fd, buf, 8) == 8
  ) {
    unsigned little = 4*(buf[0] + (buf[1] << 8) + (buf[2] << 16) + (buf[3] << 24));
    unsigned big = 4*(buf[7] + (buf[6] << 8) + (buf[5] << 16) + (buf[4] << 24));

    if(is_iso && little && little == big) {
      media->iso_blocks = little;
    }
  }

  /*
   * Application id is set to some string identifying the media.
   *
   * Read it and show it to the user later.
   */
  if(
    lseek(fd, 0x823e, SEEK_SET) == 0x823e &&
    read(fd, media->app_id, sizeof media->app_id - 1) == sizeof media->app_id - 1
  ) {
    media->app_id[sizeof media->app_id - 1] = 0;
    if(sanitize_data(media->app_id, sizeof media->app_id - 1)) {
      char *s;

      if(!strncmp(media->app_id, "MKISOFS", sizeof "MKISOFS" - 1)) *media->app_id = 0;
      if(!strncmp(media->app_id, "GENISOIMAGE", sizeof "GENISOIMAGE" - 1)) *media->app_id = 0;
      if((s = strrchr(media->app_id, '#'))) *s = 0;

      ok++;
    }
  }

  /*
   * Read application specific data block.
   *
   * We use it to store the digests over the medium.
   *
   * Read now and parse it later.
   */
  if(
    lseek(fd, 0x8373, SEEK_SET) == 0x8373 &&
    read(fd, media->app_data, sizeof media->app_data - 1) == sizeof media->app_data - 1
  ) {
    media->app_data[sizeof media->app_data - 1] = 0;
    if(sanitize_data(media->app_data, sizeof media->app_data - 1)) ok++;
  }

  close(fd);

  if(ok != 2) return;

  media->err = 0;

  for(key = next = media->app_data; next; key = next) {
    value = strchr(key, '=');
    next = strchr(key, ';');
    if(value && next && value > next) value = NULL;

    if(value) *value++ = 0;
    if(next) *next++ = 0;

    key = no_extra_spaces(key);
    value = no_extra_spaces(value);

    if(tag_count < sizeof media->tags / sizeof *media->tags) {
      media->tags[tag_count].key = strdup(key);
      media->tags[tag_count].value = strdup(value ?: "");
      tag_count++;
    }

    if(
      !strcasecmp(key, "md5sum") ||
      !strcasecmp(key, "sha1sum") ||
      !strcasecmp(key, "sha224sum") ||
      !strcasecmp(key, "sha256sum") ||
      !strcasecmp(key, "sha384sum") ||
      !strcasecmp(key, "sha512sum")
    ) {
      if(media->iso_blocks) {
        mediacheck_digest_init(media->digest.iso, NULL, value);
      }
    }
    else if(!strcasecmp(key, "partition")) {
      if(value && isdigit(*value)) {
        unsigned start = strtoul(value, &value, 0);
        if(*value++ == ',') {
          unsigned blocks = strtoul(value, &value, 0);
          if(*value++ == ',' && blocks) {
            media->part_start = start;
            media->part_blocks = blocks;

            mediacheck_digest_init(media->digest.part, NULL, value);
          }
        }
      }
    }
    else if(!strcasecmp(key, "pad")) {
      if(value && isdigit(*value)) {
        media->pad_blocks = strtoul(value, NULL, 0) << 2;
      }
    }
  }

  // if we didn't get the image size via stat() above, try other ways
  if(!media->full_blocks) {
    media->full_blocks = media->part_start + media->part_blocks;
    if(!media->full_blocks) media->full_blocks = media->iso_blocks;
  }
}


/*
 * Do basic validation on the data and cut off trailing spcaes.
 */
int sanitize_data(char *data, int length)
{
  char *s;

  for(s = data + length; s >= data; *s-- = 0) {
    if(*s != 0 && *s != ' ') break;
  }

  // printable ascii
  for(s = data; *s; s++) {
    if(*s < 0x20) return 0;
  }

  return 1;
}


/*
 * Helper function to remove leading & trailing spaces.
 *
 * Modifies str!
 */
char *no_extra_spaces(char *str)
{
  int i;

  if(str) {
    while(isspace(*str)) str++;
    if((i = strlen(str))) {
      while(i && isspace(str[i-1])) str[--i] = 0;
    }
  }

  return str;
}


/*
 * Update progress indicator.
 */
void update_progress(mediacheck_t *media, unsigned blocks)
{
  int percent;

  if(!media->full_blocks) {
    percent = 100;
  }
  else {
    percent = (blocks * 100) / media->full_blocks;
    if(percent > 100) percent = 100;
  }

  if(percent != media->last_percent) {
    media->last_percent = percent;
    if(media->progress) {
      media->abort = media->progress(percent);
    }
  }
}


/*
 * Process digest of a single chunk.
 *
 * digest: pointer to digest struct
 * region: pointer to region (start and size of area) over which to calculate digest
 * chunk: current chunk (counted 0-based)
 * chunk_blocks: chunk size in blocks (0.5 kB)
 * buffer: chunk_blocks sized buffer
 *
 * Start and end of the area may not be aligned with chunks. So we need
 * some calculations.
 */
void process_chunk(mediacheck_digest_t *digest, chunk_region_t *region, unsigned chunk, unsigned chunk_blocks, unsigned char *buffer)
{
  unsigned first_chunk = region->start / chunk_blocks;
  if(chunk < first_chunk) return;

  unsigned last_chunk = (region->start + region->blocks) / chunk_blocks;
  if(chunk > last_chunk) return;

  unsigned first_ofs = region->start % chunk_blocks;
  unsigned first_len = chunk_blocks - first_ofs;

  unsigned last_ofs = 0;
  unsigned last_len = (region->start + region->blocks) % chunk_blocks;

  if(last_chunk == first_chunk) {
    first_len = last_len - first_ofs;
  }

  if(chunk == first_chunk) {
    mediacheck_digest_process(digest, buffer + (first_ofs << 9), first_len << 9);
  }
  else if(chunk == last_chunk) {
    mediacheck_digest_process(digest, buffer + (last_ofs << 9), last_len << 9);
  }
  else {
    mediacheck_digest_process(digest, buffer, chunk_blocks << 9);
  }
}
