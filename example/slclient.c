/***************************************************************************
 * slclient.c
 * An example SeedLink client demonstrating the use of libslink.
 *
 * Connects to a SeedLink server, configures a connection using either
 * uni or multi-station mode and collects data.  Detailed information about
 * the data received can be printed.
 *
 * This file is part of the SeedLink Library.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Copyright (C) 2021:
 * @author Chad Trabant, IRIS Data Management Center
 ***************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libslink.h>

#ifndef WIN32
#include <signal.h>

static void term_handler (int sig);
#else
#define strcasecmp _stricmp
#endif

#define PACKAGE "slclient"
#define VERSION LIBSLINK_VERSION

static short int verbose  = 0;
static short int ppackets = 0;
static char *statefile    = 0; /* state file for saving/restoring state */

static SLCD *slconn; /* connection parameters */

static void packet_handler (const SLpacketinfo *packetinfo,
                            const char *payload, uint32_t payloadlen);
static int parameter_proc (int argcount, char **argvec);
static void usage (void);

int
main (int argc, char **argv)
{
  const SLpacketinfo *packetinfo = NULL;
  uint32_t maxpayloadsize = 262144;
  uint32_t plbuffersize = 0;
  char *plbuffer = NULL;
  int status;

#ifndef WIN32
  /* Signal handling, use POSIX calls with standardized semantics */
  struct sigaction sa;

  sigemptyset (&sa.sa_mask);
  sa.sa_flags = SA_RESTART;

  sa.sa_handler = term_handler;
  sigaction (SIGINT, &sa, NULL);
  sigaction (SIGQUIT, &sa, NULL);
  sigaction (SIGTERM, &sa, NULL);

  sa.sa_handler = SIG_IGN;
  sigaction (SIGHUP, &sa, NULL);
  sigaction (SIGPIPE, &sa, NULL);
#endif

  /* Allocate and initialize a new connection description */
  slconn = sl_newslcd (PACKAGE, VERSION);

  /* Process given parameters (command line and parameter file) */
  if (parameter_proc (argc, argv) < 0)
  {
    fprintf (stderr, "Parameter processing failed\n\n");
    fprintf (stderr, "Try '-h' for detailed help\n");
    return -1;
  }

  /* Connection options can be set */
  //slconn->dialup = 1;
  //slconn->noblock = 1;
  //sl_request_info (slconn, "ID");

  /* Loop with the connection manager */
  while ((status = sl_collect (slconn, &packetinfo,
                               &plbuffer, &plbuffersize,
                               maxpayloadsize)) != SLTERMINATE)
  {
    if (status == SLPACKET)
    {
      /* Here we do something with the packet */
      packet_handler (packetinfo, plbuffer, packetinfo->payloadcollected);
    }
    else if (status == SLTOOLARGE)
    {
      /* Here we could increase the maxpayloadsize to accommodate if desired */
      sl_log (2, 0, "received payload length %u too large for max buffer of %u\n",
              packetinfo->payloadlength, maxpayloadsize);
      break;
    }
    else if (status == SLNOPACKET)
    {
      /* Here should only occurr when non-blocking, i.e. slconn->noblock == 1 */
      sl_log (0, 2, "sleeping after receiving no data from sl_collect()\n");
      sl_usleep(500000);
    }

    /* Here we could send an in-stream INFO request with sl_request_info() */
  }

  /* Make sure everything is shut down and save the state file */
  if (slconn->link != -1)
    sl_disconnect (slconn);

  if (statefile)
    sl_savestate (slconn, statefile);

  return 0;
} /* End of main() */

/***************************************************************************
 * packet_handler():
 * Process a received packet based on packet type.
 ***************************************************************************/
static void
packet_handler (const SLpacketinfo *packetinfo,
                const char *payload, uint32_t payloadlength)
{
  static SLMSrecord *msr = NULL;

  double dtime;   /* Epoch time */
  double secfrac; /* Fractional part of epoch time */
  time_t itime;   /* Integer part of epoch time */
  char timestamp[30] = {0};
  struct tm *timep;
  int printed;

  /* Build a current local time string */
  dtime   = sl_dtime (void);
  secfrac = (double)((double)dtime - (int)dtime);
  itime   = (time_t)dtime;
  timep   = localtime (&itime);

  printed = snprintf (timestamp, sizeof (timestamp), "%04d-%03dT%02d:%02d:%02d.%01.0f",
                      timep->tm_year + 1900, timep->tm_yday + 1, timep->tm_hour,
                      timep->tm_min, timep->tm_sec, secfrac);

  if (printed >= sizeof (timestamp))
  {
    sl_log (1, 0, "%s() Time string overflow\n", __func__);
  }

  sl_log (0, 1, "%s, seq %" PRIu64 ", Received %u bytes of payload format %s\n",
          timestamp, packetinfo->seqnum, payloadlength,
          sl_typestr(packetinfo->payloadtype));

  /* Process waveform data */
  if (packetinfo->payloadtype == SLPAYLOAD_MSEED2)
  {
    sl_msr_parse (slconn->log, (const char *)payload, &msr, 1, 0,
                  packetinfo->payloadlength);

    if (verbose || ppackets)
      sl_msr_print (slconn->log, msr, ppackets);
  }
  else
  {
    sl_log (1, 1, "Unsupported payload type: %c\n", packetinfo->payloadtype);
  }

} /* End of packet_handler() */

/***************************************************************************
 * parameter_proc:
 *
 * Process the command line parameters.
 *
 * Returns 0 on success, and -1 on failure
 ***************************************************************************/
