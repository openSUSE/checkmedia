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

#define MAX_DIGEST_SIZE SHA512_DIGEST_SIZE

typedef enum {
  digest_none, digest_md5, digest_sha1, digest_sha224, digest_sha256, digest_sha384, digest_sha512
} digest_t;

typedef union {
  struct md5_ctx md5;
  struct sha1_ctx sha1;
  struct sha256_ctx sha224;
  struct sha256_ctx sha256;
  struct sha512_ctx sha384;
  struct sha512_ctx sha512;
} digest_ctx_t;


void help(void);
void show_result(char *name, unsigned checked, unsigned ok);
void do_digest(char *file);
unsigned set_digest(char *value, unsigned char *buf);
int sanitize_data(char *data, int length);
void get_info(char *file);
char *no_extra_spaces(char *str);
void update_progress(unsigned size);

void digest_media_init(digest_ctx_t *ctx);
void digest_media_process(digest_ctx_t *ctx, unsigned char *buffer, unsigned len);
void digest_media_finish(digest_ctx_t *ctx, unsigned char *buffer);


struct {
  struct {
    digest_ctx_t iso_ctx;
    digest_ctx_t part_ctx;
    digest_ctx_t full_ctx;
    digest_t type;                              /* digest type */
    char *name;                                 /* digest name */
    unsigned got_iso_ref:1;                     /* got iso digest from tags */
    unsigned got_part_ref:1;                    /* got partition digest from tags */
    unsigned got_iso:1;                         /* iso digest has been calculated */
    unsigned got_part:1;                        /* partition digest has been calculated */
    unsigned got_full:1;                        /* full image digest has been calculated */
    unsigned iso_ok:1;                          /* iso digest matches */
    unsigned part_ok:1;                         /* partition digest matches */
    int size;                                   /* (binary) digest size - refers to the following arrays */
    unsigned char iso_ref[MAX_DIGEST_SIZE];     /* iso digest from tags */
    unsigned char part_ref[MAX_DIGEST_SIZE];    /* partition digest from tags */
    unsigned char iso[MAX_DIGEST_SIZE];         /* calculated iso digest */
    unsigned char part[MAX_DIGEST_SIZE];        /* calculated partition digest */
    unsigned char full[MAX_DIGEST_SIZE];        /* calculated full image digest */
  } digest;

  unsigned err:1;		/* some error */
  unsigned err_ofs;		/* read error pos */
  unsigned iso_size;		/* iso size in kB */
  unsigned full_size;		/* full image size, in kB */
  unsigned part_start;		/* partition start, in 0.5 kB units */
  unsigned part_blocks;		/* partition size, in 0.5 kB units */

  char app_id[0x81];		/* application id */
  char app_data[0x201];		/* app specific data*/
  unsigned pad;			/* pad size in kB */
} iso;

struct {
  unsigned verbose:1;
  unsigned help:1;
  unsigned version:1;
  unsigned part:1;
  unsigned iso:1;
  char *file_name;
} opt;

struct option options[] = {
  { "help", 0, NULL, 'h' },
  { "verbose", 0, NULL, 'v' },
  { "version", 0, NULL, 1 },
  { "iso", 0, NULL, 2 },
  { "partition", 0, NULL, 3 },
  { }
};


