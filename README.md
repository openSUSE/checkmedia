# checkmedia

## Adding digests to iso images

Supported digests are: md5, sha1, sha224, sha256, sha384, sha512.

There are 512 bytes reserved for application specific use in the iso header
at offset 0x373 (cf. application_data in struct iso_primary_descriptor in
/usr/include/linux/iso_fs.h). We're free to do anything with it.

mkisofs fills it with spaces (512 x ' '), there's no mkisofs command line
option to set the field to some other value.

'tagmedia' calculates the digest and puts a line like '<DIGEST>sum=<HEX_DIGEST>'
into the field.

'checkmedia' calculates the digest but assumes to be spaces in the range
0x8373-0x8572 of the iso image (iso header starts at 0x8000) and compares
the result against the stored digest.

To avoid problems with isohybrid images, checkmedia also does not check the
first 512 bytes of the iso image (isohybrid writes an MBR there).

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
