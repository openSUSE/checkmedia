#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

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

/*
 * Here are some ISO9660 file system related constants.
 *
 * If in doubt about the ISO9660 layout, check https://wiki.osdev.org/ISO_9660
 */
// offset of volume descriptor
// usually points to "\x01CD001\x01\x00"
#define ISO9660_MAGIC_START	0x8000

// offset of volume size (in 2 kiB units)
// stored as 32 bit little-endian, followed by a 32 bit big-endian value
#define ISO9660_VOLUME_SIZE	0x8050

// offset of application identifier (some string)
#define ISO9660_APP_ID_START	0x823e

// application identifier length
#define ISO9660_APP_ID_LENGTH	0x80

// offset of volume identifier (some string)
#define ISO9660_VOLUME_ID_START	0x8028

// volume identifier length
#define ISO9660_VOLUME_ID_LENGTH	0x20

// offset of application specific data (anything goes)
#define ISO9660_APP_DATA_START	0x8373

// application specific data length
#define ISO9660_APP_DATA_LENGTH	0x200

// signature block starts with this string
#define SIGNATURE_MAGIC "7984fc91-a43f-4e45-bf27-6d3aa08b24cf"

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

struct mediacheck_digest_s {
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
};

typedef struct {
  unsigned start, blocks;
} chunk_region_t;

#include "mediacheck.h"

// corresponds to sign_state_t
static char *sign_states[] = {
  "not signed", "not checked", "ok", "bad", "bad (no matching key)"
};

static void digest_ctx_init(mediacheck_digest_t *digest);
static void digest_finish(mediacheck_digest_t *digest);
static void digest_data_to_hex(mediacheck_digest_t *digest);
static void get_info(mediacheck_t *media);
static int sanitize_data(char *data, int length);
static char *no_extra_spaces(char *str);
static void update_progress(mediacheck_t *media, unsigned blocks);
static void process_chunk(mediacheck_digest_t *digest, chunk_region_t *region, unsigned chunk, unsigned chunk_blocks, unsigned char *buffer);
static void normalize_chunk(mediacheck_t *media, unsigned chunk, unsigned chunk_blocks, unsigned char *buffer);
static void set_signature_state(mediacheck_t *media, sign_state_t state);
extern void verify_signature(mediacheck_t *media);

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

  set_signature_state(media, sig_not_signed);

  get_info(media);

  switch(media->style) {
    case style_suse:
      media->skip_blocks = 0;
      break;

    case style_rh:
      media->pad_blocks = 0;
      break;
  }

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

  mediacheck_digest_done(media->digest.iso);
  mediacheck_digest_done(media->digest.part);
  mediacheck_digest_done(media->digest.full);
  mediacheck_digest_done(media->digest.frag);

  free(media->signature.gpg_keys_log);
  free(media->signature.gpg_sign_log);
  free(media->signature.key_file);
  free(media->signature.signed_by);

  free(media);
}


/*
 * Set a specific public key to use for signature checking.
 *
 * If nothing is set, all keys from /usr/lib/rpm/gnupg/keys/ are used.
 */
API_SYM void mediacheck_set_public_key(mediacheck_t *media, char *key_file)
{
  if(!media) return;

  free(media->signature.key_file);
  media->signature.key_file = NULL;

  if(key_file) {
    media->signature.key_file = strdup(key_file);
  }
}


/*
 * Calculate digest over image.
 *
 * Call mediacheck_init() before doing this.
 */
