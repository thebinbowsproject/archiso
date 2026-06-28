CC     = gcc
CFLAGS = $(shell pkg-config --cflags gtk+-3.0) -Wall -Wextra -O2
LIBS   = $(shell pkg-config --libs   gtk+-3.0)

binbows-installer: binbows-installer.c
	$(CC) -o binbows-installer binbows-installer.c $(CFLAGS) $(LIBS)

clean:
	rm -f binbows-installer
