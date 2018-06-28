#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "mediacheck.h"

/*
 * Very simple program demonstrating using libmediacheck to calculate digests.
 *
 * digestdemo foo.img sha256
 *
 *   - calculate sha256 digest over foo.img
 *
 * digestdemo foo.img md5 d41d8cd98f00b204e9800998ecf8427e
 *
 *   - calculate md5 digest over foo.img and compare against d41...
 */
int main(int argc, char **argv)
{
  int fd, len, ok;
  unsigned char buffer[64 * 1024];

  if(argc < 3 || argc > 4) {
    fprintf(stderr, "invalid arguments\n");
    fprintf(stderr, "usage: digestdemo FILE DIGEST_NAME [DIGEST]\n");
    return 2;
  }

  mediacheck_digest_t *digest = mediacheck_digest_init(argv[2], argv[3]);

  if(!digest) {
    fprintf(stderr, "invalid digest data\n");
    return 2;
  }

  fd = open(argv[1], O_RDONLY);

  if(fd == -1) {
    perror(argv[1]);
    return 2;
  }

  while((len = read(fd, buffer, sizeof buffer)) > 0) {
    mediacheck_digest_process(digest, buffer, len);
  }

  if(len == -1) {
    perror(argv[1]);
    return 2;
  }

  close(fd);

  ok = mediacheck_digest_ok(digest);

  printf(
    "%s: %s(%d)=%s\n",
    argv[1],
    mediacheck_digest_name(digest),
    ok,
    mediacheck_digest_hex(digest)
  );

  mediacheck_digest_done(digest);

  return ok ? 0 : 1;
}
