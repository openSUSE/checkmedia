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

// exported symbol - all others are not exported by the library
#define API_SYM __attribute__((visibility("default")))

#include "mediacheck.h"

static void get_info(mediacheck_t *media);
static unsigned set_digest(mediacheck_t *media, char *value, unsigned char *buf);
static int sanitize_data(char *data, int length);
static char *no_extra_spaces(char *str);
static void update_progress(mediacheck_t *media, unsigned size);

static void digest_media_init(mediacheck_t *media, digest_ctx_t *ctx);
static void digest_media_process(mediacheck_t *media, digest_ctx_t *ctx, unsigned char *buffer, unsigned len);
static void digest_media_finish(mediacheck_t *media, digest_ctx_t *ctx, unsigned char *buffer);


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
  unsigned char buffer[64 << 10]; /* at least 36k! */
  int fd, err = 0;
  unsigned chunks = (media->iso_size - media->pad) / (sizeof buffer >> 10);
  unsigned chunk, u;
  unsigned last_size = ((media->iso_size - media->pad) % (sizeof buffer >> 10)) << 10;

  if(!media || !media->file_name) return;
  
  if((fd = open(media->file_name, O_RDONLY | O_LARGEFILE)) == -1) return;

  update_progress(media, 0);

  digest_media_init(media, &media->digest.iso_ctx);
  digest_media_init(media, &media->digest.part_ctx);
  digest_media_init(media, &media->digest.full_ctx);

  for(chunk = 0; chunk < chunks; chunk++) {
    if((u = read(fd, buffer, sizeof buffer)) != sizeof buffer) {
      err = 1;
      if(u > sizeof buffer) u = 0 ;
      media->err_ofs = (u >> 10) + chunk * (sizeof buffer >> 10);
      break;
    };

    digest_media_process(media, &media->digest.full_ctx, buffer, sizeof buffer);

    if(chunk == 0) {
      memset(buffer, 0, 0x200);
      memset(buffer + 0x8373, ' ', 0x200);
    }

    digest_media_process(media, &media->digest.iso_ctx, buffer, sizeof buffer);

    update_progress(media, (chunk + 1) * (sizeof buffer >> 10));
  }

  if(!err && last_size) {
    if((u = read(fd, buffer, last_size)) != last_size) {
      err = 1;
      if(u > sizeof buffer) u = 0;
      media->err_ofs = (u >> 10) + chunk * (sizeof buffer >> 10);
    }
    else {
      digest_media_process(media, &media->digest.iso_ctx, buffer, last_size);
      digest_media_process(media, &media->digest.full_ctx, buffer, last_size);

      update_progress(media, media->iso_size - media->pad);
    }
  }

  if(!err) {
    memset(buffer, 0, 2 << 10);		/* 2k */
    for(u = 0; u < (media->pad >> 1); u++) {
      digest_media_process(media, &media->digest.iso_ctx, buffer, 2 << 10);
      digest_media_process(media, &media->digest.full_ctx, buffer, 2 << 10);

      update_progress(media, media->iso_size - media->pad + ((u + 1) << 1));
    }
  }

  if(
    !err &&
    media->full_size > media->iso_size &&
    (media->full_size - media->iso_size) < 16*1024	// allow for sane padding amounts (up to 16 MB)
  ) {
    for(u = 0; u < ((media->full_size - media->iso_size) >> 1); u++) {
      digest_media_process(media, &media->digest.full_ctx, buffer, 2 << 10);
    }
  }

  digest_media_finish(media, &media->digest.iso_ctx, media->digest.iso);
  digest_media_finish(media, &media->digest.part_ctx, media->digest.part);
  digest_media_finish(media, &media->digest.full_ctx, media->digest.full);

  update_progress(media, media->full_size);

  if(err) {
    media->err = 1;
  }
  else {
    media->got_iso = 1;
    media->got_part = 1;
    media->got_full = 1;

    if(
      media->got_iso_ref &&
      !memcmp(media->digest.iso, media->digest.iso_ref, media->digest.size)
    ) {
      media->iso_ok = 1;
    }

    if(
      media->got_part_ref &&
      !memcmp(media->digest.part, media->digest.part_ref, media->digest.size)
    ) {
      media->part_ok = 1;
    }
  }

  close(fd);
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
  int fd, ok = 0, tag_count = 0;
  unsigned char buf[8];
  char *key, *value, *next;
  struct stat sb;

  media->err = 1;

  if(!media->file_name) return;

  if((fd = open(media->file_name, O_RDONLY | O_LARGEFILE)) == -1) return;

  if(!fstat(fd, &sb) && S_ISREG(sb.st_mode)) {
    media->full_size = sb.st_size >> 10;
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
    unsigned little = 2*(buf[0] + (buf[1] << 8) + (buf[2] << 16) + (buf[3] << 24));
    unsigned big = 2*(buf[7] + (buf[6] << 8) + (buf[5] << 16) + (buf[4] << 24));

    if(little && little == big) {
      media->iso_size = little;
      ok++;
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
   * Application specific data - there's no restriction on what to put there.
   *
   * We use it to store the digests over the medium.
   *
   * Read it, to be parsed later.
   */
  if(
    lseek(fd, 0x8373, SEEK_SET) == 0x8373 &&
    read(fd, media->app_data, sizeof media->app_data - 1) == sizeof media->app_data - 1
  ) {
    media->app_data[sizeof media->app_data - 1] = 0;
    if(sanitize_data(media->app_data, sizeof media->app_data - 1)) ok++;
  }

  close(fd);

  if(ok != 3) return;

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
      !strcasecmp(key, "iso md5sum") ||
      !strcasecmp(key, "sha1sum") ||
      !strcasecmp(key, "sha224sum") ||
      !strcasecmp(key, "sha256sum") ||
      !strcasecmp(key, "sha384sum") ||
      !strcasecmp(key, "sha512sum")
    ) {
      media->got_iso_ref = set_digest(media, value, media->digest.iso_ref);
    }
    else if(!strcasecmp(key, "partition")) {
      if(value && isdigit(*value)) {
        unsigned start = strtoul(value, &value, 0);
        if(*value++ == ',') {
          unsigned blocks = strtoul(value, &value, 0);
          if(*value++ == ',') {
            media->part_start = start;
            media->part_blocks = blocks;

            media->got_part_ref = set_digest(media, value, media->digest.part_ref);
          }
        }
      }
    }
    else if(!strcasecmp(key, "pad")) {
      if(value && isdigit(*value)) {
        media->pad = strtoul(value, NULL, 0) << 1;
      }
    }
  }

  // if we didn't get the image size via stat() above, try other ways
  if(!media->full_size) {
    media->full_size = (media->part_start + media->part_blocks) >> 1;
    if(!media->full_size) media->full_size = media->iso_size;
  }
}


