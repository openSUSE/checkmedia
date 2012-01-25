#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <fcntl.h>
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


static void do_digest(char *file);
static void get_info(char *file, unsigned opt_verbose);
static char *no_extra_spaces(char *str);
static void update_progress(unsigned size);
static void check_mbr(unsigned char *mbr);

static void digest_media_init(digest_ctx_t *ctx);
static void digest_media_process(digest_ctx_t *ctx, unsigned char *buffer, unsigned len);
static void digest_media_finish(digest_ctx_t *ctx, unsigned char *buffer);


struct {
  struct {
    digest_ctx_t ctx;
    digest_ctx_t full_ctx;
    digest_t type;                              /* digest type */
    char *name;                                 /* digest name */
    int size;                                   /* digest size */
    unsigned got_old:1;                         /* got digest stored in iso */
    unsigned got_current:1;                     /* calculated current digest */
    unsigned ok:1;                              /* digest matches */
    unsigned char old[MAX_DIGEST_SIZE];         /* digest stored in iso */
    unsigned char current[MAX_DIGEST_SIZE];     /* digest of iso ex special area */
    unsigned char full[MAX_DIGEST_SIZE];        /* full digest of iso */
  } digest;

  unsigned err:1;		/* some error */
  unsigned err_ofs;		/* read error pos */
  unsigned size;		/* in kb */
  unsigned hybrid_size;		/* hybrid partition, in kb */
  unsigned media_nr;		/* media number */

  char *media_type;		/* media type */
  char vol_id[33];		/* volume id */
  char app_id[81];		/* application id */
  char app_data[0x201];		/* app specific data*/
  unsigned pad;			/* pad size in kb */
  unsigned pad_is_skip:1;	/* skip last sectors */
} iso;


int main(int argc, char **argv)
{
  int i;
  unsigned opt_verbose = 0;
  unsigned opt_help = 0;
  unsigned opt_version = 0;

  if(argc > 1) {
    if(!strcmp(argv[1], "-v") || !strcmp(argv[1], "--verbose")) {
      opt_verbose = 1;
      argc--;
      argv++;
    }
    else if(!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
      opt_help = 1;
    }
    else if(!strcmp(argv[1], "--version")) {
      opt_version = 1;
    }
  }

  if(opt_help || argc != 2) {
    printf(
      "usage: checkmedia [options] iso\n"
      "Check SUSE installation media.\n"
      "Options:\n"
      "  --verbose\tshow some more info\n"
      "  --version\tdisplay version number\n"
      "  --help\tshort help text\n"
    );

    return 1;
  }

  if(opt_version) {
    printf("3.0\n");

    return 0;
  }

  get_info(argv[1], opt_verbose);

  if(iso.err) {
    printf("%s: not an iso\n", argv[1]);
    return 1;
  }

  if(!iso.digest.type) {
    printf("%s: no digest found\n", argv[1]);
    return 1;
  }

  printf("    app: %s\n  media: %s%d\n   size: %u kB\n",
    iso.app_id,
    iso.media_type,
    iso.media_nr ?: 1,
    iso.size
  );

  if(iso.hybrid_size) printf(" hybrid: %u kB\n", iso.hybrid_size);

  printf("    pad: %u kB\n", iso.pad);

  if(iso.pad >= iso.size) {
    printf("wrong padding value\n");
    return 1;
  }

  if(opt_verbose) {
    printf("    ref: ");
    if(iso.digest.got_old) for(i = 0; i < iso.digest.size; i++) printf("%02x", iso.digest.old[i]);
    printf("\n");
  }

  if(!iso.err) do_digest(argv[1]);

  if(iso.err && iso.err_ofs) {
    printf("   err: sector %u\n", iso.err_ofs >> 1);
  }

  printf("  check: ");
  if(iso.digest.got_old) {
    if(iso.digest.ok) {
      printf("%ssum ok\n", iso.digest.name);
    }
    else {
      printf("%ssum wrong\n", iso.digest.name);
    }
  }
  else {
    printf("%ssum not checked\n", iso.digest.name);
  }

  if(iso.digest.got_current && opt_verbose) {
    printf("%6s*: ", iso.digest.name);
    for(i = 0; i < iso.digest.size; i++) printf("%02x", iso.digest.current[i]);
    printf("\n");
  }

  if(iso.digest.got_current) {
    printf("%7s: ", iso.digest.name);
    for(i = 0; i < iso.digest.size; i++) printf("%02x", iso.digest.full[i]);
    printf("\n");
  }

  return iso.digest.ok ? 0 : 1;
}