API_SYM void mediacheck_calculate_digest(mediacheck_t *media)
{
  unsigned char buffer[64 << 10];		/* arbitrary, but at least 32 kiB, and stick to powers of 2 */
  unsigned chunk_size = sizeof buffer;

  /* fragment digest calculation requires a chunk size of 32 kiB */
  if(media->fragment.count) chunk_size = 32 << 10;

  unsigned chunk_blocks = chunk_size >> 9;
  unsigned last_chunk, last_chunk_blocks;
  unsigned chunk;
  int fd;

  chunk_region_t full_region = { 0, media->full_blocks } ;
  chunk_region_t iso_region = { 0, media->iso_blocks - media->pad_blocks - media->skip_blocks } ;
  chunk_region_t part_region = { media->part_start, media->part_blocks } ;

  last_chunk = media->full_blocks / chunk_blocks;
  last_chunk_blocks = media->full_blocks % chunk_blocks;

  uint64_t fragment_bytes = ((uint64_t) iso_region.blocks << 9) / (media->fragment.count + 1);

  if(!media || !media->file_name) return;

  if((fd = open(media->file_name, O_RDONLY | O_LARGEFILE)) == -1) return;

  update_progress(media, 0);

  media->digest.full = mediacheck_digest_init(
    media->digest.iso ? media->digest.iso->name : media->digest.part ? media->digest.part->name : NULL, NULL
  );

  unsigned last_fragment = 0;
  *media->fragment.sums = 0;;

  for(chunk = 0; !media->abort && chunk <= last_chunk; chunk++) {
    unsigned u, size = chunk_size;

    if(chunk == last_chunk) size = last_chunk_blocks << 9;

    if((u = read(fd, buffer, size)) != size) {
      media->err = 1;
      if(u > size) u = 0 ;
      media->err_block = (u >> 9) + chunk * chunk_blocks;
      break;
    };

    /*
     * The full digest should give the digest over the real file, without
     * any adjustments. So do it before manipulating the buffer.
     */
    process_chunk(media->digest.full, &full_region, chunk, chunk_blocks, buffer);

    normalize_chunk(media, chunk, chunk_blocks, buffer);

    process_chunk(media->digest.iso, &iso_region, chunk, chunk_blocks, buffer);
    process_chunk(media->digest.part, &part_region, chunk, chunk_blocks, buffer);

    update_progress(media, (chunk + 1) * chunk_blocks);

    if(media->fragment.count) {
      unsigned fragment = ((uint64_t) chunk * chunk_size) / fragment_bytes;
      if(fragment != last_fragment && fragment <= media->fragment.count) {
        unsigned fragment_size = FRAGMENT_SUM_LENGTH / media->fragment.count;
        if(!media->digest.frag) {
          media->digest.frag = calloc(1, sizeof *media->digest.frag);
        }
        *media->digest.frag = *media->digest.iso;
        digest_finish(media->digest.frag);

        for(unsigned u = 0; u < fragment_size && u < media->digest.frag->size; u++) {
          char buf[4];
          sprintf(buf, "%x", media->digest.frag->data[u]);
          strncat(media->fragment.sums, buf, 1);
        }
        if(memcmp(media->fragment.sums_ref, media->fragment.sums, strlen(media->fragment.sums))) {
          media->digest.frag->ok = 0;

          media->abort = 1;

          // since we abort, the other digest calculations will not be completed
          media->digest.iso->valid = 0;
          media->digest.full->valid = 0;
          if(media->digest.part) media->digest.part->valid = 0;
        }
        else {
          media->digest.frag->ok = 1;
        }

        last_fragment = fragment;
      }
    }
  }

  if(!media->err && !media->abort) {
    unsigned u;

    memset(buffer, 0, 1 << 9);		/* 0.5 kiB */
    for(u = 0; u < media->pad_blocks; u++) {
      mediacheck_digest_process(media->digest.iso, buffer, 1 << 9);
    }
  }

  if(!media->abort) update_progress(media, media->full_blocks);

  if(media->err) {
    if(media->digest.iso) media->digest.iso->valid = 0;
    if(media->digest.part) media->digest.part->valid = 0;
    if(media->digest.full) media->digest.full->valid = 0;
    if(media->digest.frag) media->digest.frag->valid = 0;
  }

  close(fd);

  verify_signature(media);
}


/*
 * Initialize digest struct with hex value.
 *
 * digest: digest struct
 * digest_name: digest name
 * digest_value: expected digest value as hex string
 *
 * return: new digest; NULL if invalid data have been passed to mediacheck_digest_init()
 *
 * If digest_name is NULL it is inferred from digest_value length.
 * If digest_value is NULL, no value is set.
 */
API_SYM mediacheck_digest_t *mediacheck_digest_init(char *digest_name, char *digest_value)
{
  unsigned u;
  int i, digest_by_name = 0, digest_by_size = 0;
  mediacheck_digest_t *digest;

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

  digest = calloc(1, sizeof *digest);

  digest->valid = 1;

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
  if(!digest_by_name && !digest_by_size) digest->valid = 0;

  // inconsistency
  if(digest_by_name && digest_by_size && digest_by_name != digest_by_size) digest->valid = 0;

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
        digest->valid = 0;
        break;
      }
    }
  }

  digest_data_to_hex(digest);

  if(!digest->valid) {
    mediacheck_digest_done(digest);
    digest = NULL;
  }

  return digest;
}