static int
parameter_proc (int argcount, char **argvec)
{
  int optind;
  int error = 0;

  char *streamfile  = 0; /* stream list file for configuring streams */
  char *multiselect = 0;
  char *selectors   = 0;

  if (argcount <= 1)
    error++;

  /* Process all command line arguments */
  for (optind = 1; optind < argcount; optind++)
  {
    if (strcmp (argvec[optind], "-V") == 0)
    {
      fprintf (stderr, "%s version: %s\n", PACKAGE, VERSION);
      exit (0);
    }
    else if (strcmp (argvec[optind], "-h") == 0)
    {
      usage ();
      exit (0);
    }
    else if (strncmp (argvec[optind], "-v", 2) == 0)
    {
      verbose += strspn (&argvec[optind][1], "v");
    }
    else if (strcmp (argvec[optind], "-p") == 0)
    {
      ppackets = 1;
    }
    else if (strcmp (argvec[optind], "-nt") == 0)
    {
      slconn->netto = atoi (argvec[++optind]);
    }
    else if (strcmp (argvec[optind], "-nd") == 0)
    {
      slconn->netdly = atoi (argvec[++optind]);
    }
    else if (strcmp (argvec[optind], "-k") == 0)
    {
      slconn->keepalive = atoi (argvec[++optind]);
    }
    else if (strcmp (argvec[optind], "-l") == 0)
    {
      streamfile = argvec[++optind];
    }
    else if (strcmp (argvec[optind], "-s") == 0)
    {
      selectors = argvec[++optind];
    }
    else if (strcmp (argvec[optind], "-S") == 0)
    {
      multiselect = argvec[++optind];
    }
    else if (strcmp (argvec[optind], "-x") == 0)
    {
      statefile = argvec[++optind];
    }
    else if (strncmp (argvec[optind], "-", 1) == 0)
    {
      fprintf (stderr, "Unknown option: %s\n", argvec[optind]);
      exit (1);
    }
    else if (!slconn->sladdr)
    {
      slconn->sladdr = argvec[optind];
    }
    else
    {
      fprintf (stderr, "Unknown option: %s\n", argvec[optind]);
      exit (1);
    }
  }

  /* Make sure a server was specified */
  if (!slconn->sladdr)
  {
    fprintf (stderr, "%s version: %s\n\n", PACKAGE, VERSION);
    fprintf (stderr, "No SeedLink server specified\n\n");
    fprintf (stderr, "Usage: %s [options] [host][:port]\n", PACKAGE);
    fprintf (stderr, "Try '-h' for detailed help\n");
    exit (1);
  }

  /* Initialize the verbosity for the sl_log function */
  sl_loginit (verbose, NULL, NULL, NULL, NULL);

  /* Report the program version */
  sl_log (0, 1, "%s version: %s\n", PACKAGE, VERSION);

  /* If errors then report the usage message and quit */
  if (error)
  {
    usage ();
    exit (1);
  }

  /* If verbosity is 2 or greater print detailed packet infor */
  if (verbose >= 2)
    ppackets = 1;

  /* Load the stream list from a file if specified */
  if (streamfile)
    sl_read_streamlist (slconn, streamfile, selectors);

  /* Parse the 'multiselect' string following '-S' */
  if (multiselect)
  {
    if (sl_parse_streamlist (slconn, multiselect, selectors) == -1)
      return -1;
  }
  else if (!streamfile)
  { /* No 'streams' array, assuming uni-station mode */
    sl_setuniparams (slconn, selectors, -1, 0);
  }

  /* Attempt to recover sequence numbers from state file */
  if (statefile)
  {
    if (sl_recoverstate (slconn, statefile) < 0)
    {
      sl_log (2, 0, "state recovery failed\n");
    }
  }

  return 0;
} /* End of parameter_proc() */

/***************************************************************************
 * usage:
 * Print the usage message and exit.
 ***************************************************************************/
static void
usage (void)
{
  fprintf (stderr, "\nUsage: %s [options] [host][:port]\n\n", PACKAGE);
  fprintf (stderr,
           " ## General program options ##\n"
           " -V             report program version\n"
           " -h             show this usage message\n"
           " -v             be more verbose, multiple flags can be used\n"
           " -p             print details of data packets\n\n"
           " -nd delay      network re-connect delay (seconds), default 30\n"
           " -nt timeout    network timeout (seconds), re-establish connection if no\n"
           "                  data/keepalives are received in this time, default 600\n"
           " -k interval    send keepalive (heartbeat) packets this often (seconds)\n"
           " -x statefile   save/restore stream state information to this file\n"
           "\n"
           " ## Data stream selection ##\n"
           " -l listfile    read a stream list from this file for multi-station mode\n"
           " -s selectors   selectors for uni-station or default for multi-station\n"
           " -S streams     select streams for multi-station (requires SeedLink >= 2.5)\n"
           "   'streams' = 'stream1[:selectors1],stream2[:selectors2],...'\n"
           "        'stream' is in NET_STA format, for example:\n"
           "        -S \"IU_KONO:BHE BHN,GE_WLF,MN_AQU:HH?.D\"\n\n"
           "\n"
           " [host][:port]  Address of the SeedLink server in host:port format\n"
           "                  if host is omitted (i.e. ':18000'), localhost is assumed\n"
           "                  if :port is omitted (i.e. 'localhost'), 18000 is assumed\n\n");

} /* End of usage() */

#ifndef WIN32
/***************************************************************************
 * term_handler:
 * Signal handler routine.
 ***************************************************************************/
static void
term_handler (int sig)
{
  sl_terminate (slconn);
}
#endif