int main(int argc, char **argv)
{
  int i;

  opterr = 0;

  while((i = getopt_long(argc, argv, "hv", options, NULL)) != -1) {
    switch(i) {
      case 1:
        opt.version = 1;
        break;

      case 2:
        opt.iso = 1;
        break;

      case 3:
        opt.part = 1;
        break;

      case 'v':
        opt.verbose = 1;
        break;

      default:
        help();
        return i == 'h' ? 0 : 1;
    }
  }

  if(argc == optind + 1) {
    opt.file_name = argv[optind];
  }
  else {
    help();
    return 1;
  }

  if(opt.version) {
    printf(VERSION "\n");

    return 0;
  }

  get_info(opt.file_name);

  if(iso.err) {
    printf("%s: not an iso\n", opt.file_name);

    return 1;
  }

  if(iso.digest.type == digest_none) {
    printf("%s: no digest found\n", opt.file_name);

    return 1;
  }

  printf("        app: %s\n   iso size: %u kB\n",
    iso.app_id,
    iso.iso_size
  );

  printf("        pad: %u kB\n", iso.pad);

  if(iso.pad >= iso.iso_size) {
    printf("wrong padding value\n");
    return 1;
  }

  if(iso.part_blocks) {
    printf(
      "  partition: start %u%s kB, size %u%s kB\n",
      iso.part_start >> 1,
      (iso.part_start & 1) ? ".5" : "",
      iso.part_blocks >> 1,
      (iso.part_blocks & 1) ? ".5" : ""
    );
  }

  if(iso.full_size && opt.verbose) printf("  full size: %u kB\n", iso.full_size);

  if(opt.verbose) {
    if(iso.digest.got_iso_ref) {
      printf("    iso ref: ");
      for(i = 0; i < iso.digest.size; i++) printf("%02x", iso.digest.iso_ref[i]);
      printf("\n");
    }
    if(iso.digest.got_part_ref) {
      printf("   part ref: ");
      for(i = 0; i < iso.digest.size; i++) printf("%02x", iso.digest.part_ref[i]);
      printf("\n");
    }
  }

  if(!iso.err) do_digest(opt.file_name);

  if(iso.err && iso.err_ofs) {
    printf("        err: sector %u\n", iso.err_ofs >> 1);
  }

  printf("     result: ");
  i = 0;
  if(iso.iso_size) {
    show_result("iso", iso.digest.got_iso_ref, iso.digest.iso_ok);
    i = 1;
  }
  if(iso.part_blocks) {
    if(i) printf(", ");
    show_result("partition", iso.digest.got_part_ref, iso.digest.part_ok);
  }
  printf("\n");

  if(iso.digest.got_iso && opt.verbose) {
    printf(" iso %6s: ", iso.digest.name);
    for(i = 0; i < iso.digest.size; i++) printf("%02x", iso.digest.iso[i]);
    printf("\n");
  }

  if(iso.digest.got_part && opt.verbose) {
    printf("part %6s: ", iso.digest.name);
    for(i = 0; i < iso.digest.size; i++) printf("%02x", iso.digest.part[i]);
    printf("\n");
  }

  if(iso.digest.got_full) {
    printf("%11s: ", iso.digest.name);
    for(i = 0; i < iso.digest.size; i++) printf("%02x", iso.digest.full[i]);
    printf("\n");
  }

  return iso.digest.iso_ok || iso.digest.part_ok ? 0 : 1;
}


/*
 * Display short usage message.
 */
void help()
{
  printf(
    "Usage: checkmedia [OPTIONS] FILE\n"
    "\n"
    "Check SUSE installation media.\n"
   "\n"
    "Options:\n"
    "      --iso             Verify whole ISO image.\n"
    "      --partition       Verify only partition.\n"
    "\n"
    "      --version         Show checkmedia version.\n"
    "  -v, --verbose         Show more detailed info.\n"
    "  -h, --help            Show this text.\n"
    "\n"
    "Usually both a checksum over the whole ISO image and the installation\n"
    "partition are available. In this case per default the whole image is\n"
    "checked.\n"
    "\n"
    "You can use the --iso and --partition options to force verification\n"
    "using either checksum.\n"
  );
}


void show_result(char *name, unsigned checked, unsigned ok)
{
  printf("%s %s ", name, iso.digest.name);
  printf(checked ? ok ? "ok" : "wrong" : "unchecked");
}


/*
 * Calculate digest over image.
 *
 * Normal digest, except that we assume
 *   - 0x0000 - 0x01ff is filled with zeros (0)
 *   - 0x8373 - 0x8572 is filled with spaces (' ').
 */
