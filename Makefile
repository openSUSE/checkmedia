CC      = gcc
CFLAGS  = -c -g -O2 -Wall
LDFLAGS =

SRC     = $(wildcard *.c)
OBJ     = $(SRC:.c=.o)

%.o:    %.c
	$(CC) $(CFLAGS) -o $@ $<

all: checkmedia

checkmedia: $(OBJ)
	$(CC) $(OBJ) $(LDFLAGS) -o $@

clean:
	rm -f $(OBJ) checkmedia *~