/*
 * Set digest type and value.
 *
 * value: hex string
 * buf: buffer to store digest
 *
 * return: 1 if valid, else 0
 *
 * Digest type is inferred from digest length.
 */
unsigned set_digest(mediacheck_t *media, char *value, unsigned char *buf)
{
  int len;
  unsigned u, u1;

  if(!value) return 0;

  len = strlen(value);

  // invalid length
  if(len & 1) len = 0;

  // binary size
  len >>= 1;

  switch(len) {
    case MD5_DIGEST_SIZE:
      media->digest.type = digest_md5;
      media->digest.name = "md5";
      break;

    case SHA1_DIGEST_SIZE:
      media->digest.type = digest_sha1;
      media->digest.name = "sha1";
      break;

    case SHA224_DIGEST_SIZE:
      media->digest.type = digest_sha224;
      media->digest.name = "sha224";
      break;

    case SHA256_DIGEST_SIZE:
      media->digest.type = digest_sha256;
      media->digest.name = "sha256";
      break;

    case SHA384_DIGEST_SIZE:
      media->digest.type = digest_sha384;
      media->digest.name = "sha384";
      break;

    case SHA512_DIGEST_SIZE:
      media->digest.type = digest_sha512;
      media->digest.name = "sha512";
      break;

    default:
      media->digest.type = digest_none;
      len = 0;
  }

  if(!len) return 0;

  // only one digest type
  if(media->digest.size && len != media->digest.size) return 0;

  media->digest.size = len;

  for(u = 0; u < media->digest.size; u++, value += 2) {
    if(sscanf(value, "%2x", &u1) == 1) {
      buf[u] = u1;
    }
    else {
      return 0;
    }
  }

  return 1;
}


