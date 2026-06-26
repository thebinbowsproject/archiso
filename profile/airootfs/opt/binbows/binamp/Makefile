CC     = gcc
CFLAGS = $(shell pkg-config --cflags gtk+-3.0 gstreamer-1.0 gstreamer-pbutils-1.0) -Wall -Wextra -O2
LIBS   = $(shell pkg-config --libs   gtk+-3.0 gstreamer-1.0 gstreamer-pbutils-1.0)

binamp: binamp.c
	$(CC) -o binamp binamp.c $(CFLAGS) $(LIBS)

clean:
	rm -f binamp