/*
 * This function does the actual digest calculation.
 */
API_SYM void mediacheck_digest_process(mediacheck_digest_t *digest, unsigned char *buffer, unsigned len)
{
  if(!digest || digest->finished) return;

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
 */
API_SYM void mediacheck_digest_done(mediacheck_digest_t *digest)
{
  if(!digest) return;

  free(digest);

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
 */
void get_info(mediacheck_t *media)
{
  int fd, ok = 0, tag_count = 0, iso_magic_ok = 0;
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
    lseek(fd, ISO9660_MAGIC_START, SEEK_SET) == ISO9660_MAGIC_START &&
    read(fd, buf, 8) == 8
  ) {
    // yes, 8 bytes
    if(!memcmp(buf, "\001CD001\001", 8)) iso_magic_ok = 1;
  }

  /*
   * ISO size is stored as both 32 bit little- and big-endian values.
   *
   * Read both and compare as consistency check.
   */
  if(
    lseek(fd, ISO9660_VOLUME_SIZE, SEEK_SET) == ISO9660_VOLUME_SIZE &&
    read(fd, buf, 8) == 8
  ) {
    unsigned little = 4*(buf[0] + (buf[1] << 8) + (buf[2] << 16) + (buf[3] << 24));
    unsigned big = 4*(buf[7] + (buf[6] << 8) + (buf[5] << 16) + (buf[4] << 24));

    if(iso_magic_ok && little && little == big) {
      media->iso_blocks = little;
    }
  }

  /*
   * Application id is set to some string identifying the media.
   *
   * Read it and show it to the user later.
   */
  if(
    lseek(fd, ISO9660_APP_ID_START, SEEK_SET) == ISO9660_APP_ID_START &&
    read(fd, media->app_id, sizeof media->app_id - 1) == sizeof media->app_id - 1
  ) {
    media->app_id[sizeof media->app_id - 1] = 0;
    if(sanitize_data(media->app_id, sizeof media->app_id - 1)) {
      char *s;

      if(!strncmp(media->app_id, "MKISOFS", sizeof "MKISOFS" - 1)) *media->app_id = 0;
      if(!strncmp(media->app_id, "GENISOIMAGE", sizeof "GENISOIMAGE" - 1)) *media->app_id = 0;
      if((s = strrchr(media->app_id, '#'))) *s = 0;

      if(media->app_id[0]) ok++;
    }
  }

  /* ensure media->app_id[] really is large enough to alternatively hold the volume id */
  #if ISO9660_VOLUME_ID_LENGTH > ISO9660_APP_ID_LENGTH
    #error "oops, media->app_id[] too small"
  #endif

  /*
   * If application id is not set, go for volume id.
   */
  if(
    !media->app_id[0] &&
    lseek(fd, ISO9660_VOLUME_ID_START, SEEK_SET) == ISO9660_VOLUME_ID_START &&
    read(fd, media->app_id, ISO9660_VOLUME_ID_LENGTH) == ISO9660_VOLUME_ID_LENGTH
  ) {
    media->app_id[ISO9660_VOLUME_ID_LENGTH] = 0;
    if(sanitize_data(media->app_id, ISO9660_VOLUME_ID_LENGTH)) {
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
    lseek(fd, ISO9660_APP_DATA_START, SEEK_SET) == ISO9660_APP_DATA_START &&
    read(fd, media->app_data, sizeof media->app_data - 1) == sizeof media->app_data - 1
  ) {
    media->app_data[sizeof media->app_data - 1] = 0;
    memcpy(media->signature.blob, media->app_data, sizeof media->signature.blob);
    if(sanitize_data(media->app_data, sizeof media->app_data - 1)) ok++;
  }

  if(ok != 2) {
    close(fd);

    return;
  }

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

    char *digest_key = key;
    if(!strncasecmp(digest_key, "iso ", sizeof "iso " - 1)) {
      digest_key += sizeof "iso " - 1;
    }

    if(
      !strcasecmp(digest_key, "md5sum") ||
      !strcasecmp(digest_key, "sha1sum") ||
      !strcasecmp(digest_key, "sha224sum") ||
      !strcasecmp(digest_key, "sha256sum") ||
      !strcasecmp(digest_key, "sha384sum") ||
      !strcasecmp(digest_key, "sha512sum")
    ) {
      media->style = digest_key == key ? style_suse : style_rh;
      if(media->iso_blocks) {
        media->digest.iso = mediacheck_digest_init(NULL, value);
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

            media->digest.part = mediacheck_digest_init(NULL, value);
          }
        }
      }
    }
    else if(!strcasecmp(key, "pad")) {
      if(value && isdigit(*value)) {
        media->pad_blocks = strtoul(value, NULL, 0) << 2;
      }
    }
    else if(!strcasecmp(key, "skipsectors")) {
      if(value && isdigit(*value)) {
        media->skip_blocks = strtoul(value, NULL, 0) << 2;
      }
    }
    else if(!strcasecmp(key, "fragment count")) {
      if(value && isdigit(*value)) {
        media->fragment.count = strtoul(value, NULL, 0);
      }
    }
    else if(!strcasecmp(key, "fragment sums")) {
      if(value) {
        strncpy(media->fragment.sums_ref, value, sizeof media->fragment.sums_ref - 1);
      }
    }
    else if(!strcasecmp(key, "signature")) {
      if(value && isdigit(*value)) {
        media->signature.start = strtoul(value, NULL, 0);

        if(
          media->signature.start &&
          lseek(fd, (off_t) media->signature.start * 0x200, SEEK_SET) == (off_t) media->signature.start * 0x200 &&
          read(fd, media->signature.magic, sizeof media->signature.magic) == sizeof media->signature.magic &&
          read(fd, media->signature.data, sizeof media->signature.data) == sizeof media->signature.data &&
          !memcmp(media->signature.magic, SIGNATURE_MAGIC, sizeof SIGNATURE_MAGIC - 1) &&
          media->signature.data[0]
        ) {
          media->signature.magic[sizeof media->signature.magic - 1] = 0;
          media->signature.data[sizeof media->signature.data - 1] = 0;
          set_signature_state(media, sig_not_checked);
        }
      }
    }
  }

  // if we didn't get the image size via stat() above, try other ways
  if(!media->full_blocks) {
    media->full_blocks = media->part_start + media->part_blocks;
    if(media->iso_blocks > media->full_blocks) media->full_blocks = media->iso_blocks;
  }

  close(fd);
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
      media->abort |= media->progress(percent);
    }
  }
}


