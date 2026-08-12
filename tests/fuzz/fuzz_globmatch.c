/***************************************************************************
 * fuzz_globmatch.c: mutation-based dynamic testing of sl_globmatch()
 * (globmatch.c) against randomly generated patterns and strings, biased
 * toward glob metacharacters to actually exercise the backtracking and
 * character-class parsing rather than mostly hitting the literal-compare
 * fast path. Build and run by hand under a sanitizer; not part of
 * `make test`.
 ***************************************************************************/

#include "../../globmatch.h"
#include "fuzzcommon.h"

#include <inttypes.h>
#include <time.h>

#define MAX_LEN 256

/* Heavily weighted toward glob metacharacters and bracket-class syntax,
 * with a few plain letters/digits mixed in -- pure random bytes rarely
 * form a `[...]` class at all, so they'd mostly exercise the literal
 * fast path instead of the backtracking/class-parsing logic. */
static const char METACHARS[] = "*?[]!^-\\abc123";

static void
generate (char *buf, size_t maxlen)
{
  size_t len = fz_rand_below (maxlen);
  size_t i;

  for (i = 0; i < len; i++)
    buf[i] = METACHARS[fz_rand_below (sizeof (METACHARS) - 1)];
  buf[len] = '\0';
}

int
main (int argc, char **argv)
{
  long iterations = 20000000;
  uint64_t seed = 0;
  char pattern[MAX_LEN];
  char string[MAX_LEN];
  long i;

  fz_parse_args (argc, argv, &iterations, &seed);
  if (seed == 0)
    seed = (uint64_t)time (NULL);
  fz_seed (seed);

  printf ("fuzz_globmatch: seed=%" PRIu64 " iterations=%ld\n", seed, iterations);
  fflush (stdout);

  for (i = 0; i < iterations; i++)
  {
    generate (pattern, sizeof (pattern) - 1);
    generate (string, sizeof (string) - 1);
    sl_globmatch (string, pattern);
  }

  printf ("fuzz_globmatch: survived %ld iterations\n", iterations);
  return 0;
}