/*
 * Calculate digest over iso.
 *
 * Normal digest, except that we assume
 *   - 0x0000 - 0x01ff is filled with zeros (0)
 *   - 0x8373 - 0x8572 is filled with spaces (' ').
 */
void do_digest(char *file)
{
  unsigned char buffer[64 << 10]; /* at least 36k! */
  int fd, err = 0;
  unsigned chunks = (iso.size - iso.pad) / (sizeof buffer >> 10);
  unsigned chunk, u;
  unsigned last_size = ((iso.size - iso.pad) % (sizeof buffer >> 10)) << 10;
  
  if((fd = open(file, O_RDONLY | O_LARGEFILE)) == -1) return;

  printf("  check:     "); fflush(stdout);

  digest_media_init(&iso.digest.ctx);
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

    digest_media_process(&iso.digest.ctx, buffer, sizeof buffer);

    update_progress((chunk + 1) * (sizeof buffer >> 10));
  }

  if(!err && last_size) {
    if((u = read(fd, buffer, last_size)) != last_size) {
      err = 1;
      if(u > sizeof buffer) u = 0;
      iso.err_ofs = (u >> 10) + chunk * (sizeof buffer >> 10);
    }
    else {
      digest_media_process(&iso.digest.ctx, buffer, last_size);
      digest_media_process(&iso.digest.full_ctx, buffer, last_size);

      update_progress(iso.size - iso.pad);
    }
  }

  if(!err) {
    memset(buffer, 0, 2 << 10);		/* 2k */
    for(u = 0; u < (iso.pad >> 1); u++) {
      if(!iso.pad_is_skip) digest_media_process(&iso.digest.ctx, buffer, 2 << 10);
      digest_media_process(&iso.digest.full_ctx, buffer, 2 << 10);

      update_progress(iso.size - iso.pad + ((u + 1) << 1));
    }
  }

  if(
    !err &&
    iso.hybrid_size > iso.size &&
    !iso.pad_is_skip &&
    (iso.hybrid_size - iso.size) < 16*1024	// allow for sane padding amounts (up to 16MB)
  ) {
    for(u = 0; u < ((iso.hybrid_size - iso.size) >> 1); u++) {
      digest_media_process(&iso.digest.full_ctx, buffer, 2 << 10);
    }
  }

  digest_media_finish(&iso.digest.ctx, iso.digest.current);
  digest_media_finish(&iso.digest.full_ctx, iso.digest.full);

  printf("\n");

  if(err) iso.err = 1;

  iso.digest.got_current = 1;

  if(iso.digest.got_old && !memcmp(iso.digest.current, iso.digest.old, iso.digest.size)) {
    iso.digest.ok = 1;
  }

  close(fd);
}


/*
 * Read all kinds of iso header info.
 */
