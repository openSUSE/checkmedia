# checkmedia

## About

`checkmedia` is a tool to verify SUSE installation media. For this, the
digest (e.g. sha256) is stored within the media images and used to check against
the calculated digest.

Supported digests are: md5, sha1, sha224, sha256, sha384, sha512.

There are 512 bytes reserved for application specific use in the iso header
at offset 0x373 (cf. application_data in struct iso_primary_descriptor in
/usr/include/linux/iso_fs.h). We're free to do anything with it.

`tagmedia` calculates the digest and puts a line like `<DIGEST>sum=<HEX_DIGEST>`
into the field.

`checkmedia` calculates the digest but assumes to be spaces in the range
0x8373-0x8572 of the iso image (iso header starts at 0x8000) and compares
the result against the stored digest.

To avoid problems with isohybrid images, `checkmedia` also does not check the
first 512 bytes of the iso image (isohybrid writes an MBR there).

If a signature block is present the block itself is also exluded from
digest calculation.

The actual verification process is done by a separate [libmediacheck](mediacheck.md) library.

## Signing media

On the latest SUSE media the application_data block with the tags described
above can be signed. This allows checkmedia to ensure the media integrity by
also verifying this signature.

For this, a tag 'signature' is added pointing to a 2 kiB block to be used
for the gpg signature of the 512 bytes application_data block. The tag is
automatically added during digest calculation (`tagmedia --digest`). But you need to
add the actual signature later.

To create signed media, use `tagmedia --export-tags foo` to export the tag
block to file `foo`. Then create a detached signature with gpg (`foo.asc`)
and add the signature to the medium with `tagmedia --import-signature foo.asc`.

For the verification, the public keys in `/usr/lib/rpm/gnupg/keys` are used. Or
specify the public gpg key to use with the `--key-file` option to checkmedia.

## Examples

Calulate sha256 digest and store in `foo.iso`. Assume 150 sectors (of 2 kiB) padding in iso image:

```sh
tagmedia --digest sha256 --pad 150 foo.iso
```

Verify signed Tumbleweed iso, with output:

```sh
checkmedia openSUSE-Tumbleweed-NET-x86_64-Snapshot20190708-Media.iso
        app: openSUSE-Tumbleweed-NET-x86_64-Build1406.1-Media
   iso size: 132056 kB
        pad: 300 kB
  partition: start 4038 kB, size 128058 kB
   checking: 100%
     result: iso sha256 ok, partition sha256 ok
     sha256: 62b15f25b231f22ee93d576a6c9527ff7209ff715628a43b265fd61837f412e4
  signature: ok
```

Verify `foo.iso` and show more detailed information, including the actual gpg output from
signature verification:

```sh
checkmedia --verbose foo.iso
```


## Downloads

Get the latest version from the [openSUSE Build Service](https://software.opensuse.org/package/checkmedia).


## openSUSE Development

To build, simply run `make`. Install with `make install`.

Basically every new commit into the master branch of the repository will be auto-submitted
to all current SUSE products. No further action is needed except accepting the pull request.

Submissions are managed by a SUSE internal [jenkins](https://jenkins.io) node in the InstallTools tab.

Each time a new commit is integrated into the master branch of the repository,
a new submit request is created to the openSUSE Build Service. The devel project
is [system:install:head](https://build.opensuse.org/package/show/system:install:head/checkmedia).

`*.changes` and version numbers are auto-generated from git commits, you don't have to worry about this.

The spec file is maintained in the Build Service only. If you need to change it for the `master` branch,
submit to the
[devel project](https://build.opensuse.org/package/show/system:install:head/checkmedia)
in the build service directly.

Development happens exclusively in the `master` branch. The branch is used for all current products.

You can find more information about the changes auto-generation and the
tools used for jenkis submissions in the [linuxrc-devtools
documentation](https://github.com/openSUSE/linuxrc-devtools#opensuse-development).
