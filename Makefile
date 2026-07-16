include config.mk

hum: hum.c config.h
	$(CC) $(CFLAGS) -o $@ hum.c $(LDFLAGS)

clean:
	rm -f hum

install: hum
	mkdir -p $(PREFIX)/bin
	install -m 755 hum $(PREFIX)/bin/hum
	mkdir -p $(MANPREFIX)/man1
	install -m 644 hum.1 $(MANPREFIX)/man1/hum.1

uninstall:
	rm -f $(PREFIX)/bin/hum
	rm -f $(MANPREFIX)/man1/hum.1

.PHONY: clean install uninstall
