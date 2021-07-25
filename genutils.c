/***************************************************************************
 *
 * General utility functions.
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
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "libslink.h"


/***************************************************************************
 * sl_littleendianhost:
 *
 * Determine the byte order of the host machine.  Due to the lack of
 * portable defines to determine host byte order this run-time test is
 * provided.  This function originated from a similar function in libmseed.
 *
 * Returns 1 if the host is little endian, otherwise 0.
 ***************************************************************************/
uint8_t
sl_littleendianhost (void)
{
  uint16_t host = 1;
  return *((uint8_t *)(&host));
} /* End of sl_littleendianhost() */

/***************************************************************************
 * sl_doy2md:
 *
 * Compute the month and day-of-month from a year and day-of-year.
 *
 * Returns 0 on success and -1 on error.
 ***************************************************************************/
int
sl_doy2md (int year, int jday, int *month, int *mday)
{
  int idx;
  int leap;
  int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

  /* Sanity check for the supplied year */
  if (year < 1900 || year > 2100)
  {
    sl_log_r (NULL, 2, 0, "%s(): year (%d) is out of range\n", __func__, year);
    return -1;
  }

  /* Test for leap year */
  leap = (((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0)) ? 1 : 0;

  /* Add a day to February if leap year */
  if (leap)
    days[1]++;

  if (jday > 365 + leap || jday <= 0)
  {
    sl_log_r (NULL, 2, 0, "%s(): day-of-year (%d) is out of range\n", __func__, jday);
    return -1;
  }

  for (idx = 0; idx < 12; idx++)
  {
    jday -= days[idx];

    if (jday <= 0)
    {
      *month = idx + 1;
      *mday  = days[idx] + jday;
      break;
    }
  }

  return 0;
} /* End of sl_doy2md() */

/***************************************************************************
 * sl_checkversion:
 *
 * Check protocol version number against specified major and minor values
 *
 * Returns:
 *  1 = version is greater than or equal to value specified
 *  0 = no protocol version is known
 * -1 = version is less than value specified
 ***************************************************************************/
int
sl_checkversion (const SLCD *slconn, uint8_t major, uint8_t minor)
{
  if (slconn->proto_major == 0)
  {
    return 0;
  }
  else if (slconn->proto_major > major ||
           (slconn->proto_major == major && slconn->proto_minor >= minor))
  {
    return 1;
  }
  else
  {
    return -1;
  }
} /* End of sl_checkversion() */

/***************************************************************************
 * sl_checkslcd:
 *
 * Check a SeedLink connection description (SLCD struct).
 *
 * Returns 0 if pass and -1 if problems were identified.
 ***************************************************************************/
int
sl_checkslcd (const SLCD *slconn)
{
  int retval = 0;

  if (slconn->streams == NULL && slconn->info == NULL)
  {
    sl_log_r (slconn, 2, 0, "%s(): stream chain AND info type are empty\n", __func__);
    retval = -1;
  }

  return retval;
} /* End of sl_checkslconn() */

/**********************************************************************/ /**
 * @brief Return human readable description for specified payload type
 *
 * @returns Descriptive string for payload type
 ***************************************************************************/
const char *
sl_typestr (char type)
{
  switch (type)
  {
  case SLPAYLOAD_UNKNOWN:
    return "Unknown";
    break;
  case SLPAYLOAD_MSEED2INFO:
    return "INFO as XML in miniSEED 2";
    break;
  case SLPAYLOAD_MSEED2INFOTERM:
    return "INFO (terminated) as XML in miniSEED 2";
    break;
  case SLPAYLOAD_MSEED2:
    return "miniSEED 2";
    break;
  case SLPAYLOAD_MSEED3:
    return "miniSEED 3";
    break;
  case SLPAYLOAD_INFO:
    return "INFO in JSON";
    break;
  default:
    return "Unrecognized payload type";
  }
} /* End of sl_typestr() */

/***************************************************************************
 * sl_strerror:
 *
 * Return a description of the last system error, in the case of Win32
 * this will be the last Windows Sockets error.
 ***************************************************************************/
const char *
sl_strerror (void)
{
#if defined(SLP_WIN)
  static char errorstr[100];

  snprintf (errorstr, sizeof (errorstr), "%d", WSAGetLastError ());
  return (const char *)errorstr;

#else
  return (const char *)strerror (errno);

#endif
} /* End of sl_strerror() */

/**********************************************************************/ /**
 * @brief Get current time as nanosecond resolution Unix/POSIX time
 *
 * Actual resolution depends on system, nanosecond resolution should
 * not be assumed.
 *
 * @returns Current time as nanoseconds since the Unix/POSIX epoch.
 ***************************************************************************/
int64_t
sl_nstime (void)
{
#if defined(SLP_WIN)

  uint64 rv;
  FILETIME FileTime;

  GetSystemTimeAsFileTime(&FileTime);

  /* Full win32 epoch value, in 100ns */
  rv = (((LONGLONG)FileTime.dwHighDateTime << 32) +
        (LONGLONG)FileTime.dwLowDateTime);

  rv -= 116444736000000000LL; /* Convert from FileTime to UNIX epoch time */
  rv *= 100; /* Convert from 100ns to ns */

  return rv;

#else

  struct timeval tv;

  gettimeofday (&tv, NULL);
  return ((int64_t)tv.tv_sec * 1000000000 +
          (int64_t)tv.tv_usec * 1000);

#endif
} /* End of sl_nstime() */

/***************************************************************************
 * sl_usleep:
 *
 * Sleep for a given number of microseconds.  Under Win32 use SleepEx()
 * and for all others use the POSIX.4 nanosleep().
 ***************************************************************************/
void
sl_usleep (unsigned long int useconds)
{
#if defined(SL_WIN)

  SleepEx ((useconds / 1000), 1);

#else

  struct timespec treq, trem;

  treq.tv_sec  = (time_t) (useconds / 1e6);
  treq.tv_nsec = (long)((useconds * 1e3) - (treq.tv_sec * 1e9));

  nanosleep (&treq, &trem);

#endif
} /* End of sl_usleep() */
