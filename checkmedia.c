#include <stdio.h>
#include <getopt.h>

#include "mediacheck.h"

void help(void);
void progress(unsigned percent);
void show_result(char *name, char *digest_name, unsigned checked, unsigned ok);

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
  { }
};


int main(int argc, char **argv)
{
  int i;
  mediacheck_t *media;

  opterr = 0;

  while((i = getopt_long(argc, argv, "hv", options, NULL)) != -1) {
    switch(i) {
      case 1:
        opt.version = 1;
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

  media = mediacheck_init(opt.file_name, progress);

  if(opt.verbose) {
    for(i = 0; i < sizeof media->tags / sizeof *media->tags; i++) {
      if(!media->tags[i].key) break;
      printf("       tags: key = \"%s\", value = \"%s\"\n", media->tags[i].key, media->tags[i].value);
    }
  }

  if(media->err) {
    printf("%s: not a supported image format\n", media->file_name);
    return 1;
  }

  if(media->digest.type == digest_none) {
    printf("%s: no digest found\n", media->file_name);
    return 1;
  }

  if(media->pad >= media->iso_size) {
    printf("padding (%u) bigger than image size\n", media->pad);
    return 1;
  }

  if(media->app_id) printf("        app: %s\n", media->app_id);
  if(media->iso_size) printf("   iso size: %u kB\n", media->iso_size);
  if(media->pad) printf("        pad: %u kB\n", media->pad);

  if(media->part_blocks) {
    printf(
      "  partition: start %u%s kB, size %u%s kB\n",
      media->part_start >> 1,
      (media->part_start & 1) ? ".5" : "",
      media->part_blocks >> 1,
      (media->part_blocks & 1) ? ".5" : ""
    );
  }

  if(opt.verbose) {
    if(media->full_size) printf("  full size: %u kB\n", media->full_size);

    if(media->got_iso_ref) {
      printf("    iso ref: ");
      for(i = 0; i < media->digest.size; i++) printf("%02x", media->digest.iso_ref[i]);
      printf("\n");
    }
    if(media->got_part_ref) {
      printf("   part ref: ");
      for(i = 0; i < media->digest.size; i++) printf("%02x", media->digest.part_ref[i]);
      printf("\n");
    }
  }

  printf("   checking:     ");
  fflush(stdout);
  mediacheck_calculate_digest(media);
  printf("\n");

  if(media->err && media->err_ofs) {
    printf("        err: sector %u\n", media->err_ofs >> 1);
  }

  printf("     result: ");
  i = 0;
  if(media->iso_size) {
    show_result("iso", media->digest.name, media->got_iso_ref, media->iso_ok);
    i = 1;
  }
  if(media->part_blocks) {
    if(i) printf(", ");
    show_result("partition", media->digest.name, media->got_part_ref, media->part_ok);
  }
  printf("\n");

  if(opt.verbose) {
    if(media->got_iso) {
      printf(" iso %6s: ", media->digest.name);
      for(i = 0; i < media->digest.size; i++) printf("%02x", media->digest.iso[i]);
      printf("\n");
    }

    if(media->got_part) {
      printf("part %6s: ", media->digest.name);
      for(i = 0; i < media->digest.size; i++) printf("%02x", media->digest.part[i]);
      printf("\n");
    }
  }

  if(media->got_full) {
    printf("%11s: ", media->digest.name);
    for(i = 0; i < media->digest.size; i++) printf("%02x", media->digest.full[i]);
    printf("\n");
  }

  i = media->iso_ok || media->part_ok ? 0 : 1;

  mediacheck_done(media);

  return i;
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
    "      --version         Show checkmedia version.\n"
    "  -v, --verbose         Show more detailed info.\n"
    "  -h, --help            Show this text.\n"
    "\n"
    "Usually both a checksum over the whole ISO image and the installation\n"
    "partition are available. Both are checked.\n"
  );
}


/*
 * Helper function: show test result.
 */
void show_result(char *name, char *digest_name, unsigned checked, unsigned ok)
{
  printf("%s %s ", name, digest_name);
  printf(checked ? ok ? "ok" : "wrong" : "unchecked");
}


/*
 * Progress indicator.
 */
void progress(unsigned percent)
{
  printf("\x08\x08\x08\x08%3d%%", percent);
  fflush(stdout);
}