/*
 * Process digest of a single chunk.
 *
 * digest: pointer to digest struct
 * region: pointer to region (start and size of area) over which to calculate digest
 * chunk: current chunk (counted 0-based)
 * chunk_blocks: chunk size in blocks (0.5 kiB)
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


/*
 * Normalize (clear) some data in buffer.
 *
 * buffer size is chunk_blocks * 0x200 bytes
 * chunks will always be 2 kiB aligned and at least 2 kiB in size
 *
 * Normalized data assumes
 *   - SUSE style only: 0x0000 - 0x01ff (mbr) is filled with zeros (0)
 *   - 0x8373 - 0x8572 (iso9660 app data) is filled with spaces (' ').
 *   - signature block (2 kiB) contains only magic id + zeros (0)
 */
void normalize_chunk(mediacheck_t *media, unsigned chunk, unsigned chunk_blocks, unsigned char *buffer)
{
  unsigned start_block = chunk * chunk_blocks;
  unsigned end_block = start_block + chunk_blocks;

  uint64_t start_ofs = (uint64_t) start_block << 9;
  uint64_t end_ofs = (uint64_t) end_block << 9;

  if(
    media->style == style_suse &&
    start_ofs == 0
  ) {
    // clear MBR area
    memset(buffer, 0, 0x200);
  }

  // application data block
  if(
    ISO9660_APP_DATA_START >= start_ofs &&
    ISO9660_APP_DATA_START + ISO9660_APP_DATA_LENGTH <= end_ofs
  ) {
    memset(buffer + ISO9660_APP_DATA_START - start_ofs, ' ', ISO9660_APP_DATA_LENGTH);
  }

  // reset signature area (4 blocks)
  if(
    media->signature.start &&
    media->signature.start >= start_block &&
    media->signature.start + 4 <= end_block
  ) {
    // keep first 64 bytes (signature magic)
    memset(buffer + ((media->signature.start - start_block) << 9) + 0x40, 0, (4 << 9) - 0x40);
  }
}


