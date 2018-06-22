#include "md5.h"
#include "sha1.h"
#include "sha256.h"
#include "sha512.h"

#define MAX_DIGEST_SIZE SHA512_DIGEST_SIZE

typedef void (* mediacheck_progress_t)(unsigned percent);

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

typedef struct {
  char *key, *value;
} tag_t;

typedef struct {
  struct {
    digest_t type;				/* digest type */
    char *name;					/* digest name */
    int size;					/* (binary) digest size, not bigger than MAX_DIGEST_SIZE */

    digest_ctx_t iso_ctx;			/* iso digest context */
    digest_ctx_t part_ctx;			/* partition digest context */
    digest_ctx_t full_ctx;			/* full image digest context */

    unsigned char iso_ref[MAX_DIGEST_SIZE];	/* iso digest from tags */
    unsigned char part_ref[MAX_DIGEST_SIZE];	/* partition digest from tags */
    unsigned char iso[MAX_DIGEST_SIZE];		/* calculated iso digest */
    unsigned char part[MAX_DIGEST_SIZE];	/* calculated partition digest */
    unsigned char full[MAX_DIGEST_SIZE];	/* calculated full image digest */
  } digest;

  tag_t tags[16];

  unsigned err:1;				/* some error */
  unsigned err_ofs;				/* read error pos */

  unsigned iso_ok:1;				/* iso digest matches */
  unsigned part_ok:1;				/* partition digest matches */

  unsigned got_iso_ref:1;			/* got iso digest from tags */
  unsigned got_part_ref:1;			/* got partition digest from tags */
  unsigned got_iso:1;				/* iso digest has been calculated */
  unsigned got_part:1;				/* partition digest has been calculated */
  unsigned got_full:1;				/* full image digest has been calculated */

  unsigned iso_size;				/* iso size in kB */
  unsigned part_start;				/* partition start, in 0.5 kB units */
  unsigned part_blocks;				/* partition size, in 0.5 kB units */
  unsigned full_size;				/* full image size, in kB */

  char app_id[0x81];				/* application id */
  char app_data[0x201];				/* app specific data*/
  unsigned pad;					/* pad size in kB */

  char *file_name;				/* file to check */
  int last_percent;				/* last percentage shown by progress function*/
  mediacheck_progress_t progress;		/* progress function */
} mediacheck_t;


mediacheck_t *mediacheck_init(char *file_name, mediacheck_progress_t progress);
void mediacheck_calculate_digest(mediacheck_t *media);
void mediacheck_done(mediacheck_t *media);

