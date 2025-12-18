# Punto Switcher for Linux (C version)
# Interception Tools plugin

CC = gcc
CFLAGS = -O2 -Wall -Wextra -std=c11
LDFLAGS = 

SRC = src/punto.c
TARGET = punto

PREFIX = /usr/local
BINDIR = $(PREFIX)/bin
CONFDIR = /etc/punto

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -Dm755 punto-switch $(DESTDIR)$(BINDIR)/punto-switch
	install -Dm755 punto-invert $(DESTDIR)$(BINDIR)/punto-invert
	install -d $(DESTDIR)$(CONFDIR)
	@if [ ! -f $(DESTDIR)$(CONFDIR)/config.yaml ]; then \
		install -Dm644 config.yaml $(DESTDIR)$(CONFDIR)/config.yaml; \
	else \
		echo "Config exists, skipping (backup: config.yaml.new)"; \
		install -Dm644 config.yaml $(DESTDIR)$(CONFDIR)/config.yaml.new; \
	fi

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	rm -f $(DESTDIR)$(BINDIR)/punto-switch
	rm -f $(DESTDIR)$(BINDIR)/punto-invert
	@echo "Config at $(CONFDIR) preserved. Remove manually if needed."


# For testing
test: $(TARGET)
	@echo "Run: sudo intercept -g /dev/input/eventX | ./punto | uinput -d /dev/input/eventX"
	@echo "Replace eventX with your keyboard device (check with: cat /proc/bus/input/devices)"
