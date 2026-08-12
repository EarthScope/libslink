
# Build environment can be configured with the same environment
# variables as the top-level Makefile:
#   CC, CFLAGS, LDFLAGS, CPPFLAGS
#
# To run under a sanitizer:
#   cd .. && make clean && CFLAGS="-fsanitize=address,undefined -g -O1" make
#   cd tests && make clean && CFLAGS="-fsanitize=address,undefined -g -O1" make test

CFLAGS += -I.. -I../mbedtls/include -Wall -Wextra -Wno-sign-compare -g
LDFLAGS += -L..
LDLIBS = -lslink

# Auto-generated per-binary header dependencies (see the -include below),
# so a change to a header these tests include directly (slt.h,
# fixtures.h, ../libslink.h, ...) triggers a rebuild instead of `make`
# seeing an up-to-date-looking binary.
DEPFLAGS = -MMD -MP -MF $@.d

C_BINARIES = test_genutils test_globmatch test_payload test_slcd \
             test_streams test_statefile test_logging test_internals \
             test_netprims test_network

all: $(C_BINARIES) slharness

# Binaries that need fixtures.c (synthetic miniSEED builders) linked in.
test_payload: test_payload.c fixtures.c fixtures.h ../libslink.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -o $@ test_payload.c fixtures.c $(LDFLAGS) $(LDLIBS)

test_streams: test_streams.c fixtures.c fixtures.h ../libslink.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -o $@ test_streams.c fixtures.c $(LDFLAGS) $(LDLIBS)

test_statefile: test_statefile.c fixtures.c fixtures.h ../libslink.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -o $@ test_statefile.c fixtures.c $(LDFLAGS) $(LDLIBS)

# #includes ../slutils.c directly to reach its file-static helpers; see
# the comment at the top of test_internals.c for why this does not
# duplicate symbols with slutils.o in the archive.
test_internals: test_internals.c fixtures.c fixtures.h ../slutils.c ../libslink.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -o $@ test_internals.c fixtures.c $(LDFLAGS) $(LDLIBS)

# #includes ../network.c directly to reach its file-static helpers; see
# the comment at the top of test_internals.c for why this does not
# duplicate symbols with network.o in the archive.
test_network: test_network.c ../network.c ../libslink.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -o $@ test_network.c $(LDFLAGS) $(LDLIBS)

# Everything else is a single source file against the standard library.
test_genutils test_globmatch test_slcd test_logging test_netprims slharness : % : %.c ../libslink.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

# No prerequisites are listed here on purpose: once ../libslink.a exists,
# plain file-timestamp comparison can't tell whether the library sources
# (or a header they include) have changed since. Marking this phony
# always re-invokes the parent build, which does its own -MMD/-MP-based
# staleness check and only actually recompiles/relinks what changed.
.PHONY: ../libslink.a
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
	@$(RM) $(addsuffix .d,$(C_BINARIES) slharness)
	@$(RM) -r __pycache__ slmock/__pycache__
	@$(RM) -rf *.dSYM
	@echo "Test binaries clean."

.PHONY: all test check clean

-include $(addsuffix .d,$(C_BINARIES) slharness)
