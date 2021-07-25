/***************************************************************************
 * statefile.c:
 *
 * Routines to save and recover SeedLink sequence numbers to/from a file.
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
 * sl_savestate:
 *
 * Save the all the current the sequence numbers and time stamps into the
 * given state file.
 *
 * Returns:
 * -1 : error
 *  0 : completed successfully
 ***************************************************************************/
int
sl_savestate (SLCD *slconn, const char *statefile)
{
  SLstream *curstream;
  FILE *fp;
  char line[200];
  int linelen;

  curstream = slconn->streams;

  /* Open the state file */
  if ((fp = fopen (statefile, "wb")) == NULL)
  {
    sl_log_r (slconn, 2, 0, "cannot open state file for writing\n");
    return -1;
  }

  sl_log_r (slconn, 1, 2, "saving connection state to state file\n");

  /* Traverse stream chain and write sequence numbers */
  while (curstream != NULL)
  {
    if (curstream->seqnum == SL_UNSETSEQUENCE)
      linelen = snprintf (line, sizeof (line), "%s %s -1 %s\n",
                          curstream->net, curstream->sta,
                          curstream->timestamp);
    else
      linelen = snprintf (line, sizeof (line), "%s %s %" PRIu64 " %s\n",
                          curstream->net, curstream->sta,
                          curstream->seqnum, curstream->timestamp);

    if (fputs (line, fp) == EOF)
    {
      sl_log_r (slconn, 2, 0, "cannot write to state file, %s\n", strerror (errno));
      return -1;
    }

    curstream = curstream->next;
  }

  if (fclose (fp))
  {
    sl_log_r (slconn, 2, 0, "cannot close state file, %s\n", strerror (errno));
    return -1;
  }

  return 0;
} /* End of sl_savestate() */

/***************************************************************************
 * sl_recoverstate:
 *
 * Recover the state file and put the sequence numbers and time stamps into
 * the pre-existing stream chain entries.
 *
 * Returns:
 * -1 : error
 *  0 : completed successfully
 *  1 : file could not be opened (probably not found)
 ***************************************************************************/
int
sl_recoverstate (SLCD *slconn, const char *statefile)
{
  SLstream *curstream;
  FILE *fp;
  char net[3];
  char sta[6];
  char timestamp[20];
  char line[200];
  char seqstr[21];
  char *endptr = NULL;
  uint64_t seqnum;
  int fields;
  int count;

  net[0]       = '\0';
  sta[0]       = '\0';
  timestamp[0] = '\0';

  /* Open the state file */
  if ((fp = fopen (statefile, "rb")) == NULL)
  {
    if (errno == ENOENT)
    {
      sl_log_r (slconn, 1, 0, "could not find state file: %s\n", statefile);
      return 1;
    }
    else
    {
      sl_log_r (slconn, 2, 0, "could not open state file, %s\n", strerror (errno));
      return -1;
    }
  }

  sl_log_r (slconn, 1, 1, "recovering connection state from state file\n");

  count = 1;

  while (fgets (line, sizeof (line), fp))
  {
    fields = sscanf (line, "%2s %5s %s %19[0-9,]\n",
                     net, sta, seqstr, timestamp);

    if (fields < 0)
      continue;

    if (fields < 3)
    {
      sl_log_r (slconn, 2, 0, "could not parse line %d of state file\n", count);
    }

    if (seqstr[0] == '-' && seqstr[1] == '1')
    {
      seqnum = SL_UNSETSEQUENCE;
    }
    else
    {
      seqnum = (uint64_t) strtoull (seqstr, &endptr, 10);

      if (*endptr)
      {
        sl_log_r (slconn, 2, 0, "could not parse sequence number (%s) from line %d of state file\n",
                  seqstr, count);
      }
    }

    /* Search for a matching NET and STA in the stream chain */
    curstream = slconn->streams;
    while (curstream != NULL)
    {
      if (!strcmp (net, curstream->net) &&
          !strcmp (sta, curstream->sta))
      {
        curstream->seqnum = seqnum;

        if (fields == 4)
          strncpy (curstream->timestamp, timestamp, 20);

        break;
      }

      curstream = curstream->next;
    }

    count++;
  }

  if (ferror (fp))
  {
    sl_log_r (slconn, 2, 0, "file read error for %s\n", statefile);
  }

  if (fclose (fp))
  {
    sl_log_r (slconn, 2, 0, "could not close state file, %s\n", strerror (errno));
    return -1;
  }

  return 0;
} /* End of sl_recoverstate() */
