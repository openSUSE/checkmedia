#include <stdio.h>
#include <getopt.h>

#include "mediacheck.h"

void help(void);
int progress(unsigned percent);

struct {
  unsigned verbose;
  unsigned help:1;
  unsigned version:1;
  char *file_name;
  char *key_file;
} opt;

struct option options[] = {
  { "help", 0, NULL, 'h' },
  { "verbose", 0, NULL, 'v' },
  { "version", 0, NULL, 1 },
  { "key-file", 1, NULL, 2 },
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

      case 2:
        opt.key_file = optarg;
        break;

      case 'v':
        opt.verbose++;
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
    fprintf(stderr, "checkmedia: no file to check specified\n");
    help();
    return 1;
  }

  media = mediacheck_init(opt.file_name, progress);

  if(opt.key_file) mediacheck_set_public_key(media, opt.key_file);

  if(opt.verbose >= 2) {
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
      "   iso size: %u%s kiB\n",
      media->iso_blocks >> 1,
      (media->iso_blocks & 1) ? ".5" : ""
    );

    if(media->skip_blocks) printf("       skip: %u kiB\n", media->skip_blocks >> 1);
    if(media->pad_blocks) printf("        pad: %u kiB\n", media->pad_blocks >> 1);
  }

  if(media->part_blocks) {
    printf(
      "  partition: start %u%s kiB, size %u%s kiB\n",
      media->part_start >> 1,
      (media->part_start & 1) ? ".5" : "",
      media->part_blocks >> 1,
      (media->part_blocks & 1) ? ".5" : ""
    );
  }

  if(opt.verbose >= 1) {
    if(media->full_blocks) {
      printf(
        "  full size: %u%s kiB\n",
        media->full_blocks >> 1,
        (media->full_blocks & 1) ? ".5" : ""
      );
    }

    if(media->signature.start) {
      printf(" sign block: %d\n", media->signature.start);
    }

    if(mediacheck_digest_valid(media->digest.iso)) {
      printf("    iso ref: %s\n", mediacheck_digest_hex_ref(media->digest.iso));
    }

    if(mediacheck_digest_valid(media->digest.part)) {
      printf("   part ref: %s\n", mediacheck_digest_hex_ref(media->digest.part));
    }

    if(media->fragment.count) {
      printf("  fragments: %d\n", media->fragment.count);
      printf("fragsum ref: %s\n", media->fragment.sums_ref);
    }

    printf("      style: %s\n", media->style == style_rh ? "rh" : "suse");
  }

  printf("   checking:     ");
  fflush(stdout);
  mediacheck_calculate_digest(media);
  printf("\n");

  if(media->err && media->err_block) {
    printf("        err: block %u\n", media->err_block);
  }

  printf("     result: ");
  int comma_needed = 0;
  if(media->iso_blocks && mediacheck_digest_valid(media->digest.iso)) {
    printf(
      "iso %s %s",
      mediacheck_digest_name(media->digest.iso),
      mediacheck_digest_ok(media->digest.iso) ? "ok" : "wrong"
    );
    comma_needed = 1;
  }
  if(media->part_blocks && mediacheck_digest_valid(media->digest.part)) {
    if(comma_needed) printf(", ");
    printf(
      "partition %s %s",
      mediacheck_digest_name(media->digest.part),
      mediacheck_digest_ok(media->digest.part) ? "ok" : "wrong"
    );
  }
  if(media->fragment.count && mediacheck_digest_valid(media->digest.frag)) {
    if(comma_needed) printf(", ");
    printf(
      "fragments %s %s",
      mediacheck_digest_name(media->digest.frag),
      mediacheck_digest_ok(media->digest.frag) ? "ok" : "wrong"
    );
  }
  printf("\n");

  if(opt.verbose >= 1) {
    if(mediacheck_digest_valid(media->digest.iso)) {
      printf(" iso %6s: %s\n", mediacheck_digest_name(media->digest.iso), mediacheck_digest_hex(media->digest.iso));
    }

    if(mediacheck_digest_valid(media->digest.part)) {
      printf("part %6s: %s\n", mediacheck_digest_name(media->digest.part), mediacheck_digest_hex(media->digest.part));
    }

    if(media->fragment.count) {
      printf("frag %6s: %s\n", mediacheck_digest_name(media->digest.iso), media->fragment.sums);
    }
  }

  if(mediacheck_digest_valid(media->digest.full)) {
    printf("%11s: %s\n", mediacheck_digest_name(media->digest.full), mediacheck_digest_hex(media->digest.full));
  }

  if(opt.verbose >= 2) {
    if(media->signature.gpg_keys_log) {
      printf("# -- gpg key import log\n%s", media->signature.gpg_keys_log);
    }
    if(media->signature.gpg_sign_log) {
      printf("# -- gpg signature check log\n%s", media->signature.gpg_sign_log);
    }
    if(media->signature.gpg_keys_log || media->signature.gpg_sign_log) {
      printf("# --\n");
    }
  }

  printf("  signature: %s\n", media->signature.state.str);

  if(media->signature.state.id == sig_ok && media->signature.signed_by) {
    printf("  signed by: %s\n", media->signature.signed_by);
  }

  int result = mediacheck_digest_ok(media->digest.iso) || mediacheck_digest_ok(media->digest.part) || mediacheck_digest_ok(media->digest.frag) ? 0 : 1;

  if(media->signature.state.id == sig_bad) result = 1;

  mediacheck_done(media);

  return result;
}


/*
 * Display short usage message.
 */
void help()
{
  printf(
    "Usage: checkmedia [OPTIONS] FILE\n"
    "\n"
    "Check installation media.\n"
   "\n"
    "Options:\n"
    "      --key-file FILE   Use public key in FILE for signature check.\n"
    "      --version         Show checkmedia version.\n"
    "  -v, --verbose         Show more detailed info (repeat for more).\n"
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