void get_info(char *file, unsigned opt_verbose)
{
  int fd, ok = 0;
  unsigned char buf[4];
  char *s, *key, *value, *next;
  unsigned u, u1, idx;
  unsigned char mbr[512];

  memset(&iso, 0, sizeof iso);

  iso.err = 1;

  if(!file) return;

  if((fd = open(file, O_RDONLY | O_LARGEFILE)) == -1) return;

  if(read(fd, mbr, sizeof mbr) == sizeof mbr) check_mbr(mbr);

  if(
    lseek(fd, 0x8028, SEEK_SET) == 0x8028 &&
    read(fd, iso.vol_id, 32) == 32
  ) {
    iso.vol_id[sizeof iso.vol_id - 1] = 0;
    ok++;
  }

  if(
    lseek(fd, 0x8050, SEEK_SET) == 0x8050 &&
    read(fd, buf, 4) == 4
  ) {
    iso.size = 2*(buf[0] + (buf[1] << 8) + (buf[2] << 16) + (buf[3] << 24));
    ok++;
  }

  if(
    lseek(fd, 0x823e, SEEK_SET) == 0x823e &&
    read(fd, iso.app_id, 80) == 80
  ) {
    iso.app_id[sizeof iso.app_id - 1] = 0;
    ok++;
  }

  if(
    lseek(fd, 0x8373, SEEK_SET) == 0x8373 &&
    read(fd, iso.app_data, 0x200) == 0x200
  ) {
    iso.app_data[sizeof iso.app_data - 1] = 0;
    ok++;
  }

  close(fd);

  if(ok != 4) return;

  iso.media_type = "CD";

  iso.err = 0;

  for(s = iso.app_id + sizeof iso.app_id - 1; s >= iso.app_id; *s-- = 0) {
    if(*s != 0 && *s != ' ') break;
  }

  if(!strncmp(iso.app_id, "MKISOFS", sizeof "MKISOFS" - 1)) *iso.app_id = 0;

  if((s = strrchr(iso.app_id, '#'))) *s = 0;

  if(strstr(iso.app_id, "DVD") || iso.size >= 1 << 20) iso.media_type = "DVD";

  for(s = iso.vol_id + sizeof iso.vol_id - 1; s >= iso.vol_id; *s-- = 0) {
    if(*s != 0 && *s != ' ') break;
  }

  if(*iso.vol_id == 'S') {
    idx = iso.vol_id[strlen(iso.vol_id) - 1];
    if(idx >= '1' && idx <= '9') iso.media_nr = idx - '0';
  }

  for(key = next = iso.app_data; next; key = next) {
    value = strchr(key, '=');
    next = strchr(key, ';');
    if(value && next && value > next) value = NULL;

    if(value) *value++ = 0;
    if(next) *next++ = 0;

    key = no_extra_spaces(key);
    value = no_extra_spaces(value);

    if(opt_verbose) {
      printf("    raw: key = \"%s\", value = \"%s\"\n", key, value ?: "");
    }

    if((!strcasecmp(key, "md5sum") || !strcasecmp(key, "iso md5sum"))) {
      iso.digest.type = digest_md5;
      iso.digest.size = MD5_DIGEST_SIZE;
      iso.digest.name = "md5";
    }
    else if(!strcasecmp(key, "sha1sum")) {
      iso.digest.type = digest_sha1;
      iso.digest.size = SHA1_DIGEST_SIZE;
      iso.digest.name = "sha1";
    }
    else if(!strcasecmp(key, "sha224sum")) {
      iso.digest.type = digest_sha224;
      iso.digest.size = SHA224_DIGEST_SIZE;
      iso.digest.name = "sha224";
    }
    else if(!strcasecmp(key, "sha256sum")) {
      iso.digest.type = digest_sha256;
      iso.digest.size = SHA256_DIGEST_SIZE;
      iso.digest.name = "sha256";
    }
    else if(!strcasecmp(key, "sha384sum")) {
      iso.digest.type = digest_sha384;
      iso.digest.size = SHA384_DIGEST_SIZE;
      iso.digest.name = "sha384";
    }
    else if(!strcasecmp(key, "sha512sum")) {
      iso.digest.type = digest_sha512;
      iso.digest.size = SHA512_DIGEST_SIZE;
      iso.digest.name = "sha512";
    }

    if(iso.digest.type && value) {
      if(strlen(value) >= iso.digest.size) {
        for(u = 0 ; u < iso.digest.size; u++, value += 2) {
           if(sscanf(value, "%2x", &u1) == 1) {
             iso.digest.old[u] = u1;
           }
           else {
             break;
           }
        }
        if(u == iso.digest.size) iso.digest.got_old = 1;
      }
    }

    if(value && !strcasecmp(key, "pad")) {
      if(isdigit(*value)) iso.pad = strtoul(value, NULL, 0) << 1;
    }

    if(value && !strcasecmp(key, "skipsectors")) {
      if(isdigit(*value)) {
        iso.pad = strtoul(value, NULL, 0) << 1;
        iso.pad_is_skip = 1;
      }
    }
  }
}


/*
 * Remove leading & trailing spaces.
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


void update_progress(unsigned size)
{
  static int last_percent = 0;
  int percent;

  if(!size) last_percent = 0;

  percent = (size * 100) / (iso.size ?: 1);
  if(percent != last_percent) {
    last_percent = percent;
    printf("\x08\x08\x08\x08%3d%%", percent); fflush(stdout);
  }
}


void check_mbr(unsigned char *mbr)
{
  unsigned char *p = mbr + 0x1be;	// partition table
  unsigned s;

  if(mbr[0x1fe] != 0x55 || mbr[0x1ff] != 0xaa) return;

  if(p[0] & 0x7f) return;

  s = p[0x0c] + (p[0x0d] << 8) + (p[0x0e] << 16) + (p[0x0f] << 24);

  if(s <= 64) return;			// at least iso header

  if(s & 3) return;			// 2k, really

  iso.hybrid_size = s >> 1;
}


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