void do_digest(char *file)
{
  unsigned char buffer[64 << 10]; /* at least 36k! */
  int fd, err = 0;
  unsigned chunks = (iso.iso_size - iso.pad) / (sizeof buffer >> 10);
  unsigned chunk, u;
  unsigned last_size = ((iso.iso_size - iso.pad) % (sizeof buffer >> 10)) << 10;
  
  if((fd = open(file, O_RDONLY | O_LARGEFILE)) == -1) return;

  printf("   checking:     "); fflush(stdout);

  digest_media_init(&iso.digest.iso_ctx);
  digest_media_init(&iso.digest.part_ctx);
  digest_media_init(&iso.digest.full_ctx);

  for(chunk = 0; chunk < chunks; chunk++) {
    if((u = read(fd, buffer, sizeof buffer)) != sizeof buffer) {
      err = 1;
      if(u > sizeof buffer) u = 0 ;
      iso.err_ofs = (u >> 10) + chunk * (sizeof buffer >> 10);
      break;
    };

    digest_media_process(&iso.digest.full_ctx, buffer, sizeof buffer);

    if(chunk == 0) {
      memset(buffer, 0, 0x200);
      memset(buffer + 0x8373, ' ', 0x200);
    }

    digest_media_process(&iso.digest.iso_ctx, buffer, sizeof buffer);

    update_progress((chunk + 1) * (sizeof buffer >> 10));
  }

  if(!err && last_size) {
    if((u = read(fd, buffer, last_size)) != last_size) {
      err = 1;
      if(u > sizeof buffer) u = 0;
      iso.err_ofs = (u >> 10) + chunk * (sizeof buffer >> 10);
    }
    else {
      digest_media_process(&iso.digest.iso_ctx, buffer, last_size);
      digest_media_process(&iso.digest.full_ctx, buffer, last_size);

      update_progress(iso.iso_size - iso.pad);
    }
  }

  if(!err) {
    memset(buffer, 0, 2 << 10);		/* 2k */
    for(u = 0; u < (iso.pad >> 1); u++) {
      digest_media_process(&iso.digest.iso_ctx, buffer, 2 << 10);
      digest_media_process(&iso.digest.full_ctx, buffer, 2 << 10);

      update_progress(iso.iso_size - iso.pad + ((u + 1) << 1));
    }
  }

  if(
    !err &&
    iso.full_size > iso.iso_size &&
    (iso.full_size - iso.iso_size) < 16*1024	// allow for sane padding amounts (up to 16 MB)
  ) {
    for(u = 0; u < ((iso.full_size - iso.iso_size) >> 1); u++) {
      digest_media_process(&iso.digest.full_ctx, buffer, 2 << 10);
    }
  }

  digest_media_finish(&iso.digest.iso_ctx, iso.digest.iso);
  digest_media_finish(&iso.digest.part_ctx, iso.digest.part);
  digest_media_finish(&iso.digest.full_ctx, iso.digest.full);

  printf("\n");

  if(err) {
    iso.err = 1;
  }
  else {
    iso.digest.got_iso = 1;
    iso.digest.got_part = 1;
    iso.digest.got_full = 1;

    if(
      iso.digest.got_iso_ref &&
      !memcmp(iso.digest.iso, iso.digest.iso_ref, iso.digest.size)
    ) {
      iso.digest.iso_ok = 1;
    }

    if(
      iso.digest.got_part_ref &&
      !memcmp(iso.digest.part, iso.digest.part_ref, iso.digest.size)
    ) {
      iso.digest.part_ok = 1;
    }
  }

  close(fd);
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
unsigned set_digest(char *value, unsigned char *buf)
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
      iso.digest.type = digest_md5;
      iso.digest.name = "md5";
      break;

    case SHA1_DIGEST_SIZE:
      iso.digest.type = digest_sha1;
      iso.digest.name = "sha1";
      break;

    case SHA224_DIGEST_SIZE:
      iso.digest.type = digest_sha224;
      iso.digest.name = "sha224";
      break;

    case SHA256_DIGEST_SIZE:
      iso.digest.type = digest_sha256;
      iso.digest.name = "sha256";
      break;

    case SHA384_DIGEST_SIZE:
      iso.digest.type = digest_sha384;
      iso.digest.name = "sha384";
      break;

    case SHA512_DIGEST_SIZE:
      iso.digest.type = digest_sha512;
      iso.digest.name = "sha512";
      break;

    default:
      iso.digest.type = digest_none;
      len = 0;
  }

  if(!len) return 0;

  // only one digest type
  if(iso.digest.size && len != iso.digest.size) return 0;

  iso.digest.size = len;

  for(u = 0; u < iso.digest.size; u++, value += 2) {
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
 * Read iso header and fill global iso struct.
 *
 * Note: checksum info is kept in iso.app_data[].
 *
 * If in doubt about the ISO9660 layout, check http://alumnus.caltech.edu/~pje/iso9660.html
 */
void get_info(char *file)
{
  int fd, ok = 0;
  unsigned char buf[8];
  char *key, *value, *next;
  struct stat sb;

  memset(&iso, 0, sizeof iso);

  iso.err = 1;

  if(!file) return;

  if((fd = open(file, O_RDONLY | O_LARGEFILE)) == -1) return;

  if(!fstat(fd, &sb) && S_ISREG(sb.st_mode)) {
    iso.full_size = sb.st_size >> 10;
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
      iso.iso_size = little;
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
    read(fd, iso.app_id, sizeof iso.app_id - 1) == sizeof iso.app_id - 1
  ) {
    iso.app_id[sizeof iso.app_id - 1] = 0;
    if(sanitize_data(iso.app_id, sizeof iso.app_id - 1)) {
      char *s;

      if(!strncmp(iso.app_id, "MKISOFS", sizeof "MKISOFS" - 1)) *iso.app_id = 0;
      if(!strncmp(iso.app_id, "GENISOIMAGE", sizeof "GENISOIMAGE" - 1)) *iso.app_id = 0;
      if((s = strrchr(iso.app_id, '#'))) *s = 0;

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
    read(fd, iso.app_data, sizeof iso.app_data - 1) == sizeof iso.app_data - 1
  ) {
    iso.app_data[sizeof iso.app_data - 1] = 0;
    if(sanitize_data(iso.app_data, sizeof iso.app_data - 1)) ok++;
  }

  close(fd);

  if(ok != 3) return;

  iso.err = 0;

  for(key = next = iso.app_data; next; key = next) {
    value = strchr(key, '=');
    next = strchr(key, ';');
    if(value && next && value > next) value = NULL;

    if(value) *value++ = 0;
    if(next) *next++ = 0;

    key = no_extra_spaces(key);
    value = no_extra_spaces(value);

    if(opt.verbose) {
      printf("       tags: key = \"%s\", value = \"%s\"\n", key, value ?: "");
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
      iso.digest.got_iso_ref = set_digest(value, iso.digest.iso_ref);
    }
    else if(!strcasecmp(key, "partition")) {
      if(value && isdigit(*value)) {
        unsigned start = strtoul(value, &value, 0);
        if(*value++ == ',') {
          unsigned blocks = strtoul(value, &value, 0);
          if(*value++ == ',') {
            iso.part_start = start;
            iso.part_blocks = blocks;

            iso.digest.got_part_ref = set_digest(value, iso.digest.part_ref);
          }
        }
      }
    }
    else if(!strcasecmp(key, "pad")) {
      if(value && isdigit(*value)) {
        iso.pad = strtoul(value, NULL, 0) << 1;
      }
    }
  }

  // if we didn't get the image size via stat() above, try other ways
  if(!iso.full_size) {
    iso.full_size = (iso.part_start + iso.part_blocks) >> 1;
    if(!iso.full_size) iso.full_size = iso.iso_size;
  }
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
void update_progress(unsigned size)
{
  static int last_percent = 0;
  int percent;

  if(!size) last_percent = 0;

  percent = (size * 100) / (iso.iso_size ?: 1);
  if(percent != last_percent) {
    last_percent = percent;
    printf("\x08\x08\x08\x08%3d%%", percent); fflush(stdout);
  }
}


/*
 * Wrapper function for *_init_ctx() picking the correct digest.
 *
 * These functions must be called to start the digest calculation.
 */
void digest_media_init(digest_ctx_t *ctx)
{
  switch(iso.digest.type) {
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
void digest_media_process(digest_ctx_t *ctx, unsigned char *buffer, unsigned len)
{
  switch(iso.digest.type) {
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
void digest_media_finish(digest_ctx_t *ctx, unsigned char *buffer)
{
  switch(iso.digest.type) {
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