/*
 * Set signature state.
 *
 * Sets both signature.state & signature.state_str.
 */
void set_signature_state(mediacheck_t *media, sign_state_t state)
{
  media->signature.state.id = state;
  if(state < sizeof sign_states / sizeof *sign_states) {
    media->signature.state.str = sign_states[state];
  }
}


/*
 * Verify signature.
 *
 * Call mediacheck_init() before doing this.
 *
 * The is function imports all keys from /usr/lib/rpm/gnupg/keys into a
 * temporary key ring and then runs gpg to verify the signature.
 */
void verify_signature(mediacheck_t *media)
{
  char tmp_dir[] = "/tmp/mediacheck.XXXXXX";
  char *buf;
  int cmd_err;
  FILE *f;

  if(!media->signature.start || media->signature.state.id == sig_not_signed) return;

  if(!mkdtemp(tmp_dir)) return;

  asprintf(&buf, "%s/foo", tmp_dir);

  if((f = fopen(buf, "w"))) {
    fwrite(media->signature.blob, 1, sizeof media->signature.blob, f);
    fclose(f);
  }

  free(buf);

  asprintf(&buf, "%s/foo.asc", tmp_dir);

  if((f = fopen(buf, "w"))) {
    fprintf(f, "%s", media->signature.data);
    fclose(f);
  }

  free(buf);

  asprintf(&buf,
    "/usr/bin/gpg --batch --homedir %s --no-default-keyring --ignore-time-conflict --ignore-valid-from "
    "--keyring %s/sign.gpg --import %s >%s/gpg_keys.log 2>&1",
    tmp_dir,
    tmp_dir,
    media->signature.key_file ?: "/usr/lib/rpm/gnupg/keys/*",
    tmp_dir
  );

  cmd_err = WEXITSTATUS(system(buf));

  free(buf);

  asprintf(&buf, "%s/gpg_keys.log", tmp_dir);

  if((f = fopen(buf, "r"))) {
    char txt[4096] = {};	// just big enough
    fread(txt, 1, sizeof txt - 1, f);
    fclose(f);
    free(media->signature.gpg_keys_log);
    asprintf(&media->signature.gpg_keys_log, "%sgpg: exit code: %d\n", txt, cmd_err);
  }

  free(buf);

  if(!cmd_err) {
    asprintf(&buf,
      "/usr/bin/gpg --batch --homedir %s --no-default-keyring --ignore-time-conflict --ignore-valid-from "
      "--keyring %s/sign.gpg --verify %s/foo.asc %s/foo >%s/gpg_sign.log 2>&1",
      tmp_dir, tmp_dir, tmp_dir, tmp_dir, tmp_dir
    );

    cmd_err = WEXITSTATUS(system(buf));

    free(buf);

    asprintf(&buf, "%s/gpg_sign.log", tmp_dir);

    if((f = fopen(buf, "r"))) {
      char txt[4096] = {};	// just big enough
      fread(txt, 1, sizeof txt - 1, f);
      fclose(f);
      free(media->signature.gpg_sign_log);
      asprintf(&media->signature.gpg_sign_log, "%sgpg: exit code: %d\n", txt, cmd_err);
    }

    free(buf);

    set_signature_state(media, sig_bad);

    if(media->signature.gpg_sign_log) {
      char *signee_start, *signee_end;
      if((signee_start = strstr(media->signature.gpg_sign_log, "gpg: Good signature from \""))) {
        set_signature_state(media, sig_ok);
        signee_start += sizeof "gpg: Good signature from \"" - 1;
        if((signee_end = strchr(signee_start, '"'))) {
          free(media->signature.signed_by);
          asprintf(&media->signature.signed_by, "%.*s", (int) (signee_end - signee_start), signee_start);
        }
      }
      if(strstr(media->signature.gpg_sign_log, "gpg: Can't check signature: No public key")) {
        set_signature_state(media, sig_bad_no_key);
      }
    }
  }

  asprintf(&buf, "/usr/bin/rm -r %s", tmp_dir);

  system(buf);

  free(buf);
}
