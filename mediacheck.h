typedef struct digest_s digest_t;

int digest_init(digest_t *digest, char *digest_name, char *digest_value);
void digest_done(digest_t *digest);
int digest_valid(digest_t *digest);
int digest_ok(digest_t *digest);
char *digest_name(digest_t *digest);
char *digest_hex(digest_t *digest);

typedef void (* mediacheck_progress_t)(unsigned percent);

typedef struct {
  struct {
    digest_t *iso;				/* iso digest, calculated */
    digest_t *part;				/* partition digest, calculated */
    digest_t *full;				/* full image digest, calculated */
  } digest;

  struct {
    char *key, *value;
  } tags[16];					/* up to 16 key - value pairs */

  unsigned err:1;				/* some error */
  unsigned err_ofs;				/* read error pos */

  unsigned iso_size;				/* iso size in kB */
  unsigned iso_blocks;
  unsigned part_start;				/* partition start, in 0.5 kB units */
  unsigned part_blocks;				/* partition size, in 0.5 kB units */
  unsigned full_size;				/* full image size, in kB */
  unsigned full_blocks;

  char app_id[0x81];				/* application id */
  char app_data[0x201];				/* app specific data*/
  unsigned pad;					/* pad size in kB */
  unsigned pad_blocks;

  char *file_name;				/* file to check */
  int last_percent;				/* last percentage shown by progress function */
  mediacheck_progress_t progress;		/* progress function */
} mediacheck_t;

mediacheck_t *mediacheck_init(char *file_name, mediacheck_progress_t progress);
void mediacheck_done(mediacheck_t *media);
void mediacheck_calculate_digest(mediacheck_t *media);
