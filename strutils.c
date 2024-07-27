/***************************************************************************
 * strutils.c
 *
 * Routines for string manipulation
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
 * Copyright (C) 2020:
 * @author Chad Trabant, IRIS Data Management Center
 ***************************************************************************/

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "libslink.h"


/***************************************************************************
 * sl_strncpclean:
 *
 * Copy 'length' characters from 'source' to 'dest' while removing all
 * spaces.  The result is left justified and always null terminated.
 * The source string must have at least 'length' characters and the
 * destination string must have enough room needed for the non-space
 * characters within 'length' and the null terminator.
 *
 * Returns the number of characters (not including the null terminator) in
 * the destination string.
 ***************************************************************************/
int
sl_strncpclean (char *dest, const char *source, int length)
{
  int sidx, didx;

  for (sidx = 0, didx = 0; sidx < length; sidx++)
  {
    if (*(source + sidx) != ' ')
    {
      *(dest + didx) = *(source + sidx);
      didx++;
    }
  }

  *(dest + didx) = '\0';

  return didx;
} /* End of sl_strncpclean() */