/*
 * Do basic validation on the data and cut off trailing spcaes.
 *
 *
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
void update_progress(mediacheck_t *media, unsigned size)
{
  int percent;

  if(!media->full_size) {
    percent = 100;
  }
  else {
    percent = (size * 100) / media->full_size;
    if(percent > 100) percent = 100;
  }

  if(percent != media->last_percent) {
    media->last_percent = percent;
    if(media->progress) {
      media->progress(percent);
    }
  }
}


/*
 * Wrapper function for *_init_ctx() picking the correct digest.
 *
 * These functions must be called to start the digest calculation.
 */
void digest_media_init(mediacheck_t *media, digest_ctx_t *ctx)
{
  switch(media->digest.type) {
    case digest_md5:
      md5_init_ctx(&ctx->md5);
      break;
    case digest_sha1:
      sha1_init_ctx(&ctx->sha1);
      break;
    case digest_sha224:
      sha224_init_ctx(&ctx->sha224);
      break;
    case digest_sha256:
      sha256_init_ctx(&ctx->sha256);
      break;
    case digest_sha384:
      sha384_init_ctx(&ctx->sha384);
      break;
    case digest_sha512:
      sha512_init_ctx(&ctx->sha512);
      break;
    default:
      break;
  }
}


/*
 * Wrapper function for *_process_block() picking the correct digest.
 *
 * These functions do the actual digest calculation.
 *
 * Note: digest_media_process() *requires* buffer sizes to be a
 * multiple of 128. Otherwise use XXX_process_bytes().
 */
void digest_media_process(mediacheck_t *media, digest_ctx_t *ctx, unsigned char *buffer, unsigned len)
{
  switch(media->digest.type) {
    case digest_md5:
      md5_process_block(buffer, len, &ctx->md5);
      break;
    case digest_sha1:
      sha1_process_block(buffer, len, &ctx->sha1);
      break;
    case digest_sha224:
      sha256_process_block(buffer, len, &ctx->sha224);
      break;
    case digest_sha256:
      sha256_process_block(buffer, len, &ctx->sha256);
      break;
    case digest_sha384:
      sha512_process_block(buffer, len, &ctx->sha384);
      break;
    case digest_sha512:
      sha512_process_block(buffer, len, &ctx->sha512);
      break;
    default:
      break;
  }
}


/*
 * Wrapper function for *_finish_ctx() picking the correct digest.
 *
 * These functions must be called to finalize the digest calculation.
 */
void digest_media_finish(mediacheck_t *media, digest_ctx_t *ctx, unsigned char *buffer)
{
  switch(media->digest.type) {
    case digest_md5:
      md5_finish_ctx(&ctx->md5, buffer);
      break;
    case digest_sha1:
      sha1_finish_ctx(&ctx->sha1, buffer);
      break;
    case digest_sha224:
      sha224_finish_ctx(&ctx->sha224, buffer);
      break;
    case digest_sha256:
      sha256_finish_ctx(&ctx->sha256, buffer);
      break;
    case digest_sha384:
      sha384_finish_ctx(&ctx->sha384, buffer);
      break;
    case digest_sha512:
      sha512_finish_ctx(&ctx->sha512, buffer);
      break;
    default:
      break;
  }
}
