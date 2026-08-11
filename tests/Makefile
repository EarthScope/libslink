
# Build environment can be configured with the same environment
# variables as the top-level Makefile:
#   CC, CFLAGS, LDFLAGS, CPPFLAGS
#
# To run under a sanitizer:
#   cd .. && make clean && CFLAGS="-fsanitize=address,undefined -g -O1" make
#   cd tests && make clean && CFLAGS="-fsanitize=address,undefined -g -O1" make test

CFLAGS += -I.. -I../mbedtls/include -Wall -Wextra -g
LDFLAGS += -L..
LDLIBS = -lslink

C_BINARIES = test_genutils test_globmatch test_payload test_slcd \
             test_streams test_statefile test_logging test_internals \
             test_netprims test_network

all: $(C_BINARIES) slharness

# Binaries that need fixtures.c (synthetic miniSEED builders) linked in.
test_payload: test_payload.c fixtures.c fixtures.h ../libslink.a
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_payload.c fixtures.c $(LDFLAGS) $(LDLIBS)

test_streams: test_streams.c fixtures.c fixtures.h ../libslink.a
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_streams.c fixtures.c $(LDFLAGS) $(LDLIBS)

test_statefile: test_statefile.c fixtures.c fixtures.h ../libslink.a
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_statefile.c fixtures.c $(LDFLAGS) $(LDLIBS)

# #includes ../slutils.c directly to reach its file-static helpers; see
# the comment at the top of test_internals.c for why this does not
# duplicate symbols with slutils.o in the archive.
test_internals: test_internals.c fixtures.c fixtures.h ../slutils.c ../libslink.a
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_internals.c fixtures.c $(LDFLAGS) $(LDLIBS)

# #includes ../network.c directly to reach its file-static helpers; see
# the comment at the top of test_internals.c for why this does not
# duplicate symbols with network.o in the archive.
test_network: test_network.c ../network.c ../libslink.a
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_network.c $(LDFLAGS) $(LDLIBS)

# Everything else is a single source file against the standard library.
test_genutils test_globmatch test_slcd test_logging test_netprims slharness : % : %.c ../libslink.a
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

../libslink.a:
	$(MAKE) -C .. static

test check: all
	@if command -v python3 >/dev/null 2>&1; then \
	  python3 runtests.py $(ARGS); \
	else \
	  echo "python3 not found; cannot run tests/runtests.py"; \
	  exit 1; \
	fi

clean:
	@$(RM) $(C_BINARIES) slharness
	@$(RM) -r __pycache__ slmock/__pycache__
	@$(RM) -rf *.dSYM
	@echo "Test binaries clean."

.PHONY: all test check clean
