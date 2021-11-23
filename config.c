/***************************************************************************
 * config.c:
 *
 * Routines to assist with the configuration of a SeedLink connection
 * description.
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

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "libslink.h"

/***************************************************************************
 * sl_read_streamlist:
 *
 * Read a list of streams and selectors from a file and add them to the
 * stream chain for configuring a multi-station connection.
 *
 * If 'defselect' is not NULL or 0 it will be used as the default selectors
 * for entries will no specific selectors indicated.
 *
 * The file is expected to be repeating lines of the form:
 *   <NET> <STA> [selectors]
 * For example:
 * --------
 * # Comment lines begin with a '#' or '*'
 * GE ISP  BH?.D
 * NL HGN
 * MN AQU  BH?  HH?
 * --------
 *
 * Returns the number of streams configured or -1 on error.
 ***************************************************************************/
int
sl_read_streamlist (SLCD *slconn, const char *streamfile,
                    const char *defselect)
{
  FILE *fp;
  char net[11] = {0};
  char sta[11] = {0};
  char netstaid[22] = {0};
  char selectors[100] = {0};
  char line[100];
  int fields;
  int count;
  int stacount;

  /* Open the stream list file */
  if ((fp = fopen (streamfile, "rb")) == NULL)
  {
    if (errno == ENOENT)
    {
      sl_log_r (slconn, 2, 0, "could not find stream list file: %s\n", streamfile);
      return -1;
    }
    else
    {
      sl_log_r (slconn, 2, 0, "opening stream list file, %s\n", strerror (errno));
      return -1;
    }
  }

  sl_log_r (slconn, 1, 1, "Reading stream list from %s\n", streamfile);

  count    = 1;
  stacount = 0;

  while (fgets (line, sizeof (line), fp))
  {
    fields = sscanf (line, "%10s %10s %99s\n",
                     net, sta, selectors);

    /* Ignore blank or comment lines */
    if (fields < 0 || net[0] == '#' || net[0] == '*')
      continue;

    if (fields < 2)
    {
      sl_log_r (slconn, 2, 0, "cannot parse line %d of stream list\n", count);
    }

    snprintf (netstaid, sizeof (netstaid), "%s_%s", net, sta);

    /* Add this stream to the stream chain */
    if (fields == 3)
    {
      sl_addstream (slconn, netstaid, selectors, -1, NULL);
      stacount++;
    }
    else
    {
      sl_addstream (slconn, netstaid, defselect, -1, NULL);
      stacount++;
    }

    count++;
  }

  if (ferror (fp))
  {
    sl_log_r (slconn, 2, 0, "file read error for %s\n", streamfile);
  }

  if (stacount == 0)
  {
    sl_log_r (slconn, 2, 0, "no streams defined in %s\n", streamfile);
  }
  else if (stacount > 0)
  {
    sl_log_r (slconn, 1, 2, "Read %d streams from %s\n", stacount, streamfile);
  }

  if (fclose (fp))
  {
    sl_log_r (slconn, 2, 0, "closing stream list file, %s\n", strerror (errno));
    return -1;
  }

  return count;
} /* End of sl_read_streamlist() */

/***************************************************************************
 * sl_parse_streamlist:
 *
 * Parse a string of streams and selectors and add them to the stream
 * chain for configuring a multi-station connection.
 *
 * The string should be of the following form:
 * "stream1[:selectors1],stream2[:selectors2],..."
 *
 * For example:
 * "IU_COLA:*_B_H_? *_L_H_?"
 * "IU_KONO:BHE BHN,GE_WLF,MN_AQU:HH?"
 *
 * Returns the number of streams configured or -1 on error.
 ***************************************************************************/
int
sl_parse_streamlist (SLCD *slconn, const char *streamlist,
                     const char *defselect)
{
  int count = 0;
  int fields;

  const char *staselect;
  char *netstaid;

  SLstrlist *strlist = NULL; /* split streamlist on ',' */
  SLstrlist *reqlist = NULL; /* split strlist on ':' */

  SLstrlist *ringptr = NULL;
  SLstrlist *reqptr  = NULL;

  /* Parse the streams and selectors */
  sl_strparse (streamlist, ",", &strlist);
  ringptr = strlist;

  while (ringptr != 0)
  {
    staselect = NULL;

    /* Parse reqlist (the 'NET_STA:selector' part) */
    fields = sl_strparse (ringptr->element, ":", &reqlist);
    reqptr = reqlist;

    netstaid = reqptr->element;

    if (strlen(netstaid) < 3 || strchr(netstaid, '_') == NULL)
    {
      sl_log_r (slconn, 2, 0, "not in NET_STA format: %s\n", netstaid);
      count = -1;
    }

    if (fields > 1) /* Selectors were included following the ':' */
    {
      reqptr = reqptr->next;
      staselect = reqptr->element;

      if (strlen (reqptr->element) == 0)
      {
        sl_log_r (slconn, 2, 0, "empty selector: %s\n", reqptr->element);
        count = -1;
      }
    }
    else /* If no specific selectors, use the default */
    {
      staselect = defselect;
    }

    /* Add to the stream chain */
    if (count != -1)
    {
      sl_addstream (slconn, netstaid, staselect, -1, NULL);
      count++;
    }

    sl_strparse (NULL, NULL, &reqlist);

    ringptr = ringptr->next;
  }

  if (reqlist != NULL)
  {
    sl_strparse (NULL, NULL, &reqlist);
  }

  if (count == 0)
  {
    sl_log_r (slconn, 2, 0, "no streams defined in stream list\n");
  }
  else if (count > 0)
  {
    sl_log_r (slconn, 1, 2, "Parsed %d streams from stream list\n", count);
  }

  /* Free the ring list */
  sl_strparse (NULL, NULL, &strlist);

  return count;
} /* End of sl_parse_streamlist() */
