.ONESHELL:
SHELL := /bin/sh

BIN_PATH := /usr/sbin
TARGET := scripts/*.sh

.PHONY: all build install uninstall clean

all: install

build:
	@:

install: build
	@install -d "$(DESTDIR)$(BIN_PATH)"
	for script in $(TARGET); do
		[ -f "$$script" ] || continue
		install -m 755 "$$script" "$(DESTDIR)$(BIN_PATH)/$$(basename "$$script")"
	done

uninstall:
	@for script in $(TARGET); do
		[ -f "$$script" ] || continue
		rm -f "$(DESTDIR)$(BIN_PATH)/$$(basename "$$script")"
	done

clean:
	@:
