CC      = gcc
CFLAGS  = -c -g -O2 -Wall
LDFLAGS =

SRC     = $(wildcard *.c)
OBJ     = $(SRC:.c=.o)

%.o: %.c
	$(CC) $(CFLAGS) -o $@ $<

all: checkmedia

checkmedia: $(OBJ)
	$(CC) $(OBJ) $(LDFLAGS) -o $@

install: checkmedia
	install -m 755 -D checkmedia tagmedia $(DESTDIR)/usr/bin

clean:
	rm -f $(OBJ) checkmedia *~
