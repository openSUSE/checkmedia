Adding digests to iso images
-----------------------------

Suported digests are: md5, sha1, sha224, sha256, sha384, sha512.

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

