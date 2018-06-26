#include <stdio.h>
#include <getopt.h>

#include "mediacheck.h"

void help(void);
int progress(unsigned percent);

struct {
  unsigned verbose:1;
  unsigned help:1;
  unsigned version:1;
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

  if(opt.version) {
    printf(VERSION "\n");
    return 0;
  }

  if(argc == optind + 1) {
    opt.file_name = argv[optind];
  }
  else {
    help();
    return 1;
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

  if(!(mediacheck_digest_valid(media->digest.iso) || mediacheck_digest_valid(media->digest.part))) {
    printf("%s: no digest found\n", media->file_name);
    return 1;
  }

  if(media->iso_blocks && media->pad_blocks >= media->iso_blocks) {
    printf("padding (%u blocks) is bigger than image size\n", media->pad_blocks);
    return 1;
  }

  if(media->app_id) printf("        app: %s\n", media->app_id);
  if(media->iso_blocks) {
    printf(
      "   iso size: %u%s kB\n",
      media->iso_blocks >> 1,
      (media->iso_blocks & 1) ? ".5" : ""
    );

    if(media->pad_blocks) printf("        pad: %u kB\n", media->pad_blocks >> 1);
  }

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
    if(media->full_blocks) {
      printf(
        "  full size: %u%s kB\n",
        media->full_blocks >> 1,
        (media->full_blocks & 1) ? ".5" : ""
      );
    }

    if(mediacheck_digest_valid(media->digest.iso)) {
      printf("    iso ref: %s\n", mediacheck_digest_hex_ref(media->digest.iso));
    }
    if(mediacheck_digest_valid(media->digest.part)) {
      printf("   part ref: %s\n", mediacheck_digest_hex_ref(media->digest.part));
    }
  }

  printf("   checking:     ");
  fflush(stdout);
  mediacheck_calculate_digest(media);
  printf("\n");

  if(media->err && media->err_ofs) {
    printf("        err: sector %u\n", media->err_ofs);
  }

  printf("     result: ");
  i = 0;
  if(media->iso_blocks) {
    printf("iso %s %s", mediacheck_digest_name(media->digest.iso), mediacheck_digest_ok(media->digest.iso) ? "ok" : "wrong");
    i = 1;
  }
  if(media->part_blocks) {
    if(i) printf(", ");
    printf("partition %s %s", mediacheck_digest_name(media->digest.part), mediacheck_digest_ok(media->digest.part) ? "ok" : "wrong");
  }
  printf("\n");

  if(opt.verbose) {
    if(mediacheck_digest_valid(media->digest.iso)) {
      printf(" iso %6s: %s\n", mediacheck_digest_name(media->digest.iso), mediacheck_digest_hex(media->digest.iso));
    }

    if(mediacheck_digest_valid(media->digest.part)) {
      printf("part %6s: %s\n", mediacheck_digest_name(media->digest.part), mediacheck_digest_hex(media->digest.part));
    }
  }

  if(mediacheck_digest_valid(media->digest.full)) {
    printf("%11s: %s\n", mediacheck_digest_name(media->digest.full), mediacheck_digest_hex(media->digest.full));
  }

  i = mediacheck_digest_ok(media->digest.iso) || mediacheck_digest_ok(media->digest.part) ? 0 : 1;

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
    "Usually checksums both over the whole ISO image and the installation\n"
    "partition are available.\n"
  );
}


/*
 * Progress indicator.
 */
int progress(unsigned percent)
{
  printf("\x08\x08\x08\x08%3d%%", percent);
  fflush(stdout);

  return 0;
}

