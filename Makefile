all: checkmedia

checkmedia: checkmedia.c md5.o
	gcc -Wall -O2 $< md5.o -o $@

md5.o: md5.c
	gcc -Wall -O2 -c $<

clean:
	rm -f checkmedia *.o *~
