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

/***********************************************************************/ /**
 * @brief Return protocol details for a specified type
 *
 * @param protocol Protocol identifier
 * @param major Pointer to value for major protocol version (optional)
 * @param major Pointer to value for minor protocol version (optional)
 *
 * @return Pointer to string description of protocol version.
 ***************************************************************************/
char *
sl_protocol_details (LIBPROTOCOL protocol, uint8_t *major, uint8_t *minor)
{
  switch (protocol)
  {
  case SLPROTO3X:
    if (major)
      *major = 3;
    if (minor)
      *minor = 0;
    return "3.X";
  case SLPROTO40:
    if (major)
      *major = 4;
    if (minor)
      *minor = 0;
    return "4.0";
  default:
    if (major)
      *major = 0;
    if (minor)
      *minor = 0;
    return "Unknown";
  }
}

/**********************************************************************/ /**
 * @brief Return human readable description for specified payload format
 *
 * @returns Descriptive string for payload format
 ***************************************************************************/
const char *
sl_formatstr (char format, char subformat)
{
  switch (format)
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
    switch (subformat)
    {
    case 'E':
      return "miniSEED 2 event detection";
      break;
    case 'C':
      return "miniSEED 2 calibration";
      break;
    case 'T':
      return "miniSEED 2 timing exception";
      break;
    case 'L':
      return "miniSEED 2 log";
      break;
    case 'O':
      return "miniSEED 2 opaque";
      break;
    default:
      return "miniSEED 2";
    }
    break;
  case SLPAYLOAD_MSEED3:
    return "miniSEED 3";
    break;
  case SLPAYLOAD_JSON:
    switch (subformat)
    {
    case SLPAYLOAD_JSON_INFO:
      return "INFO in JSON";
      break;
    case SLPAYLOAD_JSON_ERROR:
      return "ERROR in JSON";
      break;
    default:
      return "JSON";
    }
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

  uint64_t rv;
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

/**********************************************************************/ /**
 * @brief Return ISO-compatible date-time formatted string
 *
 * Convert date-time string deliminters to match the following
 * ISO-8601 date-time format if needed and possible:
 *
 *   YYYY-MM-DDThh:mm:ss.sssssssssZ
 *
 * The output string will always be in UTC with a 'Z' designation if
 * it contains a time portion.
 *
 * The output buffer pointer can be the same as the input pointer for
 * an in-place conversion.
 *
 * This routine does very little validation, invalid input date-times
 * will result in invalid conversions.
 *
 * The output buffer must be as large as the input date-time string
 * plus a terminating 'Z' if not included in the original string.
 *
 * @param[out] isodatetime Buffer to write ISO-8601 time string
 * @param[in] datetime Date-time string to convert
 *
 * @returns Date-time string converted ISO-8601 format
 * @retval Pointer to date-time string on success
 * @retval NULL on error
 ***************************************************************************/
char *
sl_isodatetime (char *isodatetime, const char *datetime)
{
  char newchar;
  int delims;
  int idx;

  if (datetime == NULL || isodatetime == NULL)
    return NULL;

  /* Create output string char-by-char from input string */
  for (idx = 0, delims = 0; datetime[idx] != '\0'; idx++)
  {
    /* Pass through digits */
    if (isdigit (datetime[idx]))
    {
      newchar = 0;
    }
    /* Pass through acceptable delimiters */
    else if (datetime[idx] == '-' ||
             datetime[idx] == 'T' ||
             datetime[idx] == ':' ||
             datetime[idx] == '.' ||
             datetime[idx] == 'Z')
    {
      delims++;
      newchar = 0;
    }
    /* Convert comma to appropriate delimiter */
    else if (datetime[idx] == ',')
    {
      delims++;

      switch (delims)
      {
      case 1:
      case 2:
        newchar = '-';
        break;
      case 3:
        newchar = 'T';
        break;
      case 4:
      case 5:
        newchar = ':';
        break;
      case 6:
        newchar = '.';
        break;
      default:
        return NULL;
      }
    }
    /* Unrecognized character in input date-time string */
    else
    {
      return NULL;
    }

    /* Write new character if set */
    if (newchar)
    {
      isodatetime[idx] = newchar;
    }
    /* Write input character if not the same buffer */
    else if (datetime != isodatetime)
    {
      isodatetime[idx] = datetime[idx];
    }
  }

  /* Add UTC 'Z' suffix if not present and time components included */
  if (delims >= 3 && isodatetime[idx - 1] != 'Z')
  {
    isodatetime[idx++] = 'Z';
  }

  isodatetime[idx] = '\0';

  return isodatetime;
} /* End of sl_isodatetime() */

/**********************************************************************/ /**
 * @brief Return legacy SeedLink comma-deliminted date-time formatted string
 *
 * Convert date-time string deliminters to match the following
 * comma-delimited format if needed and possible:
 *
 *   YYYY,MM,DD,hh,mm,ss,ssssss
 *
 * The output buffer pointer can be the same as the input pointer for
 * an in-place conversion.
 *
 * This routine does very little validation, invalid input date-times
 * will result in invalid conversions.
 *
 * @param[out] commadatetime Buffer to write comma-delimited time string
 * @param[in] datetime Date-time string to convert
 *
 * @returns Date-time string converted comma-delimited format
 * @retval Pointer to date-time string on success
 * @retval NULL on error
 ***************************************************************************/
char *
sl_commadatetime (char *commadatetime, const char *datetime)
{
  char newchar;
  int delims;
  int idx;

  if (datetime == NULL || commadatetime == NULL)
    return NULL;

  /* Create output string char-by-char from input string */
  for (idx = 0, delims = 0; datetime[idx] != '\0'; idx++)
  {
    /* Pass through digits */
    if (isdigit (datetime[idx]))
    {
      newchar = 0;
    }
    /* Pass through acceptable delimiter */
    else if (datetime[idx] == ',')
    {
      delims++;
      newchar = 0;
    }
    /* Convert recognized delimiters to commas */
    else if (datetime[idx] == '-' ||
             datetime[idx] == 'T' ||
             datetime[idx] == ':' ||
             datetime[idx] == '.')
    {
      delims++;
      newchar = ',';
    }
    /* Terminating 'Z' is skipped */
    else if (datetime[idx] == 'Z' && datetime[idx+1] == '\0')
    {
      break;
    }
    /* Unrecognized character in input date-time string */
    else
    {
      return NULL;
    }

    /* Write new character if set */
    if (newchar)
    {
      commadatetime[idx] = newchar;
    }
    /* Write input character if not the same buffer */
    else if (datetime != commadatetime)
    {
      commadatetime[idx] = datetime[idx];
    }
  }

  commadatetime[idx] = '\0';

  return commadatetime;
} /* End of sl_commadatetime() */


/***************************************************************************
 * sl_usleep:
 *
 * Sleep for a given number of microseconds.  Under Win32 use SleepEx()
 * and for all others use the POSIX.4 nanosleep().
 ***************************************************************************/
void
sl_usleep (unsigned long int useconds)
{
#if defined(SLP_WIN)

  SleepEx ((useconds / 1000), 1);

#else

  struct timespec treq, trem;

  treq.tv_sec  = (time_t) (useconds / 1e6);
  treq.tv_nsec = (long)((useconds * 1e3) - (treq.tv_sec * 1e9));

  nanosleep (&treq, &trem);

#endif
} /* End of sl_usleep() */
