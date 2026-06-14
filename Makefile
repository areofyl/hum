CC ?= cc
CFLAGS = -Wall -Wextra -Werror -Wno-format-truncation -O2
LDFLAGS = -lncurses -ltinfo
PREFIX ?= /usr/local

hum: hum.c config.h
	$(CC) $(CFLAGS) -o $@ hum.c $(LDFLAGS)

clean:
	rm -f hum

install: hum
	cp hum $(PREFIX)/bin/hum

.PHONY: clean install
