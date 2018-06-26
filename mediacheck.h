typedef struct mediacheck_digest_s mediacheck_digest_t;

typedef int (* mediacheck_progress_t)(unsigned percent);

typedef struct {
  char *file_name;				/* file to check */
  mediacheck_progress_t progress;		/* progress function */

  unsigned full_blocks;				/* full image size, in 0.5 kB units */
  unsigned iso_blocks;				/* iso size in 0.5 kB units */
  unsigned pad_blocks;				/* padding size in 0.5 kB units */
  unsigned part_start;				/* partition start, in 0.5 kB units */
  unsigned part_blocks;				/* partition size, in 0.5 kB units */

  struct {
    mediacheck_digest_t *full;			/* full image digest, calculated */
    mediacheck_digest_t *iso;			/* iso digest, calculated */
    mediacheck_digest_t *part;			/* partition digest, calculated */
  } digest;

  struct {
    char *key, *value;
  } tags[16];					/* up to 16 key - value pairs */

  unsigned abort:1;				/* check aborted */
  unsigned err:1;				/* read error */
  unsigned err_ofs;				/* read error pos (in 0.5 kB units) */

  char app_id[0x81];				/* application id */
  char app_data[0x201];				/* app specific data*/

  int last_percent;				/* last percentage shown by progress function */
} mediacheck_t;

mediacheck_t *mediacheck_init(char *file_name, mediacheck_progress_t progress);
void mediacheck_done(mediacheck_t *media);
void mediacheck_calculate_digest(mediacheck_t *media);

int mediacheck_digest_init(mediacheck_digest_t *digest, char *digest_name, char *digest_value);
void mediacheck_digest_done(mediacheck_digest_t *digest);
void mediacheck_digest_process(mediacheck_digest_t *digest, unsigned char *buffer, unsigned len);
int mediacheck_digest_valid(mediacheck_digest_t *digest);
int mediacheck_digest_ok(mediacheck_digest_t *digest);
char *mediacheck_digest_name(mediacheck_digest_t *digest);
char *mediacheck_digest_hex(mediacheck_digest_t *digest);
char *mediacheck_digest_hex_ref(mediacheck_digest_t *digest);
