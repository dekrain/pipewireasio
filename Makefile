#!/usr/bin/make -f
# Makefile for PipeWireASIO #
# --------------------- #
# Created by falkTX
# Contributed by DeKrain
#

VERSION = 0.2.2

PREFIX ?= /usr

all:
	@echo "error: you must pass '32' or '64' as an argument to this Makefile in order to build PipeWireASIO"

# ---------------------------------------------------------------------------------------------------------------------

32:
	$(MAKE) build ARCH=i386 M=32

64:
	$(MAKE) build ARCH=x86_64 M=64

install32:
	$(MAKE) install ARCH=i386 M=32

install64:
	$(MAKE) install ARCH=x86_64 M=64

register32:
	$(MAKE) register ARCH=i386 M=32

register64:
	$(MAKE) register ARCH=x86_64 M=64

ifeq ($(M),)
install: install32 install64
register: register32 register64
endif

# ---------------------------------------------------------------------------------------------------------------------

clean:
	rm -f *.o *.so
	rm -rf build build32 build64
	rm -rf gui/__pycache__ new_gui/__pycache__

# ---------------------------------------------------------------------------------------------------------------------

tarball: clean
	rm -f ../pipewireasio-$(VERSION).tar.gz
	tar -c -z \
		--exclude=".git*" \
		--exclude=".travis*" \
		--exclude=debian \
		--exclude=prepare_64bit_asio.sh \
		--exclude=rtaudio/cmake \
		--exclude=rtaudio/contrib \
		--exclude=rtaudio/doc \
		--exclude=rtaudio/tests \
		--exclude=rtaudio/"*.ac" \
		--exclude=rtaudio/"*.am" \
		--exclude=rtaudio/"*.in" \
		--exclude=rtaudio/"*.sh" \
		--exclude=rtaudio/"*.txt" \
		--transform='s,^\.,pipewireasio-$(VERSION),' \
		-f ../pipewireasio-$(VERSION).tar.gz .

# ---------------------------------------------------------------------------------------------------------------------

ifneq ($(ARCH),)
ifneq ($(M),)
include Makefile.mk
endif
endif

# ---------------------------------------------------------------------------------------------------------------------
