
# Build environment can be configured the following
# environment variables:
#   CC : Specify the C compiler to use
#   CFLAGS : Specify compiler options to use
#   LDFLAGS : Specify linker options to use
#   CPPFLAGS : Specify c-preprocessor options to use

# Extract version from libslink.h
MAJOR_VER = $(shell grep -E 'LIBSLINK_VERSION_MAJOR[ \t]+[0-9]+' libslink.h | grep -Eo '[0-9]+')
MINOR_VER = $(shell grep -E 'LIBSLINK_VERSION_MINOR[ \t]+[0-9]+' libslink.h | grep -Eo '[0-9]+')
PATCH_VER = $(shell grep -E 'LIBSLINK_VERSION_PATCH[ \t]+[0-9]+' libslink.h | grep -Eo '[0-9]+')
COMPAT_VER = $(MAJOR_VER).0.0

# Default settings for install target
PREFIX ?= /usr/local
EXEC_PREFIX ?= $(PREFIX)
LIBDIR ?= $(EXEC_PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include
DATAROOTDIR ?= $(PREFIX)/share
DOCDIR ?= $(DATAROOTDIR)/doc/libslink
MANDIR ?= $(DATAROOTDIR)/man
MAN3DIR ?= $(MANDIR)/man3

LIB_SRCS = gswap.c payload.c genutils.c strutils.c \
           logging.c network.c statefile.c config.c \
           globmatch.c slutils.c

LIB_OBJS = $(LIB_SRCS:.c=.o)
LIB_LOBJS = $(LIB_SRCS:.c=.lo)

LIB_NAME = libslink
LIB_A = $(LIB_NAME).a

CFLAGS += -Imbedtls/include

OS := $(shell uname -s)
ROOT_DIR := $(dir $(realpath $(lastword $(MAKEFILE_LIST))))

# Build dynamic (.dylib) on macOS/Darwin, otherwise shared (.so)
ifeq ($(OS), Darwin)
	LIB_SO_BASE = $(LIB_NAME).dylib
	LIB_SO_MAJOR = $(LIB_NAME).$(MAJOR_VER).dylib
	LIB_SO = $(LIB_NAME).$(MAJOR_VER).$(MINOR_VER).$(PATCH_VER).dylib
	LIB_OPTS = -dynamiclib -compatibility_version $(COMPAT_VER) -current_version $(MAJOR_VER).$(MINOR_VER).$(PATCH_VER) -install_name $(LIB_SO)
else
	LIB_SO_BASE = $(LIB_NAME).so
	LIB_SO_MAJOR = $(LIB_NAME).so.$(MAJOR_VER)
	LIB_SO = $(LIB_NAME).so.$(MAJOR_VER).$(MINOR_VER).$(PATCH_VER)
	LIB_OPTS = -shared -Wl,--version-script=libslink.map -Wl,-soname,$(LIB_SO_MAJOR)
endif

all: static

static: $(LIB_A)

shared dynamic: $(LIB_SO)

# Build static library
$(LIB_A): $(LIB_OBJS) mbedtls
	@echo "Building static library $(LIB_A)"
	$(RM) -f $(LIB_A)
	$(AR) -crs $(LIB_A) $(LIB_OBJS) $(wildcard mbedtls/library/*.o)

# Build shared/dynamic library
$(LIB_SO): $(LIB_LOBJS) mbedtls
	@echo "Building shared library $(LIB_SO)"
	$(RM) -f $(LIB_SO) $(LIB_SO_MAJOR) $(LIB_SO_BASE)
	$(CC) $(CFLAGS) $(LDFLAGS) $(LIB_OPTS) -o $(LIB_SO) $(LIB_LOBJS) $(wildcard mbedtls/library/*.o)
	ln -s $(LIB_SO) $(LIB_SO_BASE)
	ln -s $(LIB_SO) $(LIB_SO_MAJOR)

# Build mbedtls library for needed objects
# Set modification times of mbedtls source files to containing directory
# to avoid triggering generation of build files.
mbedtls: CFLAGS += -fPIC
mbedtls: FORCE
	@find mbedtls -type f -exec touch -c -m -r mbedtls {} \;
	@echo "Building mbedtls"
	@$(MAKE) GEN_FILES= -C mbedtls lib

test check: static FORCE
	@$(MAKE) -C test test

clean:
	@$(RM) $(LIB_OBJS) $(LIB_LOBJS) $(LIB_A) $(LIB_SO) $(LIB_SO_MAJOR) $(LIB_SO_BASE)
	@$(MAKE) -C mbedtls clean
	@echo "All clean."

install: shared
	@echo "Installing into $(PREFIX)"
	@mkdir -p $(DESTDIR)$(PREFIX)/include
	@cp libslink.h $(DESTDIR)$(PREFIX)/include
	@mkdir -p $(DESTDIR)$(LIBDIR)/pkgconfig
	@cp -a $(LIB_SO_BASE) $(LIB_SO_MAJOR) $(LIB_SO_NAME) $(LIB_SO) $(DESTDIR)$(LIBDIR)
	@sed -e 's|@PREFIX@|$(PREFIX)|g' \
	     -e 's|@EXEC_PREFIX@|$(EXEC_PREFIX)|g' \
	     -e 's|@LIBDIR@|$(LIBDIR)|g' \
	     -e 's|@INCLUDEDIR@|$(PREFIX)/include|g' \
	     -e 's|@VERSION@|$(MAJOR_VER).$(MINOR_VER).$(PATCH_VER)|g' \
	     slink.pc.in > $(DESTDIR)$(LIBDIR)/pkgconfig/slink.pc
	@mkdir -p $(DESTDIR)$(DOCDIR)/example
	@cp -r example $(DESTDIR)$(DOCDIR)/

.SUFFIXES: .c .o .lo

# Standard object building
.c.o:
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# Standard object building for shared library using -fPIC
.c.lo:
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -c $< -o $@

FORCE:
