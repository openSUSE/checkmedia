CC           = gcc
CFLAGS       = -g -O2 -Wall $(RPM_OPT_FLAGS)
LDFLAGS      = -L. -lmediacheck
SHARED_FLAGS = -fPIC -fvisibility=hidden

ARCH    := $(shell uname -m)
GIT2LOG := $(shell if [ -x ./git2log ] ; then echo ./git2log --update ; else echo true ; fi)
GITDEPS := $(shell [ -d .git ] && echo .git/HEAD .git/refs/heads .git/refs/tags)
VERSION := $(shell $(GIT2LOG) --version VERSION ; cat VERSION)
BRANCH  := $(shell [ -d .git ] && git branch | perl -ne 'print $$_ if s/^\*\s*//')
PREFIX  := checkmedia-$(VERSION)

MAJOR_VERSION := $(shell $(GIT2LOG) --version VERSION ; cut -d . -f 1 VERSION)

LIB_NAME     = libmediacheck
LIB_FILENAME = $(LIB_NAME).so.$(VERSION)
LIB_SONAME   = $(LIB_NAME).so.$(MAJOR_VERSION)

DIGEST_SRC  = $(wildcard md5.c sha*.c)
DIGEST_OBJ  = $(DIGEST_SRC:.c=.o)

LIBDIR = /usr/lib$(shell ldd /bin/sh | grep -q /lib64/ && echo 64)

.PHONY: all doc clean install test archive

all: checkmedia digestdemo

checkmedia: checkmedia.c $(LIB_FILENAME)
	$(CC) $(CFLAGS) checkmedia.c $(LDFLAGS) -DVERSION=\"$(VERSION)\" -o $@

digestdemo: digestdemo.c $(LIB_FILENAME)
	$(CC) $(CFLAGS) digestdemo.c $(LDFLAGS) -o $@

mediacheck.o: mediacheck.c mediacheck.h
	$(CC) -c $(CFLAGS) $(SHARED_FLAGS) -o $@ $<

$(DIGEST_OBJ): %.o: %.c %.h
	$(CC) -c $(CFLAGS) $(SHARED_FLAGS) -o $@ $<

$(LIB_FILENAME): $(DIGEST_OBJ) mediacheck.o
	$(CC) -shared -Wl,-soname,$(LIB_SONAME) mediacheck.o $(DIGEST_OBJ) -o $(LIB_FILENAME)
	@ln -snf $(LIB_FILENAME) $(LIB_SONAME)
	@ln -snf $(LIB_SONAME) $(LIB_NAME).so

changelog: $(GITDEPS)
	$(GIT2LOG) --changelog changelog

test: checkmedia
	./testmediacheck

install: checkmedia
	@cp tagmedia tagmedia.tmp
	@perl -pi -e 's/0\.0/$(VERSION)/ if /VERSION = /' tagmedia.tmp
	install -m 755 -D tagmedia.tmp $(DESTDIR)/usr/bin/tagmedia
	@rm -f tagmedia.tmp
	install -m 755 -D checkmedia $(DESTDIR)/usr/bin
	install -D $(LIB_FILENAME) $(DESTDIR)$(LIBDIR)/$(LIB_FILENAME)
	ln -snf $(LIB_FILENAME) $(DESTDIR)$(LIBDIR)/$(LIB_SONAME)
	ln -snf $(LIB_SONAME) $(DESTDIR)$(LIBDIR)/$(LIB_NAME).so
	install -m 644 -D mediacheck.h $(DESTDIR)/usr/include/mediacheck.h

%.1: %_man.adoc
	asciidoctor -b manpage -a version=$(VERSION) -a soversion=${MAJOR_VERSION} $<

README.html: README.adoc
	asciidoctor -b html -a version=$(VERSION) -a soversion=${MAJOR_VERSION} $<

doc: tagmedia.1 checkmedia.1 README.html

archive: changelog
	@if [ ! -d .git ] ; then echo no git repo ; false ; fi
	mkdir -p package
	git archive --prefix=$(PREFIX)/ $(BRANCH) > package/$(PREFIX).tar
	tar -r -f package/$(PREFIX).tar --mode=0664 --owner=root --group=root --mtime="`git show -s --format=%ci`" --transform='s:^:$(PREFIX)/:' VERSION changelog
	xz -f package/$(PREFIX).tar

clean:
	rm -rf *.o *.so *.so.* *.1 *.html package checkmedia digestdemo *~ */*~ tests/*.{img,check,tag}
