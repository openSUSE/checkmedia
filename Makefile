CC           = gcc
CFLAGS       = -g -O2 -Wall
LDFLAGS      = -L. -lmediacheck
SHARED_FLAGS = -fPIC -fvisibility=hidden

SRC_LIB     = $(wildcard mediacheck.c md5.c sha*.c)
OBJ_LIB     = $(SRC_LIB:.c=.o)

GIT2LOG := $(shell if [ -x ./git2log ] ; then echo ./git2log --update ; else echo true ; fi)
GITDEPS := $(shell [ -d .git ] && echo .git/HEAD .git/refs/heads .git/refs/tags)
VERSION := $(shell $(GIT2LOG) --version VERSION ; cat VERSION)
BRANCH  := $(shell [ -d .git ] && git branch | perl -ne 'print $$_ if s/^\*\s*//')
PREFIX  := checkmedia-$(VERSION)

MAJOR_VERSION := $(shell $(GIT2LOG) --version VERSION ; cut -d . -f 1 VERSION)

ifneq ($(filter x86_64, $(ARCH)),)
LIBDIR  = /usr/lib64
else
LIBDIR  = /usr/lib
endif
LIB_NAME  = libmediacheck

LIB_FILENAME = $(LIB_NAME).so.$(VERSION)
LIB_SONAME   = $(LIB_NAME).so.$(MAJOR_VERSION)

%.o: %.c mediacheck.h
	$(CC) -c $(CFLAGS) $(SHARED_FLAGS) -DVERSION=\"$(VERSION)\" -o $@ $<

all: checkmedia

checkmedia: checkmedia.c $(LIB_FILENAME) mediacheck.h
	echo $(LIB_FILENAME)
	$(CC) $(CFLAGS) $(LDFLAGS) checkmedia.c -DVERSION=\"$(VERSION)\" -o $@

$(LIB_FILENAME): $(OBJ_LIB) mediacheck.h
	$(CC) -shared -Wl,-soname,$(LIB_SONAME) $(OBJ_LIB) -o $(LIB_FILENAME)
	@ln -snf $(LIB_FILENAME) $(LIB_SONAME)
	@ln -snf $(LIB_FILENAME) $(LIB_NAME).so

changelog: $(GITDEPS)
	$(GIT2LOG) --changelog changelog

install: checkmedia
	install -m 755 -D checkmedia tagmedia $(DESTDIR)/usr/bin
	install -D $(LIB_FILENAME) $(DESTDIR)$(LIBDIR)/$(LIB_FILENAME)
	ln -snf $(LIB_FILENAME) $(DESTDIR)$(LIBDIR)/$(LIB_SONAME)
	ln -snf $(LIB_SONAME) $(DESTDIR)$(LIBDIR)/$(LIB_NAME).so
	install -m 644 -D mediacheck.h $(DESTDIR)/usr/include/mediacheck.h


archive: changelog
	@if [ ! -d .git ] ; then echo no git repo ; false ; fi
	mkdir -p package
	git archive --prefix=$(PREFIX)/ $(BRANCH) > package/$(PREFIX).tar
	tar -r -f package/$(PREFIX).tar --mode=0664 --owner=root --group=root --mtime="`git show -s --format=%ci`" --transform='s:^:$(PREFIX)/:' VERSION changelog
	xz -f package/$(PREFIX).tar

clean:
	rm -rf *.o *.so *.so.* package checkmedia *~
