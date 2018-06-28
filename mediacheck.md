# libmediacheck

## About

`libmediacheck` is a library for verifying SUSE installaton media. It is used by [checkmedia](README.md) and
[linuxrc](https://github.com/openSUSE/linuxrc).

The library exports two groups of functions:

1. functions for verifying SUSE media
2. functions for digest calculations

The second group is there for convenience to be used by `linuxrc`.

## API functions for media verification

Have a look at [checkmedia.c](checkmedia.c) for a simple usage example.

### Create new mediacheck object

```
mediacheck_t *mediacheck_init(char *file_name, mediacheck_progress_t progress);
```

- `file_name` is the name of the image file (or device) to check.

- `progress` is a function that will be called at regular intervals to
indicate the verification progress. The function will be typically called
once for each additional percent progressed. The first call is guaranteed to
be for 0 %, the last for 100 %.

The function will always return a non-NULL pointer. `(mediacheck_t).err` will
be set if there has been a problem.

Look at [mediacheck.h](mediacheck.h) for the `mediacheck_t` definition.

### Destroy mediacheck object

```
void mediacheck_done(mediacheck_t *media);
```

Free resources associated with `media`.

### Run the actual media check

```
void mediacheck_calculate_digest(mediacheck_t *media);
```

During the check the `pregress` function that has been passed to
`mediacheck_init` is called at regular intervals.

Look at `media->err` and other elements in `media` for the result (see [checkmedia.c](checkmedia.c)).

## API functions for digest calculation

Have a look at [digestdemo.c](digestdemo.c) for a simple usage example.

Notes:

- `mediacheck_digest_t` is an opaque type; you cannot access any of its members directly.

- It is always ok to pass NULL as `digest` argument to any of the functions below.

### Create new digest object

```
mediacheck_digest_t *mediacheck_digest_init(char *digest_name, char *digest_value);
```

- `digest_name`: digest name, e.g. "sha256"
- `digest_value`: if given, the calculated value is compared against this value

This function returns NULL if there was a problem.

Only one of `digest_name` or `digest_value` need to be specified. If `digest_name` is missing it
is inferred from the length of `digest_value`.

If both are given, `digest_value` must have the correct length.

### Destroy digest object

```
void mediacheck_digest_done(mediacheck_digest_t *digest);
```

Free resources associated with `digest`.

### Calculate digest

```
void mediacheck_digest_process(mediacheck_digest_t *digest, unsigned char *buffer, unsigned len);
```

To be called repeatedly to update the digest state.

Note: after looking at the calculated digest (e.g. via
`mediacheck_digest_hex` or ``mediacheck_digest_ok`) you cannot call
`mediacheck_digest_process` on `digest` any longer.

### Check if digest is valid

```
int mediacheck_digest_valid(mediacheck_digest_t *digest);
```

Return 1 if valid, 0 if not.

### Check if calculated digest matched the expected value

```
int mediacheck_digest_ok(mediacheck_digest_t *digest);
```

Return 1 if it matches, 0 if not.

Note: this implicitly finishes the digest calulation (you can no longer call
`mediacheck_digest_process` on `digest`).

### Digest name

```
char *mediacheck_digest_name(mediacheck_digest_t *digest);
```

Never returns NULL but possibly "" (empty string) if there's no valid digest.

### Digest as hex string

```
char *mediacheck_digest_hex(mediacheck_digest_t *digest);
```

Never returns NULL but possibly "" (empty string) if there's no valid digest.

Note: this implicitly finishes the digest calulation (you can no longer call
`mediacheck_digest_process` on `digest`).

### Reference digest (the expected value) as hex string

```
char *mediacheck_digest_hex_ref(mediacheck_digest_t *digest);
```

Never returns NULL but possibly "" (empty string) if there's no reference
digest (last argument to `mediacheck_digest_init`).
