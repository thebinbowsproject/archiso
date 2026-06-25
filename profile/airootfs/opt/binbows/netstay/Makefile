CC     = gcc
CFLAGS = $(shell pkg-config --cflags gtk+-3.0 webkit2gtk-4.1) -Wall -Wextra -O2
LIBS   = $(shell pkg-config --libs   gtk+-3.0 webkit2gtk-4.1)

netstay: netstay.c
	$(CC) -o netstay netstay.c $(CFLAGS) $(LIBS)

clean:
	rm -f netstay
