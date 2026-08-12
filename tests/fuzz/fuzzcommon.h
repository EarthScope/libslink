/***************************************************************************
 * fuzzcommon.h: shared helpers for the mutation-based dynamic-testing
 * drivers in this directory. Header-only, no dependencies beyond libc.
 *
 * These drivers are not part of the regular `make test` suite -- they are
 * long-running by design, meant to be built and run by hand (or in CI as
 * a separate time-boxed job) under a sanitizer to look for memory-safety
 * and undefined-behavior bugs that reading alone cannot guarantee to find.
 * Each takes an optional iteration count and an optional seed on argv, so
 * a crash can be reproduced deterministically by rerunning with the same
 * seed (printed at start) rather than relying on the dumped input alone.
 ***************************************************************************/

#ifndef FUZZCOMMON_H
#define FUZZCOMMON_H 1

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* xorshift64*, adequate for generating varied test input; not
 * cryptographic and not meant to be. */
static uint64_t fz_rng_state = 0x9e3779b97f4a7c15ULL;

static void
fz_seed (uint64_t seed)
{
  fz_rng_state = seed ? seed : 1;
}

static uint64_t
fz_rand (void)
{
  fz_rng_state ^= fz_rng_state << 13;
  fz_rng_state ^= fz_rng_state >> 7;
  fz_rng_state ^= fz_rng_state << 17;
  return fz_rng_state;
}

/* Returns a value in [0, bound). */
static uint64_t
fz_rand_below (uint64_t bound)
{
  return bound ? (fz_rand () % bound) : 0;
}

/* Not every driver uses every helper below; __attribute__((unused))
 * silences -Wunused-function for whichever ones a given driver doesn't
 * call, rather than splitting this header up per-driver. */
static void __attribute__ ((unused))
fz_random_bytes (uint8_t *buf, size_t len)
{
  size_t i;
  for (i = 0; i < len; i++)
    buf[i] = (uint8_t)(fz_rand () & 0xff);
}

/* Flips a handful of random bytes in an otherwise-valid buffer, so
 * mutation reaches deeper into a parser than uniform random noise would
 * on its own -- structure-aware corruption of a real record exercises
 * length/offset fields specifically, which is where the bugs are. */
static void __attribute__ ((unused))
fz_mutate (uint8_t *buf, size_t len)
{
  int flips = 1 + (int)fz_rand_below (8);
  int i;

  if (len == 0)
    return;

  for (i = 0; i < flips; i++)
    buf[fz_rand_below (len)] = (uint8_t)(fz_rand () & 0xff);
}

/* Parses "--iterations N" and "--seed N" from argv, with the given
 * defaults; unrecognized arguments are ignored. */
static void
fz_parse_args (int argc, char **argv, long *iterations, uint64_t *seed)
{
  int i;

  for (i = 1; i < argc; i++)
  {
    if (strcmp (argv[i], "--iterations") == 0 && i + 1 < argc)
      *iterations = strtol (argv[++i], NULL, 10);
    else if (strcmp (argv[i], "--seed") == 0 && i + 1 < argc)
      *seed = (uint64_t)strtoull (argv[++i], NULL, 10);
  }
}

#endif /* FUZZCOMMON_H */
