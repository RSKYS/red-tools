.ONESHELL:
SHELL := /bin/sh

BIN_PATH := /usr/sbin
SCRIPT_DIR := scripts

.PHONY: all build install uninstall clean

all: install

build:
	@:

install: build
	@install -d "$(DESTDIR)$(BIN_PATH)"
	for script in $(SCRIPT_DIR)/*; do
		[ -f "$$script" ] || continue
		name=$${script##*/}
		basename=$${name%.*}
		case "$$name" in
			*.c)
				gcc "$$script" -o "$(DESTDIR)$(BIN_PATH)/$$basename"
				;;
			*.sh)
				install -m 755 "$$script" "$(DESTDIR)$(BIN_PATH)/$$basename"
				;;
		esac
	done

uninstall:
	@for script in $(SCRIPT_DIR)/*; do
		[ -f "$$script" ] || continue
		name=$${script##*/}
		name=$${name%.*}
		rm -f "$(DESTDIR)$(BIN_PATH)/$$name"
	done

clean:
	@:
