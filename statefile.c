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

  /* Traverse stream chain and write sequence numbers
   * Format: StationID  Sequence#  Timestamp */
  while (curstream != NULL)
  {
    if (curstream->seqnum == SL_UNSETSEQUENCE)
      linelen = snprintf (line, sizeof (line), "%s -1 %s\n",
                          curstream->netstaid,
                          curstream->timestamp);
    else
      linelen = snprintf (line, sizeof (line), "%s %" PRIu64 " %s\n",
                          curstream->netstaid,
                          curstream->seqnum,
                          curstream->timestamp);

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

  char line[200];
  char *field[5];
  int fields;
  int idx;

  char netstaid[22] = {0};
  char timestamp[31] = {0};
  char *netstastr;
  char *seqstr;
  char *timestr;
  char *endptr;

  uint64_t seqnum;
  int count;

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
    netstastr = NULL;
    seqstr = NULL;
    timestr = NULL;

    /* Store pointers to space-separated fields & convert spaces to terminators */
    for (idx = 0, fields = 0;
         line[idx] && fields < 5;
         idx++)
    {
      if (!isspace(line[idx]))
      {
        if (idx == 0 || line[idx - 1] == '\0')
        {
          field[fields] = line + idx;
          fields++;
        }
      }
      else
      {
        line[idx] = '\0';
      }
    }

    if (fields == 0)
    {
      continue;
    }
    /* Format: NET_STA Sequence# [Timestamp] */
    if (fields >= 2 && strchr (field[0], '_') != NULL)
    {
      netstastr = field[0];
      seqstr    = field[1];
      timestr   = (fields >= 3) ? field[2] : NULL;
    }
    /* Old format: NET STA Sequence [Timestamp] */
    else if (fields >= 3)
    {
      snprintf (netstaid, sizeof (netstaid), "%s_%s", field[0], field[1]);
      netstastr = netstaid;
      seqstr    = field[2];
      timestr   = (fields >= 4) ? field[3] : NULL;
    }
    else if (fields < 3)
    {
      sl_log_r (slconn, 2, 0, "could not parse line %d of state file\n", count);
      break;
    }

    /* Convert old comma-delimited date-time to ISO-compatible format
     * Example: '2021,11,19,17,23,18' => '2021-11-18T17:23:18.0Z' */
    if (timestr)
    {
      if (sl_isodatetime(timestamp, timestr) != NULL)
      {
        timestr = timestamp;
      }
      else
      {
        sl_log_r (slconn, 1, 0, "could not parse timestamp for %s entry: '%s', ignoring\n",
                  netstastr, timestr);
        return -1;
      }
    }

    if (seqstr[0] == '-' && seqstr[1] == '1')
    {
      seqnum = SL_UNSETSEQUENCE;
    }
    else
    {
      seqnum = (uint64_t)strtoull (seqstr, &endptr, 10);

      if (*endptr)
      {
        sl_log_r (slconn, 2, 0, "could not parse sequence number (%s) from line %d of state file\n",
                  seqstr, count);
      }
    }

    /* Search for a matching NET_STA in the stream list */
    curstream = slconn->streams;
    while (curstream != NULL)
    {
      if (!strcmp (netstastr, curstream->netstaid))
      {
        curstream->seqnum = seqnum;

        if (timestr)
          strncpy (curstream->timestamp, timestr, sizeof(curstream->timestamp));

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
