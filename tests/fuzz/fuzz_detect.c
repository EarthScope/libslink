/***************************************************************************
 * fuzz_detect.c: mutation-based dynamic testing of slutils.c's file-static
 * miniSEED/header parsing helpers -- detect(), receive_header(), and
 * receive_payload() -- against malformed and randomly mutated input.
 * Build and run by hand under a sanitizer; not part of `make test`.
 *
 * #includes slutils.c directly to reach its static functions; see the
 * comment at the top of test_internals.c for why this is safe against
 * duplicate symbols when linked against libslink.a.
 ***************************************************************************/

#include "../../slutils.c"
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

static void
fuzz_detect_fn (long iterations, const uint8_t *seed2, const uint8_t *seed3)
{
  uint8_t buf[MAX_BUF];
  char payloadformat;
  long i;

  for (i = 0; i < iterations; i++)
  {
    size_t len = 1 + fz_rand_below (MAX_BUF);
    int mode = (int)fz_rand_below (3);

    if (mode == 0)
      fz_random_bytes (buf, len);
    else if (mode == 1)
    {
      memcpy (buf, seed2, MAX_BUF);
      fz_mutate (buf, len);
    }
    else
    {
      memcpy (buf, seed3, MAX_BUF);
      fz_mutate (buf, len);
    }

    detect ((const char *)buf, len, &payloadformat);
  }
}

static void
fuzz_receive_header_fn (long iterations, const uint8_t *seed2, const uint8_t *seed3)
{
  uint8_t buf[MAX_BUF];
  SLCD *slconn = sl_initslcd ("t", NULL);
  long i;

  for (i = 0; i < iterations; i++)
  {
    size_t len = fz_rand_below (MAX_BUF);
    int mode = (int)fz_rand_below (3);
    uint32_t bytesavailable;

    if (mode == 0)
      fz_random_bytes (buf, len);
    else if (mode == 1)
    {
      memcpy (buf, seed2, MAX_BUF);
      fz_mutate (buf, len);
    }
    else
    {
      memcpy (buf, seed3, MAX_BUF);
      fz_mutate (buf, len);
    }

    slconn->protocol = fz_rand_below (2) ? SLPROTO3X : SLPROTO40;
    /* bytesavailable must never exceed the real capacity of buf -- in
     * real use it is bounded by the fixed-size internal receive buffer
     * it's drawn from, so reporting more here would be a lie about our
     * own input, not a case the library is responsible for handling. */
    bytesavailable = (uint32_t)fz_rand_below (MAX_BUF + 1);

    receive_header (slconn, buf, bytesavailable);
  }

  sl_freeslcd (slconn);
}

static void
fuzz_receive_payload_fn (long iterations, const uint8_t *seed2, const uint8_t *seed3)
{
  uint8_t buf[MAX_BUF];
  char plbuffer[MAX_BUF];
  SLCD *slconn = sl_initslcd ("t", NULL);
  long i;

  for (i = 0; i < iterations; i++)
  {
    size_t len = fz_rand_below (MAX_BUF);
    int mode = (int)fz_rand_below (3);
    uint32_t plbuffersize = (uint32_t)fz_rand_below (MAX_BUF + 1);
    /* bytesavailable must never exceed the real capacity of buf; see the
     * comment in fuzz_receive_header_fn() above. */
    uint32_t bytesavailable = (uint32_t)fz_rand_below (MAX_BUF + 1);

    if (mode == 0)
      fz_random_bytes (buf, len);
    else if (mode == 1)
    {
      memcpy (buf, seed2, MAX_BUF);
      fz_mutate (buf, len);
    }
    else
    {
      memcpy (buf, seed3, MAX_BUF);
      fz_mutate (buf, len);
    }

    slconn->protocol = fz_rand_below (2) ? SLPROTO3X : SLPROTO40;
    /* Randomize prior state too: payloadlength/payloadcollected persist
     * across calls in real use, including combinations a single valid
     * sequence of calls might not reach on its own. */
    slconn->stat->packetinfo.payloadlength = (uint32_t)fz_rand_below (MAX_BUF * 2);
    slconn->stat->packetinfo.payloadcollected =
        (uint32_t)fz_rand_below (slconn->stat->packetinfo.payloadlength + 1);
    slconn->stat->packetinfo.payloadformat =
        fz_rand_below (2) ? SLPAYLOAD_UNKNOWN : SLPAYLOAD_MSEED2;

    receive_payload (slconn, plbuffer, plbuffersize, buf, bytesavailable);
  }

  sl_freeslcd (slconn);
}

int
main (int argc, char **argv)
{
  long iterations = 2000000;
  uint64_t seed = 0;
  uint8_t seed2[MAX_BUF];
  uint8_t seed3[MAX_BUF];

  fz_parse_args (argc, argv, &iterations, &seed);
  if (seed == 0)
    seed = (uint64_t)time (NULL);
  fz_seed (seed);

  printf ("fuzz_detect: seed=%" PRIu64 " iterations=%ld (x3 targets)\n", seed, iterations);
  fflush (stdout);

  build_seeds (seed2, seed3);

  fuzz_detect_fn (iterations, seed2, seed3);
  printf ("fuzz_detect: detect() survived %ld iterations\n", iterations);
  fflush (stdout);

  fuzz_receive_header_fn (iterations, seed2, seed3);
  printf ("fuzz_detect: receive_header() survived %ld iterations\n", iterations);
  fflush (stdout);

  fuzz_receive_payload_fn (iterations, seed2, seed3);
  printf ("fuzz_detect: receive_payload() survived %ld iterations\n", iterations);
  fflush (stdout);

  return 0;
}
