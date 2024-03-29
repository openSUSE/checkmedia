#ifndef _MEDIACHECK_H
#define _MEDIACHECK_H

#ifdef __cplusplus
extern "C" {
#endif

// see mediacheck.c
#define ISO9660_APP_ID_LENGTH	0x80
#define ISO9660_APP_DATA_LENGTH	0x200

//
#define FRAGMENT_SUM_LENGTH	60

typedef struct mediacheck_digest_s mediacheck_digest_t;

typedef int (* mediacheck_progress_t)(unsigned percent);

typedef enum { sig_not_signed, sig_not_checked, sig_ok, sig_bad, sig_bad_no_key } sign_state_t;

typedef enum { style_suse = 1, style_rh } digest_style_t;

typedef struct {
  char *file_name;				/* file to check */
  mediacheck_progress_t progress;		/* progress function */

  unsigned full_blocks;				/* full image size, in 0.5 kiB units */
  unsigned iso_blocks;				/* iso size in 0.5 kiB units */
  unsigned pad_blocks;				/* padding size in 0.5 kiB units */
  unsigned skip_blocks;				/* skip size in 0.5 kiB units */
  unsigned part_start;				/* partition start, in 0.5 kiB units */
  unsigned part_blocks;				/* partition size, in 0.5 kiB units */

  digest_style_t style;				/* type of digest data */

  struct {
    mediacheck_digest_t *full;			/* full image digest, calculated */
    mediacheck_digest_t *iso;			/* iso digest, calculated */
    mediacheck_digest_t *part;			/* partition digest, calculated */
    mediacheck_digest_t *frag;			/* last fragment digest, calculated */
  } digest;

  struct {
    unsigned count;				/* number of fragments */
    char sums_ref[FRAGMENT_SUM_LENGTH + 1];	/* fragment checksums, reference value */
    char sums[FRAGMENT_SUM_LENGTH + 1];		/* fragment checksums, calculated value */
  } fragment;

  struct {
    char *key, *value;
  } tags[16];					/* up to 16 key - value pairs */

  unsigned abort:1;				/* check aborted */
  unsigned err:1;				/* read error */
  unsigned err_block;				/* read error position (in 0.5 kiB units) */

  char app_id[ISO9660_APP_ID_LENGTH + 1];	/* application id */
  char app_data[ISO9660_APP_DATA_LENGTH + 1];	/* app specific data */

  int last_percent;				/* last percentage shown by progress function */

  struct {
    unsigned start;				/* start block of signature (if any), in 0.5 kiB units */
    struct {					/* signature state */
      sign_state_t id;				/* ... numerical */
      char *str;				/* ... as string (static, don't free) */
    } state;
    char magic[0x40];				/* 64 bytes */
    char data[0x800 - 0x40];			/* 2k block - 64 bytes */
    char blob[ISO9660_APP_DATA_LENGTH];		/* data the signature applies to */
    char *gpg_keys_log;				/* gpg output from key import */
    char *gpg_sign_log;				/* gpg output from signature check */
    char *key_file;				/* gpg public key to use for signature check */
    char *signed_by;				/* signee, parsed from gpg output */
  } signature;
} mediacheck_t;


/*
 * Create new mediacheck object.
 *
 * file_name: the name of the image file (or device) to check
 *
 * progress: function that will be called at regular intervals to
 *   indicate the verification progress. The function will be typically called
 *   once for each additional percent progressed. The first call is guaranteed to
 *   be for 0 %, the last for 100 %.
 *
 * 'progress' may return 0 or 1. 1 indicates that the check should be aborted.
 *
 * 'mediacheck_init' always returns a non-NULL pointer. '(mediacheck_t).err' will
 *  be set if there has been a problem.
 */
mediacheck_t *mediacheck_init(char *file_name, mediacheck_progress_t progress);

/*
 * Free resources associated with 'media'.
 */
void mediacheck_done(mediacheck_t *media);

/*
 * Set specific public key for signature checking.
 */
void mediacheck_set_public_key(mediacheck_t *media, char *key_file);

/*
 * Run the actual media check.
 *
 * During the check the 'progress' function that has been passed to
 * 'mediacheck_init()' is called at regular intervals.
 *
 * Look at 'media->err' and other elements in 'media' for the result.
 */
void mediacheck_calculate_digest(mediacheck_t *media);


/*
 * Create new digest object.
 *
 * digest_name: digest name, e.g. "sha256"
 * digest_value: if given, the calculated value is compared against this value
 *
 * This function returns NULL if there was a problem.
 *
 * Only one of 'digest_name' or 'digest_value' need to be specified. If 'digest_name' is missing it
 * is inferred from the length of 'digest_value'.
 *
 * If both are given, 'digest_value' must have the correct length.
 */
mediacheck_digest_t *mediacheck_digest_init(char *digest_name, char *digest_value);

/*
 * Destroy digest object.
 */
void mediacheck_digest_done(mediacheck_digest_t *digest);


/*
 * Calculate digest.
 *
 * To be called repeatedly to update the digest state.
 *
 * Note: after looking at the calculated digest (e.g. via
 *  'mediacheck_digest_hex()' or 'mediacheck_digest_ok()') you cannot call
 *  'mediacheck_digest_process()' on 'digest' any longer - 'digest' will no
 *  longer be updated.
 */
void mediacheck_digest_process(mediacheck_digest_t *digest, unsigned char *buffer, unsigned len);

/*
 * Check if digest is valid.
 *
 * Return 1 if valid, 0 if not.
 */
int mediacheck_digest_valid(mediacheck_digest_t *digest);

/*
 * Check if calculated digest matches the expected value.
 *
 * Return 1 if it matches, 0 if not.
 *
 * Note: this implicitly finishes the digest calulation (you can no longer call
 * 'mediacheck_digest_process()' on 'digest').
 */
int mediacheck_digest_ok(mediacheck_digest_t *digest);

/*
 * Digest name.
 *
 * Never returns NULL but possibly "" (empty string) if there's no valid digest.
 */
char *mediacheck_digest_name(mediacheck_digest_t *digest);

/*
 * Digest as hex string.
 *
 * Never returns NULL but possibly "" (empty string) if there's no valid digest.
 *
 * Note: this implicitly finishes the digest calulation (you can no longer call
 * 'mediacheck_digest_process()' on 'digest').
 */
char *mediacheck_digest_hex(mediacheck_digest_t *digest);

/*
 * Reference digest (the expected value) as hex string
 *
 * Never returns NULL but possibly "" (empty string) if there's no reference
 * digest (last argument to 'mediacheck_digest_init()').
 */
char *mediacheck_digest_hex_ref(mediacheck_digest_t *digest);

#ifdef __cplusplus
}
#endif

#endif /* _MEDIACHECK_H */
