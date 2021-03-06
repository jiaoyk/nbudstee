prefix ?= /usr/local

all: nbudstee

VERSION_STRING := $(shell cat version 2>/dev/null || git describe --tags --always --dirty=-m 2>/dev/null || date "+%F %T %z" 2>/dev/null)
ifdef VERSION_STRING
CVFLAGS := -DVERSION_STRING='"${VERSION_STRING}"'
endif

CXXFLAGS ?= -Wall -Wextra -Wno-unused-parameter -O3 -g
LDFLAGS ?=
CPPFLAGS += -D_FILE_OFFSET_BITS=64
CXXFLAGS += -std=c++11

nbudstee: nbudstee.cpp
	g++ nbudstee.cpp $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) $(CVFLAGS) $(AFLAGS) -o nbudstee $(LOADLIBES) $(LDLIBS)

.PHONY: all install uninstall clean dumpversion

dumpversion:
	@echo $(VERSION_STRING)

clean:
	rm -f nbudstee nbudstee.1

install: nbudstee
	install -D -m 755 nbudstee $(DESTDIR)$(prefix)/bin/nbudstee

uninstall:
	rm -f $(DESTDIR)$(prefix)/bin/nbudstee $(DESTDIR)$(prefix)/share/man/man1/nbudstee.1

HELP2MANOK := $(shell help2man --version 2>/dev/null)
ifdef HELP2MANOK
all: nbudstee.1

nbudstee.1: nbudstee
	help2man -s 1 -N ./nbudstee -n "Non-Blocking Unix Domain Socket Tee" -o nbudstee.1

install: install-man

.PHONY: install-man

install-man: nbudstee.1
	install -D -m 644 nbudstee.1 $(DESTDIR)$(prefix)/share/man/man1/nbudstee.1
	-mandb -pq

else
$(shell echo "Install help2man for man page generation" >&2)
endif
