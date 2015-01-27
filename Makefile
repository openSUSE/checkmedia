CC      = gcc
CFLAGS  = -c -g -O2 -Wall
LDFLAGS =

SRC     = $(wildcard *.c)
OBJ     = $(SRC:.c=.o)

GIT2LOG := $(shell if [ -x ./git2log ] ; then echo ./git2log --update ; else echo true ; fi)
GITDEPS := $(shell [ -d .git ] && echo .git/HEAD .git/refs/heads .git/refs/tags)
VERSION := $(shell $(GIT2LOG) --version VERSION ; cat VERSION)
BRANCH  := $(shell [ -d .git ] && git branch | perl -ne 'print $$_ if s/^\*\s*//')
PREFIX  := checkmedia-$(VERSION)

%.o: %.c
	$(CC) $(CFLAGS) -DVERSION=\"$(VERSION)\" -o $@ $<

all: checkmedia

checkmedia: $(OBJ)
	$(CC) $(OBJ) $(LDFLAGS) -o $@

changelog: $(GITDEPS)
	$(GIT2LOG) --changelog changelog

install: checkmedia
	install -m 755 -D checkmedia tagmedia $(DESTDIR)/usr/bin

archive: changelog
	@if [ ! -d .git ] ; then echo no git repo ; false ; fi
	mkdir -p package
	git archive --prefix=$(PREFIX)/ $(BRANCH) > package/$(PREFIX).tar
	tar -r -f package/$(PREFIX).tar --mode=0664 --owner=root --group=root --mtime="`git show -s --format=%ci`" --transform='s:^:$(PREFIX)/:' VERSION changelog
	xz -f package/$(PREFIX).tar

clean:
	rm -rf $(OBJ) package checkmedia *~
