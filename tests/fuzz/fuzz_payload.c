/***************************************************************************
 * fuzz_payload.c: mutation-based dynamic testing of the public payload
 * metadata extraction functions in payload.c -- sl_payload_info() and
 * sl_payload_summary() -- against malformed and randomly mutated input,
 * mismatched buffer sizes, and out-of-range packetinfo fields. Build and
 * run by hand under a sanitizer; not part of `make test`.
 ***************************************************************************/

#include "../../libslink.h"
#include "../fixtures.h"
#include "fuzzcommon.h"

#include <inttypes.h>
#include <time.h>

#define MAX_BUF 4096

static void
build_seeds (uint8_t *seed2, uint8_t *seed3)
{
  MS2Fields f2;
  MS3Fields f3;

  memset (&f2, 0, sizeof (f2));
  f2.network = "XX";
  f2.station = "TEST";
  f2.channel = "BHZ";
  f2.year = 2024;
  f2.day = 216;
  f2.numblockettes = 1;
  f2.blocketteoffset = MS2_FIXED_LENGTH;
  memset (seed2, 0, MAX_BUF);
  fx_ms2_fixed (seed2, MAX_BUF, &f2, 0);
  fx_ms2_b1000 (seed2, MAX_BUF, MS2_FIXED_LENGTH, 11, 0, 9 /* 2^9 = 512 */, 0, 0);

  memset (&f3, 0, sizeof (f3));
  f3.sid = "FDSN:XX_TEST";
  f3.year = 2024;
  f3.day = 216;
  f3.samplerate = 20.0;
  f3.datalength = 200;
  memset (seed3, 0, MAX_BUF);
  fx_ms3_fixed (seed3, MAX_BUF, &f3, 0);
}

/* Payload formats worth exercising specifically, beyond pure-random bytes
 * in the field: the two recognized ones, and a handful the switch in
 * sl_payload_info() must reject cleanly. */
static char
random_payloadformat (void)
{
  static const char formats[] = {
      SLPAYLOAD_MSEED2, SLPAYLOAD_MSEED3, SLPAYLOAD_UNKNOWN,
      SLPAYLOAD_JSON,   SLPAYLOAD_XML,    'Z',
  };
  return formats[fz_rand_below (sizeof (formats))];
}

static void
run (long iterations, const uint8_t *seed2, const uint8_t *seed3)
{
  uint8_t plbuffer[MAX_BUF];
  char sourceid[128];
  char starttimestr[128];
  char summary[256];
  long i;

  for (i = 0; i < iterations; i++)
  {
    SLpacketinfo packetinfo;
    double samplerate = 0.0;
    uint32_t samplecount = 0;
    size_t len = fz_rand_below (MAX_BUF);
    int mode = (int)fz_rand_below (3);
    uint32_t plbuffer_size = (uint32_t)fz_rand_below (MAX_BUF + 64);
    /* Never exceeds the real destination size -- doing so would be a lie
     * to the callee about how much room it actually has, which is the
     * caller's responsibility to get right, not something this API can
     * defend against. Under-reporting (including 0) is fair game. */
    size_t sourceid_size = fz_rand_below (sizeof (sourceid) + 1);
    size_t starttimestr_size = fz_rand_below (sizeof (starttimestr) + 1);
    size_t summary_size = fz_rand_below (sizeof (summary) + 1);

    if (mode == 0)
      fz_random_bytes (plbuffer, len);
    else if (mode == 1)
    {
      memcpy (plbuffer, seed2, MAX_BUF);
      fz_mutate (plbuffer, len);
    }
    else
    {
      memcpy (plbuffer, seed3, MAX_BUF);
      fz_mutate (plbuffer, len);
    }

    memset (&packetinfo, 0, sizeof (packetinfo));
    packetinfo.payloadformat = random_payloadformat ();
    /* payloadlength deliberately not tied to plbuffer_size or len -- every
     * combination of "declared longer/shorter than the real buffer, and
     * longer/shorter than what's actually initialized" must be safe. */
    packetinfo.payloadlength = (uint32_t)fz_rand_below (MAX_BUF * 2);

    sl_payload_info (NULL, &packetinfo, (const char *)plbuffer, plbuffer_size,
                     fz_rand_below (2) ? sourceid : NULL, sourceid_size,
                     fz_rand_below (2) ? starttimestr : NULL, starttimestr_size,
                     fz_rand_below (2) ? &samplerate : NULL,
                     fz_rand_below (2) ? &samplecount : NULL);

    sl_payload_summary (NULL, &packetinfo, (const char *)plbuffer, plbuffer_size, summary,
                        summary_size);
  }
}

int
main (int argc, char **argv)
{
  long iterations = 10000000;
  uint64_t seed = 0;
  uint8_t seed2[MAX_BUF];
  uint8_t seed3[MAX_BUF];

  fz_parse_args (argc, argv, &iterations, &seed);
  if (seed == 0)
    seed = (uint64_t)time (NULL);
  fz_seed (seed);

  printf ("fuzz_payload: seed=%" PRIu64 " iterations=%ld\n", seed, iterations);
  fflush (stdout);

  build_seeds (seed2, seed3);
  run (iterations, seed2, seed3);

  printf ("fuzz_payload: survived %ld iterations\n", iterations);
  return 0;
}
