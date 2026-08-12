/***************************************************************************
 * fuzz_statefile.c: mutation-based dynamic testing of sl_recoverstate()
 * (statefile.c) against malformed and randomly generated state-file
 * content, covering both the legacy and "V2" line formats plus pure
 * random/binary garbage. Build and run by hand under a sanitizer; not
 * part of `make test`.
 *
 * Writes generated content to a single reused file rather than a fresh
 * mkstemp() path per iteration, since the per-iteration file I/O this
 * target requires (sl_recoverstate() takes a path, not a buffer) is
 * already the throughput bottleneck without adding tempfile churn on
 * top of it.
 ***************************************************************************/

#include "../../libslink.h"
#include "fuzzcommon.h"

#include <inttypes.h>
#include <time.h>

#define STATEFILE_PATH "/tmp/fuzz_statefile_input.txt"
#define MAX_LINE 512
#define MAX_LINES 40

static const char *const STATION_TOKENS[] = {
    "XX_TEST", "IU_COLA", "*", "XX", "UNI", "TOOLONGSTATIONIDENTIFIERVALUE",
};

static const char *const SEQ_TOKENS[] = {
    "0", "1234567890", "UNSET", "-1", "99999999999999999999", "not-a-number", "",
};

static const char *const TIMESTAMP_TOKENS[] = {
    "2024-08-03T17:23:18.0Z",
    "2021,11,19,17,23,18",
    "2024-13-99T99:99:99.0Z",
    "not-a-timestamp",
    "2024-08-03T17:23:18.000000000000000000000000000000Z",
    "",
};

static const char *
random_of (const char *const *tokens, size_t count)
{
  return tokens[fz_rand_below (count)];
}

/* Fills buf with one iteration's worth of generated file content: a mix
 * of a V2 header, legacy/V2-shaped lines with randomly chosen (and
 * sometimes mismatched) field counts, comment/blank lines, and raw
 * random bytes -- possibly including embedded NULs and lines far longer
 * than statefile.c's 200-byte line buffer. Returns the length written. */
static size_t
generate_content (uint8_t *buf, size_t bufcap)
{
  size_t pos = 0;
  int nlines = 1 + (int)fz_rand_below (MAX_LINES);
  int has_v2_header = (int)fz_rand_below (2);
  int i;

  if (has_v2_header)
    pos += (size_t)snprintf ((char *)buf + pos, bufcap - pos, "#V2 StationID  Sequence  [Timestamp]\n");

  for (i = 0; i < nlines && pos + MAX_LINE < bufcap; i++)
  {
    int kind = (int)fz_rand_below (5);

    switch (kind)
    {
    case 0: /* legacy-shaped: NET STA SEQ [TIMESTAMP] */
      pos += (size_t)snprintf ((char *)buf + pos, bufcap - pos, "%s %s %s %s\n",
                               random_of (STATION_TOKENS, sizeof (STATION_TOKENS) / sizeof (*STATION_TOKENS)),
                               random_of (STATION_TOKENS, sizeof (STATION_TOKENS) / sizeof (*STATION_TOKENS)),
                               random_of (SEQ_TOKENS, sizeof (SEQ_TOKENS) / sizeof (*SEQ_TOKENS)),
                               random_of (TIMESTAMP_TOKENS, sizeof (TIMESTAMP_TOKENS) / sizeof (*TIMESTAMP_TOKENS)));
      break;

    case 1: /* V2-shaped: StationID SEQ [TIMESTAMP] */
      pos += (size_t)snprintf ((char *)buf + pos, bufcap - pos, "%s %s %s\n",
                               random_of (STATION_TOKENS, sizeof (STATION_TOKENS) / sizeof (*STATION_TOKENS)),
                               random_of (SEQ_TOKENS, sizeof (SEQ_TOKENS) / sizeof (*SEQ_TOKENS)),
                               random_of (TIMESTAMP_TOKENS, sizeof (TIMESTAMP_TOKENS) / sizeof (*TIMESTAMP_TOKENS)));
      break;

    case 2: /* comment or blank */
      pos += (size_t)snprintf ((char *)buf + pos, bufcap - pos, "%s\n",
                               fz_rand_below (2) ? "# a comment" : "");
      break;

    case 3: /* a line far longer than the internal 200-byte line buffer */
    {
      size_t len = 200 + fz_rand_below (MAX_LINE - 200 - 1);
      size_t j;
      for (j = 0; j < len && pos + 1 < bufcap; j++, pos++)
        buf[pos] = (uint8_t)(fz_rand () & 0x7f); /* stay printable-ish, no embedded newline */
      if (pos < bufcap)
        buf[pos++] = '\n';
      break;
    }

    default: /* raw random bytes, possibly including embedded NULs */
    {
      size_t len = fz_rand_below (MAX_LINE);
      size_t j;
      for (j = 0; j < len && pos + 1 < bufcap; j++, pos++)
        buf[pos] = (uint8_t)(fz_rand () & 0xff);
      if (pos < bufcap)
        buf[pos++] = '\n';
      break;
    }
    }
  }

  return pos;
}

static void
write_statefile (const uint8_t *buf, size_t len)
{
  FILE *fp = fopen (STATEFILE_PATH, "wb");
  if (!fp)
    return;
  fwrite (buf, 1, len, fp);
  fclose (fp);
}

static SLCD *
make_slcd_with_streams (void)
{
  SLCD *slconn = sl_initslcd ("t", NULL);
  sl_add_stream (slconn, "XX_TEST", "BHZ", SL_UNSETSEQUENCE, NULL);
  sl_add_stream (slconn, "IU_COLA", NULL, SL_UNSETSEQUENCE, NULL);
  return slconn;
}

int
main (int argc, char **argv)
{
  long iterations = 200000;
  uint64_t seed = 0;
  uint8_t buf[4096];
  SLCD *slconn;
  long i;

  fz_parse_args (argc, argv, &iterations, &seed);
  if (seed == 0)
    seed = (uint64_t)time (NULL);
  fz_seed (seed);

  printf ("fuzz_statefile: seed=%" PRIu64 " iterations=%ld\n", seed, iterations);
  fflush (stdout);

  slconn = make_slcd_with_streams ();

  for (i = 0; i < iterations; i++)
  {
    size_t len = generate_content (buf, sizeof (buf));
    write_statefile (buf, len);
    sl_recoverstate (slconn, STATEFILE_PATH);
  }

  sl_freeslcd (slconn);
  remove (STATEFILE_PATH);

  printf ("fuzz_statefile: survived %ld iterations\n", iterations);
  return 0;
}
