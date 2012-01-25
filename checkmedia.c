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

static void do_md5(char *file);
static void get_info(char *file, unsigned opt_verbose);
static char *no_extra_spaces(char *str);
static void update_progress(unsigned size);
static void check_mbr(unsigned char *mbr);

struct {
  unsigned got_md5:1;		/* got md5sum */
  unsigned got_old_md5:1;	/* got md5sum stored in iso */
  unsigned md5_ok:1;		/* md5sums match */
  unsigned err:1;		/* some error */
  unsigned err_ofs;		/* read error pos */
  unsigned size;		/* in kb */
  unsigned hybrid_size;		/* hybrid partition, in kb */
  unsigned media_nr;		/* media number */

  /* align md5sum buffers for int32; md5_finish_ctx() expects it */
  unsigned char old_md5[16];	/* md5sum stored in iso */
  unsigned char md5[16];	/* md5sum */
  unsigned char full_md5[16];	/* complete md5sum */

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

  if(argc > 1) {
    if(!strcmp(argv[1], "-v")) {
      opt_verbose = 1;
      argc--;
      argv++;
    }
    else if(!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
      opt_help = 1;
    }
  }

  if(opt_help || argc != 2) {
    printf(
      "usage: checkmedia iso\n"
      "Check SUSE installation media.\n"
    );

    return 1;
  }

  get_info(argv[1], opt_verbose);

  if(iso.err) {
    printf("not an iso\n");
    return 1;
  }

  printf("   app: %s\n media: %s%d\n  size: %u kB\n",
    iso.app_id,
    iso.media_type,
    iso.media_nr ?: 1,
    iso.size
  );

  if(iso.hybrid_size) printf("hybrid: %u kB\n", iso.hybrid_size);

  printf("   pad: %u kB\n", iso.pad);

  if(iso.pad >= iso.size) {
    printf("wrong padding value\n");
    return 1;
  }

  if(opt_verbose) {
    printf("   ref: ");
    if(iso.got_old_md5) for(i = 0; i < sizeof iso.old_md5; i++) printf("%02x", iso.old_md5[i]);
    printf("\n");
  }

  if(!iso.err) do_md5(argv[1]);

  if(iso.err && iso.err_ofs) {
    printf("   err: sector %u\n", iso.err_ofs >> 1);
  }

  printf(" check: ");
  if(iso.got_old_md5) {
    if(iso.md5_ok) {
      printf("md5sum ok\n");
    }
    else {
      printf("md5sum wrong\n");
    }
  }
  else {
    printf("md5sum not checked\n");
  }

  if(iso.got_md5 && opt_verbose) {
    printf("  rmd5: ");
    for(i = 0; i < sizeof iso.md5; i++) printf("%02x", iso.md5[i]);
    printf("\n");
  }

  if(iso.got_md5) {
    printf("   md5: ");
    for(i = 0; i < sizeof iso.full_md5; i++) printf("%02x", iso.full_md5[i]);
    printf("\n");
  }

  return iso.md5_ok ? 0 : 1;
}


/*
 * Calculate md5 sum.
 *
 * Normal md5sum, except that we assume
 *   - 0x0000 - 0x01ff is filled with zeros (0)
 *   - 0x8373 - 0x8572 is filled with spaces (' ').
 */
void do_md5(char *file)
{
  unsigned char buffer[64 << 10]; /* at least 36k! */
  struct md5_ctx ctx, full_ctx;
  int fd, err = 0;
  unsigned chunks = (iso.size - iso.pad) / (sizeof buffer >> 10);
  unsigned chunk, u;
  unsigned last_size = ((iso.size - iso.pad) % (sizeof buffer >> 10)) << 10;
  
  if((fd = open(file, O_RDONLY | O_LARGEFILE)) == -1) return;

  printf(" check:     "); fflush(stdout);

  md5_init_ctx(&ctx);
  md5_init_ctx(&full_ctx);

  /*
   * Note: md5_process_block() below *requires* buffer sizes to be a
   * multiple of 64. Otherwise use md5_process_bytes().
   */

  for(chunk = 0; chunk < chunks; chunk++) {
    if((u = read(fd, buffer, sizeof buffer)) != sizeof buffer) {
      err = 1;
      if(u > sizeof buffer) u = 0 ;
      iso.err_ofs = (u >> 10) + chunk * (sizeof buffer >> 10);
      break;
    };

    md5_process_block(buffer, sizeof buffer, &full_ctx);

    if(chunk == 0) {
      memset(buffer, 0, 0x200);
      memset(buffer + 0x8373, ' ', 0x200);
    }

    md5_process_block(buffer, sizeof buffer, &ctx);

    update_progress((chunk + 1) * (sizeof buffer >> 10));
  }

  if(!err && last_size) {
    if((u = read(fd, buffer, last_size)) != last_size) {
      err = 1;
      if(u > sizeof buffer) u = 0;
      iso.err_ofs = (u >> 10) + chunk * (sizeof buffer >> 10);
    }
    else {
      md5_process_block(buffer, last_size, &ctx);
      md5_process_block(buffer, last_size, &full_ctx);

      update_progress(iso.size - iso.pad);
    }
  }

  if(!err) {
    memset(buffer, 0, 2 << 10);		/* 2k */
    for(u = 0; u < (iso.pad >> 1); u++) {
      if(!iso.pad_is_skip) md5_process_block(buffer, 2 << 10, &ctx);
      md5_process_block(buffer, 2 << 10, &full_ctx);

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
      md5_process_block(buffer, 2 << 10, &full_ctx);
    }
  }

  md5_finish_ctx(&ctx, iso.md5);
  md5_finish_ctx(&full_ctx, iso.full_md5);

  printf("\n");

  if(err) iso.err = 1;

  iso.got_md5 = 1;

  if(iso.got_old_md5 && !memcmp(iso.md5, iso.old_md5, sizeof iso.md5)) {
    iso.md5_ok = 1;
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
      printf("   raw: key = \"%s\", value = \"%s\"\n", key, value ?: "");
    }

    if(
      (!strcasecmp(key, "md5sum") || !strcasecmp(key, "iso md5sum")) &&
      value
    ) {
      if(strlen(value) >= 32) {
        for(u = 0 ; u < sizeof iso.old_md5; u++, value += 2) {
           if(sscanf(value, "%2x", &u1) == 1) {
             iso.old_md5[u] = u1;
           }
           else {
             break;
           }
        }
        if(u == sizeof iso.old_md5) iso.got_old_md5 = 1;
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

