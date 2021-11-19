/***************************************************************************
 * network.c
 *
 * Network communication routines for SeedLink
 *
 * Originally based on the SeedLink interface of the modified Comserv in
 * SeisComP written by Andres Heinloo
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

#include "libslink.h"

/* Functions only used in this source file */
static int sayhello_int (SLCD *slconn);
static int batchmode_int (SLCD *slconn);
static int negotiate_uni_v3 (SLCD *slconn);
static int negotiate_multi_v3 (SLCD *slconn);
static int negotiate_v4 (SLCD *slconn);
static int checksock_int (SOCKET sock, int tosec, int tousec);

static int sockstartup_int (void);
static int sockconnect_int (SOCKET sock, struct sockaddr *inetaddr, int addrlen);
static int sockclose_int (SOCKET sock);
static int socknoblock_int (SOCKET sock);
static int noblockcheck_int (void);
static int setsocktimeo_int (SOCKET socket, int timeout);

/***************************************************************************
 * sl_connect:
 *
 * Open a network socket connection to a SeedLink server and set
 * 'slconn->link' to the new descriptor.  Expects 'slconn->sladdr' to
 * be in 'host:port' format.  Either the host, port or both are
 * optional, if the host is not specified 'localhost' is assumed, if
 * the port is not specified '18000' is assumed, if neither is
 * specified (only a colon) then 'localhost' and port '18000' are
 * assumed.
 *
 * If 'sayhello' is true, commands will be sent to the server
 * to determine server features and set features supported by the
 * library.  This includes upgrading the protocol to the maximum
 * version supported by both server and client.  Unless you wish to do
 * low level negotiation independently, always set this to 1.
 *
 * If a permanent error is detected (invalid port specified) the
 * slconn->terminate flag will be set so the sl_collect() family of
 * routines will not continue trying to connect.
 *
 * Returns -1 on errors otherwise the socket descriptor created.
 ***************************************************************************/
SOCKET
sl_connect (SLCD *slconn, int sayhello)
{
  struct addrinfo *addr0 = NULL;
  struct addrinfo *addr  = NULL;
  struct addrinfo hints;
  SOCKET sock;
  int on = 1;
  int sockstat;
  long int nport;
  char nodename[300] = {0};
  char nodeport[100] = {0};
  char *ptr, *tail;
  int timeout;

  if (sockstartup_int ())
  {
    sl_log_r (slconn, 2, 0, "could not initialize network sockets\n");
    return -1;
  }

  /* Search address host-port separator, first for '@', then ':' */
  if ((ptr = strchr (slconn->sladdr, '@')) == NULL && (ptr = strchr (slconn->sladdr, ':')))
  {
    /* If first ':' is not the last, this is not a separator */
    if (strrchr (slconn->sladdr, ':') != ptr)
      ptr = NULL;
  }

  /* If address begins with the separator */
  if (slconn->sladdr == ptr)
  {
    if (slconn->sladdr[1] == '\0') /* Only a separator */
    {
      strcpy (nodename, SL_DEFAULT_HOST);
      strcpy (nodeport, SL_DEFAULT_PORT);
    }
    else /* Only a port */
    {
      strcpy (nodename, SL_DEFAULT_HOST);
      strncpy (nodeport, slconn->sladdr + 1, sizeof (nodeport) - 1);
    }
  }
  /* Otherwise if no separator, use default port */
  else if (ptr == NULL)
  {
    strncpy (nodename, slconn->sladdr, sizeof (nodename) - 1);
    strcpy (nodeport, SL_DEFAULT_PORT);
  }
  /* Otherwise separate host and port */
  else if ((ptr - slconn->sladdr) < sizeof (nodename))
  {
    strncpy (nodename, slconn->sladdr, (ptr - slconn->sladdr));
    nodename[(ptr - slconn->sladdr)] = '\0';
    strncpy (nodeport, ptr + 1, sizeof (nodeport) - 1);
  }

  /* Sanity test the port number */
  nport = strtoul (nodeport, &tail, 10);
  if (*tail || (nport <= 0 || nport > 0xffff))
  {
    sl_log_r (slconn, 2, 0, "server port specified incorrectly\n");
    slconn->terminate = 1;
    return -1;
  }

  /* Resolve for either IPv4 or IPv6 (PF_UNSPEC) for a TCP stream (SOCK_STREAM) */
  memset (&hints, 0, sizeof (hints));
  hints.ai_family   = PF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  /* Resolve server address */
  if (getaddrinfo (nodename, nodeport, &hints, &addr0))
  {
    sl_log_r (slconn, 2, 0, "cannot resolve hostname %s\n", nodename);
    return -1;
  }

  /* Traverse address results trying to connect */
  sock = -1;
  for (addr = addr0; addr != NULL; addr = addr->ai_next)
  {
    /* Create socket */
    if ((sock = socket (addr->ai_family, addr->ai_socktype, addr->ai_protocol)) < 0)
    {
      continue;
    }

    /* Set socket I/O timeouts if possible */
    if (slconn->iotimeout)
    {
      timeout = (slconn->iotimeout > 0) ? slconn->iotimeout : -slconn->iotimeout;

      if (setsocktimeo_int (sock, timeout) == 1)
      {
        /* Negate timeout to indicate socket timeouts are set */
        slconn->iotimeout = -timeout;
      }
    }

    /* Connect socket */
    if ((sockconnect_int (sock, addr->ai_addr, addr->ai_addrlen)))
    {
      sockclose_int (sock);
      sock = -1;
      continue;
    }

    break;
  }

  if (sock < 0)
  {
    sl_log_r (slconn, 2, 0, "[%s] Cannot connect: %s\n", slconn->sladdr, sl_strerror ());
    sockclose_int (sock);
    freeaddrinfo (addr0);
    return -1;
  }

  freeaddrinfo (addr0);

  if (slconn->iotimeout < 0)
  {
    sl_log_r (slconn, 1, 2, "[%s] using system socket timeouts\n", slconn->sladdr);
  }

  /* Set non-blocking IO */
  if (socknoblock_int (sock))
  {
    sl_log_r (slconn, 2, 0, "Error setting socket to non-blocking\n");
    sockclose_int (sock);
  }

  /* Wait up to 10 seconds for the socket to be connected */
  if ((sockstat = checksock_int (sock, 10, 0)) <= 0)
  {
    if (sockstat < 0)
    { /* select() returned error */
      sl_log_r (slconn, 2, 1, "[%s] socket connect error\n", slconn->sladdr);
    }
    else
    { /* socket time-out */
      sl_log_r (slconn, 2, 1, "[%s] socket connect time-out (10s)\n",
                slconn->sladdr);
    }

    sockclose_int (sock);
    return -1;
  }
  else if (!slconn->terminate)
  { /* socket connected */
    sl_log_r (slconn, 1, 1, "[%s] network socket opened\n", slconn->sladdr);

    /* Set the SO_KEEPALIVE socket option, although not really useful */
    if (setsockopt (sock, SOL_SOCKET, SO_KEEPALIVE, (char *)&on, sizeof (on)) < 0)
      sl_log_r (slconn, 1, 1, "[%s] cannot set SO_KEEPALIVE socket option\n",
                slconn->sladdr);

    slconn->link = sock;

    if (slconn->batchmode)
      slconn->batchmode = 1;

    /* Everything should be connected, get capabilities if requested */
    if (sayhello)
    {
      if (sayhello_int (slconn) == -1)
      {
        sockclose_int (sock);
        return -1;
      }
    }

    /* Try to enter batch mode if requested */
    if (slconn->batchmode)
    {
      if (batchmode_int (slconn) == -1)
      {
        sockclose_int (sock);
        return -1;
      }
    }

    return sock;
  }

  return -1;
} /* End of sl_connect() */

/***************************************************************************
 * sl_configlink:
 *
 * Configure/negotiate data stream(s) with the remote SeedLink
 * server.  Negotiation will be either uni or multi-station
 * depending on the value of 'multistation' in the SLCD
 * struct.
 *
 * Returns -1 on errors, otherwise returns the link descriptor.
 ***************************************************************************/
SOCKET
sl_configlink (SLCD *slconn)
{
  SOCKET ret = slconn->link;

  if (slconn->proto_major == 4)
  {
    ret = negotiate_v4 (slconn);
  }
  else if (slconn->multistation)
  {
    if (sl_checkversion (slconn, 2, 5) >= 0)
    {
      ret = negotiate_multi_v3 (slconn);
    }
    else
    {
      sl_log_r (slconn, 2, 0,
                "[%s] Protocol version (%d.%d) does not support multi-station protocol\n",
                slconn->sladdr, slconn->proto_major, slconn->proto_minor);
      ret = -1;
    }
  }
  else if (slconn->proto_major <= 3)
  {
    ret = negotiate_uni_v3 (slconn);
  }

  return ret;
} /* End of sl_configlink() */

/***************************************************************************
 * sl_send_info:
 *
 * Send a request for the specified INFO level.  The verbosity level
 * can be specified, allowing control of when the request should be
 * logged.
 *
 * Returns -1 on errors, otherwise the socket descriptor.
 ***************************************************************************/
int
sl_send_info (SLCD *slconn, const char *infostr, int verbose)
{
  char sendstr[100]; /* A buffer for command strings */

  if (sl_checkversion (slconn, 2, 92) >= 0)
  {
    sprintf (sendstr, "INFO %s\r", infostr);

    sl_log_r (slconn, 1, verbose, "[%s] requesting INFO %s\n",
              slconn->sladdr, infostr);

    if (sl_senddata (slconn, (void *)sendstr, strlen (sendstr),
                     slconn->sladdr, (void *)NULL, 0) < 0)
    {
      sl_log_r (slconn, 2, 0, "[%s] error sending INFO request\n", slconn->sladdr);
      return -1;
    }
  }
  else
  {
    sl_log_r (slconn, 2, 0,
              "[%s] Protocol version (%d.%d) does not support INFO requests\n",
              slconn->sladdr, slconn->proto_major, slconn->proto_minor);
    return -1;
  }

  return slconn->link;
} /* End of sl_send_info() */

/***************************************************************************
 * sl_disconnect:
 *
 * Close the network socket associated with connection
 *
 * Returns -1, historically used to set the old descriptor
 ***************************************************************************/
int
sl_disconnect (SLCD *slconn)
{
  if (slconn->link != -1)
  {
    sockclose_int (slconn->link);
    slconn->link = -1;

    sl_log_r (slconn, 1, 1, "[%s] network socket closed\n", slconn->sladdr);
  }

  return -1;
} /* End of sl_disconnect() */

/***************************************************************************
 * sl_ping:
 *
 * Connect to a server, issue the HELLO command, parse out the server
 * ID and organization resonse and disconnect.  The server ID and
 * site/organization strings are copied into serverid and site strings
 * which should have 100 characters of space each.
 *
 * Returns:
 *   0  Success
 *  -1  Connection opened but invalid response to 'HELLO'.
 *  -2  Could not open network connection
 ***************************************************************************/
int
sl_ping (SLCD *slconn, char *serverid, char *site)
{
  int servcnt = 0;
  int sitecnt = 0;
  char sendstr[100] = {0}; /* A buffer for command strings */
  char servstr[100] = {0}; /* The remote server ident */
  char sitestr[100] = {0}; /* The site/data center ident */

  /* Open network connection to server */
  if (sl_connect (slconn, 0) == -1)
  {
    sl_log_r (slconn, 2, 1, "Could not connect to server\n");
    return -2;
  }

  /* Send HELLO */
  sprintf (sendstr, "HELLO\r");
  sl_log_r (slconn, 1, 2, "[%s] sending: HELLO\n", slconn->sladdr);
  sl_senddata (slconn, (void *)sendstr, strlen (sendstr), slconn->sladdr,
               NULL, 0);

  /* Recv the two lines of response */
  if (sl_recvresp (slconn, (void *)servstr, (size_t)sizeof (servstr),
                   sendstr, slconn->sladdr) < 0)
  {
    return -1;
  }

  if (sl_recvresp (slconn, (void *)sitestr, (size_t)sizeof (sitestr),
                   sendstr, slconn->sladdr) < 0)
  {
    return -1;
  }

  servcnt = strcspn (servstr, "\r");
  if (servcnt > 99)
  {
    servcnt = 99;
  }
  servstr[servcnt] = '\0';

  sitecnt = strcspn (sitestr, "\r");
  if (sitecnt > 99)
  {
    sitecnt = 99;
  }
  sitestr[sitecnt] = '\0';

  /* Copy the response strings into the supplied strings */
  strcpy (serverid, servstr);
  strcpy (site, sitestr);

  slconn->link = sl_disconnect (slconn);

  return 0;
} /* End of sl_ping() */

/***************************************************************************
 * sl_senddata:
 *
 * send() 'buflen' bytes from 'buffer' to 'slconn->link'.  'ident' is
 * a string to include in error messages for identification, usually
 * the address of the remote server.  If 'resp' is not NULL then read
 * up to 'resplen' bytes into 'resp' after sending 'buffer'.  This is
 * only designed for small pieces of data, specifically the server
 * responses to commands terminated by '\r\n'.
 *
 * Returns -1 on error, and size (in bytes) of the response
 * received (0 if 'resp' == NULL).
 ***************************************************************************/
int
sl_senddata (SLCD *slconn, void *buffer, size_t buflen,
             const char *ident, void *resp, int resplen)
{
  int bytesread = 0; /* bytes read into resp */

  if (send (slconn->link, buffer, buflen, 0) < 0)
  {
    sl_log_r (slconn, 2, 0, "[%s] error sending '%.*s'\n",
              ident,
              (int)strcspn ((char *)buffer, "\r\n"),
              (char *)buffer);
    return -1;
  }

  /* If requested collect the response */
  if (resp != NULL)
  {
    memset (resp, 0, resplen);
    bytesread = sl_recvresp (slconn, resp, resplen, buffer, ident);
  }

  return bytesread;
} /* End of sl_senddata() */

/***************************************************************************
 * sl_recvdata:
 *
 * recv() 'maxbytes' data from 'slconn->link' into a specified
 * 'buffer'.  'ident' is a string to be included in error messages for
 * identification, usually the address of the remote server.
 *
 * Returns -1 on error/EOF, 0 for no available data and the number
 * of bytes read on success.
 ***************************************************************************/
int64_t
sl_recvdata (SLCD *slconn, void *buffer, size_t maxbytes,
             const char *ident)
{
  int64_t bytesread = 0;

  if (buffer == NULL)
  {
    return -1;
  }

  bytesread = recv (slconn->link, buffer, maxbytes, 0);

  if (bytesread == 0) /* should indicate TCP FIN or EOF */
  {
    sl_log_r (slconn, 1, 1, "[%s] recv():%" PRId64 " TCP FIN or EOF received\n",
              ident, bytesread);
    return -1;
  }
  else if (bytesread < 0)
  {
    if (noblockcheck_int ())
    {
      sl_log_r (slconn, 2, 0, "[%s] recv():%" PRId64 " %s\n", ident, bytesread,
                sl_strerror ());
      return -1;
    }

    /* no data available for NONBLOCKing IO */
    return 0;
  }

  return bytesread;
} /* End of sl_recvdata() */

/***************************************************************************
 * sl_recvresp:
 *
 * To receive a response to a command recv() one byte at a time until
 * '\r\n' or up to 'maxbytes' is read from 'slconn->link' into a
 * specified 'buffer'.  The function will wait up to 30 seconds for a
 * response to be recv'd.  'command' is a string to be included in
 * error messages indicating which command the response is
 * for. 'ident' is a string to be included in error messages for
 * identification, usually the address of the remote server.
 *
 * It should not be assumed that the populated buffer contains a
 * terminated string.
 *
 * Returns -1 on error/EOF and the number of bytes read on success.
 ***************************************************************************/
int
sl_recvresp (SLCD *slconn, void *buffer, size_t maxbytes,
             const char *command, const char *ident)
{

  int bytesread = 0;     /* total bytes read */
  int recvret   = 0;     /* return from sl_recvdata */
  int ackcnt    = 0;     /* counter for the read loop */
  int ackpoll   = 50000; /* poll at 0.05 seconds for reading */

  if (buffer == NULL)
  {
    return -1;
  }

  /* Clear the receiving buffer */
  memset (buffer, 0, maxbytes);

  /* Recv a byte at a time and wait up to 30 seconds for a response */
  while (bytesread < maxbytes)
  {
    recvret = sl_recvdata (slconn, (char *)buffer + bytesread, 1, ident);

    /* Trap door for termination */
    if (slconn->terminate)
    {
      return -1;
    }

    if (recvret > 0)
    {
      bytesread += recvret;
    }
    else if (recvret < 0)
    {
      sl_log_r (slconn, 2, 0, "[%s] bad response to '%.*s'\n",
                ident,
                (int)strcspn (command, "\r\n"),
                command);
      return -1;
    }

    /* Done if '\r\n' is recv'd */
    if (bytesread >= 2 &&
        *(char *)((char *)buffer + bytesread - 2) == '\r' &&
        *(char *)((char *)buffer + bytesread - 1) == '\n')
    {
      return bytesread;
    }

    /* Trap door if 30 seconds has elapsed, (ackpoll x 600) */
    if (ackcnt > 600)
    {
      sl_log_r (slconn, 2, 0, "[%s] timeout waiting for response to '%.*s'\n",
                ident,
                (int)strcspn (command, "\r\n"),
                command);
      return -1;
    }

    /* Delay if no data received */
    if (recvret == 0)
    {
      sl_usleep (ackpoll);
      ackcnt++;
    }
  }

  return bytesread;
} /* End of sl_recvresp() */

/***************************************************************************
 * sayhello_int:
 *
 * Send the HELLO and other commands to determine server capabilities.
 *
 * The connection is promoted to the highest version supported by both
 * server and client.
 *
 * Returns -1 on errors, 0 on success.
 ***************************************************************************/
static int
sayhello_int (SLCD *slconn)
{
  int ret     = 0;
  int servcnt = 0;
  int sitecnt = 0;
  char sendstr[100];   /* A buffer for command strings */
  char servstr[200];   /* The remote server ident */
  char sitestr[100];   /* The site/data center ident */
  char servid[100];    /* Server ID string, i.e. 'SeedLink' */
  char *capptr;        /* Pointer to capabilities flags */
  char capflag = 0;    /* CAPABILITIES command is supported by server */

  int bytesread = 0;
  char readbuf[1024];

  /* Send HELLO */
  sprintf (sendstr, "HELLO\r");
  sl_log_r (slconn, 1, 2, "[%s] sending: %s\n", slconn->sladdr, sendstr);
  sl_senddata (slconn, (void *)sendstr, strlen (sendstr), slconn->sladdr,
               NULL, 0);

  /* Recv the two lines of response: server ID and site installation ID */
  if (sl_recvresp (slconn, (void *)servstr, (size_t)sizeof (servstr),
                   sendstr, slconn->sladdr) < 0)
  {
    return -1;
  }

  if (sl_recvresp (slconn, (void *)sitestr, (size_t)sizeof (sitestr),
                   sendstr, slconn->sladdr) < 0)
  {
    return -1;
  }

  /* Terminate on first "\r" character or at one character before end of buffer */
  servcnt = strcspn (servstr, "\r");
  if (servcnt > (sizeof (servstr) - 2))
  {
    servcnt = (sizeof (servstr) - 2);
  }
  servstr[servcnt] = '\0';

  sitecnt = strcspn (sitestr, "\r");
  if (sitecnt > (sizeof (sitestr) - 2))
  {
    sitecnt = (sizeof (sitestr) - 2);
  }
  sitestr[sitecnt] = '\0';

  /* Search for capabilities flags in server ID by looking for "::"
   * The expected format of the complete server ID is:
   * "SeedLink v#.# <optional text> <:: optional capability flags>"
   */
  capptr = strstr (servstr, "::");
  if (capptr)
  {
    /* Truncate server ID portion of string */
    *capptr = '\0';

    /* Move pointer to beginning of flags */
    capptr += 2;

    /* Move capptr up to first non-space character */
    while (*capptr == ' ')
      capptr++;

    if (slconn->capabilities)
      free (slconn->capabilities);
    if (slconn->caparray)
      free (slconn->caparray);

    slconn->capabilities = strdup(capptr);
    slconn->caparray = NULL;
  }

  /* Report server details */
  sl_log_r (slconn, 1, 1, "[%s] connected to: %s\n", slconn->sladdr, servstr);
  sl_log_r (slconn, 1, 1, "[%s] organization: %s\n", slconn->sladdr, sitestr);

  /* Parse old-school server ID and version from the returned string.
   * The expected format at this point is:
   * "SeedLink v#.# <optional text>"
   * where 'SeedLink' is case insensitive and '#.#' is the server/protocol version.
   */
  /* Add a space to the end to allowing parsing when the optionals are not present */
  servstr[servcnt]     = ' ';
  servstr[servcnt + 1] = '\0';
  ret                  = sscanf (servstr, "%s v%" SCNu8 ".%" SCNu8,
                                 &servid[0],
                                 &slconn->server_major,
                                 &slconn->server_minor);

  if (strncasecmp (servid, "SEEDLINK", 8))
  {
    sl_log_r (slconn, 2, 0,
              "[%s] unrecognized server identification: '%s'\n",
              slconn->sladdr, servid);
    return -1;
  }

  /* Check capability flags included in HELLO response */
  capptr = slconn->capabilities;
  while (*capptr)
  {
    while (*capptr == ' ')
      capptr++;

    if (!strncmp (capptr, "SLPROTO:", 8))
    {
      /* This protocol specification overrides any earlier determination */
      ret = sscanf (capptr, "SLPROTO:%"SCNu8".%"SCNu8,
                    &slconn->server_major, &slconn->server_minor);

      if (ret < 1)
      {
        sl_log_r (slconn, 1, 1,
                  "[%s] could not parse protocol version from SLPROTO flag: %s\n",
                  slconn->sladdr, capptr);
        slconn->server_major = 0;
        slconn->server_minor = 0;
      }

      capptr += 8;
    }
    else if (!strncmp (capptr, "CAP", 3))
    {
      capptr += 3;
      capflag = 1;
    }

    capptr++;
  }

  /* Promote protocol to 4.x if supported by server */
  if (slconn->server_major == 4)
  {
    /* Send maximum protocol version supported by library */
    sprintf (sendstr,
             "SLPROTO %u.%u\r",
             SL_PROTO_MAJOR,
             SL_PROTO_MINOR);

    /* Send SLPROTO and recv response */
    sl_log_r (slconn, 1, 2, "[%s] sending: %s\n", slconn->sladdr, sendstr);
    bytesread = sl_senddata (slconn, (void *)sendstr, strlen (sendstr), slconn->sladdr,
                             readbuf, sizeof (readbuf));

    if (bytesread < 0)
    { /* Error from sl_senddata() */
      return -1;
    }

    /* Check response to SLPROTO */
    if (!strncmp (readbuf, "OK\r", 3) && bytesread >= 4)
    {
      sl_log_r (slconn, 1, 2, "[%s] SLPROTO %u.%u accepted\n", slconn->sladdr,
                slconn->server_major, slconn->server_minor);
    }
    else if (!strncmp (readbuf, "ERROR", 5) && bytesread >= 6)
    {
      char *cp = readbuf + (bytesread-1);

      /* Trim space, \r, and \n while terminating response string */
      while (*cp == ' ' || *cp == '\r' || *cp == '\n')
        *cp-- = '\0';

      sl_log_r (slconn, 1, 2, "[%s] SLPROTO not accepted: %s\n", slconn->sladdr,
                readbuf+6);
      return -1;
    }
    else
    {
      sl_log_r (slconn, 2, 0,
                "[%s] invalid response to SLPROTO command: %.*s\n",
                slconn->sladdr, bytesread, readbuf);
      return -1;
    }

    slconn->proto_major = slconn->server_major;
    slconn->proto_minor = slconn->server_minor;
  }

  /* Send GETCAPABILITIES if supported by server */
  if (slconn->proto_major == 4)
  {
    uint8_t server_major = 0;
    uint8_t server_minor = 0;

    /* Send GETCAPABILITIES and recv response */
    sl_log_r (slconn, 1, 2, "[%s] sending: GETCAPABILITIES\n", slconn->sladdr);
    bytesread = sl_senddata (slconn, "GETCAPABILITIES\r\n", 17, slconn->sladdr,
                             readbuf, sizeof (readbuf));

    if (bytesread < 0)
    { /* Error from sl_senddata() */
      return -1;
    }

    /* Response is a string of space-separated flags terminated with \r\n */
    if (bytesread > 2)
    {
      char *cp = readbuf + (bytesread-1);

      /* Trim space, \r, and \n while terminating response string */
      while (*cp == ' ' || *cp == '\r' || *cp == '\n')
        *cp-- = '\0';

      if (slconn->capabilities)
        free (slconn->capabilities);
      if (slconn->caparray)
        free (slconn->caparray);

      slconn->capabilities = strdup(readbuf);
      slconn->caparray = NULL;
    }

    /* Parse highest protocol version from capabilities */
    capptr = slconn->capabilities;
    while (*capptr)
    {
      while (*capptr == ' ')
        capptr++;

      if (!strncmp (capptr, "SLPROTO:", 8))
      {
        /* This protocol specification overrides any earlier determination */
        ret = sscanf (capptr, "SLPROTO:%"SCNu8".%"SCNu8,
                      &server_major, &server_minor);

        if (ret < 1)
        {
          sl_log_r (slconn, 1, 1,
                    "[%s] could not parse protocol version from SLPROTO flag: %s\n",
                    slconn->sladdr, capptr);
          server_major = 0;
          server_minor = 0;
        }
        else if (server_major > slconn->server_major)
        {
          slconn->server_major = server_major;
          slconn->server_minor = server_minor;
        }
        else if (server_major == slconn->server_major &&
                 server_minor > slconn->server_minor)
        {
          slconn->server_minor = server_minor;
        }

        capptr += 8;
      }

      capptr++;
    }
  }
  /* Otherwise, send CAPABILITIES flags if supported by server */
  else if (capflag)
  {
    char *term1, *term2;
    char *extreply = 0;

    /* Send maximum protocol version and EXTREPLY capability flag */
    sprintf (sendstr,
             "CAPABILITIES SLPROTO:%u.%u EXTREPLY\r",
             SL_PROTO_MAJOR,
             SL_PROTO_MINOR);

    /* Send CAPABILITIES and recv response */
    sl_log_r (slconn, 1, 2, "[%s] sending: %s\n", slconn->sladdr, sendstr);
    bytesread = sl_senddata (slconn, (void *)sendstr, strlen (sendstr), slconn->sladdr,
                             readbuf, sizeof (readbuf));

    if (bytesread < 0)
    { /* Error from sl_senddata() */
      return -1;
    }

    /* Search for 2nd "\r" indicating extended reply message present */
    extreply = 0;
    if ((term1 = memchr (readbuf, '\r', bytesread)))
    {
      if ((term2 = memchr (term1 + 1, '\r', bytesread - (readbuf - term1) - 1)))
      {
        *term2   = '\0';
        extreply = term1 + 1;
      }
    }

    /* Check response to CAPABILITIES */
    if (!strncmp (readbuf, "OK\r", 3) && bytesread >= 4)
    {
      sl_log_r (slconn, 1, 2, "[%s] capabilities OK %s%s%s\n", slconn->sladdr,
                (extreply) ? "{" : "", (extreply) ? extreply : "", (extreply) ? "}" : "");
    }
    else if (!strncmp (readbuf, "ERROR\r", 6) && bytesread >= 7)
    {
      sl_log_r (slconn, 1, 2, "[%s] CAPABILITIES not accepted %s%s%s\n", slconn->sladdr,
                (extreply) ? "{" : "", (extreply) ? extreply : "", (extreply) ? "}" : "");
      return -1;
    }
    else
    {
      sl_log_r (slconn, 2, 0,
                "[%s] invalid response to CAPABILITIES command: %.*s\n",
                slconn->sladdr, bytesread, readbuf);
      return -1;
    }
  }

  /* Report server capabilities */
  if (slconn->capabilities)
    sl_log_r (slconn, 1, 1, "[%s] capabilities: %s\n", slconn->sladdr,
              (slconn->capabilities) ? slconn->capabilities : "");

  /* Set protocol version to server version for <= 3 protocols */
  if (slconn->server_major <= 3)
  {
    slconn->proto_major = slconn->server_major;
    slconn->proto_minor = slconn->server_minor;
  }

  /* Send USERAGENT if protocol >= v4 */
  if (slconn->proto_major == 4)
  {
    /* Create USERAGENT, optional client name and version */
    sprintf (sendstr,
             "USERAGENT %s%s%s libslink/%s\r",
             (slconn->clientname) ? slconn->clientname : "",
             (slconn->clientname && slconn->clientversion) ? "/" : "",
             (slconn->clientname && slconn->clientversion) ? slconn->clientversion : "",
             LIBSLINK_VERSION);

    /* Send USERAGENT and recv response */
    sl_log_r (slconn, 1, 2, "[%s] sending: %s\n", slconn->sladdr, sendstr);
    bytesread = sl_senddata (slconn, (void *)sendstr, strlen (sendstr), slconn->sladdr,
                             readbuf, sizeof (readbuf));

    if (bytesread < 0)
    { /* Error from sl_senddata() */
      return -1;
    }

    /* Check response to USERAGENT */
    if (!strncmp (readbuf, "OK\r", 3) && bytesread >= 4)
    {
      sl_log_r (slconn, 1, 2, "[%s] USERAGENT accepted\n",
                slconn->sladdr);
    }
    else if (!strncmp (readbuf, "ERROR", 5) && bytesread >= 6)
    {
      char *cp = readbuf + (bytesread-1);

      /* Trim space, \r, and \n while terminating response string */
      while (*cp == ' ' || *cp == '\r' || *cp == '\n')
        *cp-- = '\0';

      sl_log_r (slconn, 1, 2, "[%s] USERAGENT not accepted: %s\n", slconn->sladdr,
                readbuf+6);
      return -1;
    }
    else
    {
      sl_log_r (slconn, 2, 0,
                "[%s] invalid response to USERAGENT command: %.*s\n",
                slconn->sladdr, bytesread, readbuf);
      return -1;
    }
  }

  return 0;
} /* End of sayhello_int() */

/***************************************************************************
 * batchmode_int:
 *
 * Send the BATCH command to switch the connection to batch command
 * mode, in this mode the server will not send acknowledgments (OK or
 * ERROR) after recognized commands are submitted.
 *
 * Returns -1 on errors, 0 on success (regardless if the command was accepted).
 * Sets slconn->batchmode accordingly.
 ***************************************************************************/
static int
batchmode_int (SLCD *slconn)
{
  char sendstr[100]; /* A buffer for command strings */
  char readbuf[100]; /* A buffer for server reply */
  int bytesread = 0;

  if (!slconn)
    return -1;

  if (sl_checkversion (slconn, 3, 1) < 0 || slconn->proto_major != 3)
  {
    sl_log_r (slconn, 2, 0,
              "[%s] Protocol version (%d.%d) does not support the BATCH command\n",
              slconn->sladdr, slconn->proto_major, slconn->proto_minor);
    return -1;
  }

  /* Send BATCH and recv response */
  sprintf (sendstr, "BATCH\r");
  sl_log_r (slconn, 1, 2, "[%s] sending: %s\n", slconn->sladdr, sendstr);
  bytesread = sl_senddata (slconn, (void *)sendstr, strlen (sendstr), slconn->sladdr,
                           readbuf, sizeof (readbuf));

  if (bytesread < 0)
  { /* Error from sl_senddata() */
    return -1;
  }

  /* Check response to BATCH */
  if (!strncmp (readbuf, "OK\r", 3) && bytesread >= 4)
  {
    sl_log_r (slconn, 1, 2, "[%s] BATCH accepted\n", slconn->sladdr);
    slconn->batchmode = 2;
  }
  else if (!strncmp (readbuf, "ERROR\r", 6) && bytesread >= 7)
  {
    sl_log_r (slconn, 1, 2, "[%s] BATCH not accepted\n", slconn->sladdr);
  }
  else
  {
    sl_log_r (slconn, 2, 0,
              "[%s] invalid response to BATCH command: %.*s\n",
              slconn->sladdr, bytesread, readbuf);
    return -1;
  }

  return 0;
} /* End of batchmode_int() */

/***************************************************************************
 * negotiate_uni_v3:
 *
 * Negotiate stream details with protocol 3 in uni-station mode and
 * issue the DATA command.  This is compatible with SeedLink Protocol
 * version 2 or greater.
 *
 * If 'selectors' != 0 then the string is parsed on space and each
 * selector is sent.
 *
 * If 'seqnum' != SL_UNSETSEQUENCE and the SLCD 'resume' flag is true
 * then data is requested starting at seqnum.
 *
 * Returns -1 on errors, otherwise returns the link descriptor.
 ***************************************************************************/
static SOCKET
negotiate_uni_v3 (SLCD *slconn)
{
  int sellen    = 0;
  int bytesread = 0;
  int acceptsel = 0; /* Count of accepted selectors */
  char *selptr;
  char *extreply = 0;
  char *term1, *term2;
  char sendstr[100]; /* A buffer for command strings */
  char readbuf[100]; /* A buffer for responses */
  SLstream *curstream;

  /* Point to the stream chain */
  curstream = slconn->streams;

  selptr = curstream->selectors;

  /* Send the selector(s) and check the response(s) */
  if (curstream->selectors != 0)
  {
    while (1)
    {
      selptr += sellen;
      selptr += strspn (selptr, " ");
      sellen = strcspn (selptr, " ");

      if (sellen == 0)
      {
        break; /* end of while loop */
      }
      else
      {

        /* Build SELECT command, send it and receive response */
        sprintf (sendstr, "SELECT %.*s\r", sellen, selptr);
        sl_log_r (slconn, 1, 2, "[%s] sending: SELECT %.*s\n", slconn->sladdr,
                  sellen, selptr);
        bytesread = sl_senddata (slconn, (void *)sendstr,
                                 strlen (sendstr), slconn->sladdr,
                                 readbuf, sizeof (readbuf));
        if (bytesread < 0)
        { /* Error from sl_senddata() */
          return -1;
        }

        /* Search for 2nd "\r" indicating extended reply message present */
        extreply = 0;
        if ((term1 = memchr (readbuf, '\r', bytesread)))
        {
          if ((term2 = memchr (term1 + 1, '\r', bytesread - (readbuf - term1) - 1)))
          {
            *term2   = '\0';
            extreply = term1 + 1;
          }
        }

        /* Check response to SELECT */
        if (!strncmp (readbuf, "OK\r", 3) && bytesread >= 4)
        {
          sl_log_r (slconn, 1, 2, "[%s] selector %.*s is OK %s%s%s\n", slconn->sladdr,
                    sellen, selptr, (extreply) ? "{" : "", (extreply) ? extreply : "", (extreply) ? "}" : "");
          acceptsel++;
        }
        else if (!strncmp (readbuf, "ERROR\r", 6) && bytesread >= 7)
        {
          sl_log_r (slconn, 1, 2, "[%s] selector %.*s not accepted %s%s%s\n", slconn->sladdr,
                    sellen, selptr, (extreply) ? "{" : "", (extreply) ? extreply : "", (extreply) ? "}" : "");
        }
        else
        {
          sl_log_r (slconn, 2, 0,
                    "[%s] invalid response to SELECT command: %.*s\n",
                    slconn->sladdr, bytesread, readbuf);
          return -1;
        }
      }
    }

    /* Fail if none of the given selectors were accepted */
    if (!acceptsel)
    {
      sl_log_r (slconn, 2, 0, "[%s] no data stream selector(s) accepted\n",
                slconn->sladdr);
      return -1;
    }
    else
    {
      sl_log_r (slconn, 1, 2, "[%s] %d selector(s) accepted\n",
                slconn->sladdr, acceptsel);
    }
  } /* End of selector processing */

  /* Issue the DATA, FETCH or TIME action commands.  A specified start (and
     optionally, stop time) takes precedence over the resumption from any
     previous sequence number. */
  if (slconn->begin_time != NULL)
  {
    if (sl_checkversion (slconn, 2, 92) >= 0)
    {
      if (slconn->end_time == NULL)
      {
        sprintf (sendstr, "TIME %.30s\r", slconn->begin_time);
      }
      else
      {
        sprintf (sendstr, "TIME %.30s %.30s\r", slconn->begin_time,
                 slconn->end_time);
      }
      sl_log_r (slconn, 1, 1, "[%s] requesting specified time window\n",
                slconn->sladdr);
    }
    else
    {
      sl_log_r (slconn, 2, 0,
                "[%s] Protocol version (%d.%d) does not support TIME windows\n",
                slconn->sladdr, slconn->proto_major, slconn->proto_minor);
    }
  }
  else if (curstream->seqnum != SL_UNSETSEQUENCE && slconn->resume)
  {
    char cmd[10];

    if (slconn->dialup)
    {
      sprintf (cmd, "FETCH");
    }
    else
    {
      sprintf (cmd, "DATA");
    }

    /* Append the last packet time if the feature is enabled and server is >= 2.93 */
    if (slconn->lastpkttime &&
        sl_checkversion (slconn, 2, 93) >= 0 &&
        strlen (curstream->timestamp))
    {
      /* Increment sequence number by 1 */
      sprintf (sendstr, "%s %0" PRIX64 " %.30s\r", cmd,
               (curstream->seqnum + 1), curstream->timestamp);

      sl_log_r (slconn, 1, 1,
                "[%s] resuming data from %0" PRIX64 " (Dec %"PRIu64") at %.30s\n",
                slconn->sladdr, (curstream->seqnum + 1),
                (curstream->seqnum + 1), curstream->timestamp);
    }
    else
    {
      /* Increment sequence number by 1 */
      sprintf (sendstr, "%s %0" PRIX64 "\r", cmd,
               (curstream->seqnum + 1));

      sl_log_r (slconn, 1, 1,
                "[%s] resuming data from %0" PRIX64 " (Dec %" PRIu64 ")\n",
                slconn->sladdr, (curstream->seqnum + 1),
                (curstream->seqnum + 1));
    }
  }
  else
  {
    if (slconn->dialup)
    {
      sprintf (sendstr, "FETCH\r");
    }
    else
    {
      sprintf (sendstr, "DATA\r");
    }

    sl_log_r (slconn, 1, 1, "[%s] requesting next available data\n", slconn->sladdr);
  }

  if (sl_senddata (slconn, (void *)sendstr, strlen (sendstr),
                   slconn->sladdr, (void *)NULL, 0) < 0)
  {
    sl_log_r (slconn, 2, 0, "[%s] error sending DATA/FETCH/TIME request\n", slconn->sladdr);
    return -1;
  }

  return slconn->link;
} /* End of negotiate_uni_v3() */

/***************************************************************************
 * negotiate_multi_v3:
 *
 * Negotiate stream selection with protocol 3 in multi-station mode
 * and issue the END command to start streaming.
 *
 * If 'curstream->selectors' != 0 then the string is parsed on space
 * and each selector is sent.
 *
 * If 'curstream->seqnum' != SL_UNSETSEQUENCE and the SLCD 'resume'
 * flag is true then data is requested starting at seqnum.
 *
 * Returns -1 on errors, otherwise returns the link descriptor.
 ***************************************************************************/
static SOCKET
negotiate_multi_v3 (SLCD *slconn)
{
  int sellen    = 0;
  int bytesread = 0;
  int acceptsta = 0; /* Count of accepted stations */
  int acceptsel = 0; /* Count of accepted selectors */
  char *selptr;
  char *term1, *term2;
  char *extreply = 0;
  char sendstr[100]; /* A buffer for command strings */
  char readbuf[100]; /* A buffer for responses */
  char netstaid[12]; /* Network-station identifier */
  SLstream *curstream;

  /* Point to the stream chain */
  curstream = slconn->streams;

  /* Loop through the stream chain */
  while (curstream != NULL)
  {
    /* A network-station identifier */
    snprintf (netstaid, sizeof (netstaid), "%s_%s",
              curstream->net, curstream->sta);

    /* Send the STATION command */
    sprintf (sendstr, "STATION %s %s\r", curstream->sta, curstream->net);
    sl_log_r (slconn, 1, 2, "[%s] sending: STATION %s %s\n",
              netstaid, curstream->sta, curstream->net);

    bytesread = sl_senddata (slconn, (void *)sendstr, strlen (sendstr), netstaid,
                             (slconn->batchmode == 2) ? (void *)NULL : readbuf,
                             sizeof (readbuf));

    if (bytesread < 0)
    {
      return -1;
    }
    else if (bytesread == 0 && slconn->batchmode == 2)
    {
      acceptsta++;
    }
    else
    {
      /* Search for 2nd "\r" indicating extended reply message present */
      extreply = 0;
      if ((term1 = memchr (readbuf, '\r', bytesread)))
      {
        if ((term2 = memchr (term1 + 1, '\r', bytesread - (readbuf - term1) - 1)))
        {
          *term2   = '\0';
          extreply = term1 + 1;
        }
      }

      /* Check the response */
      if (!strncmp (readbuf, "OK\r", 3) && bytesread >= 4)
      {
        sl_log_r (slconn, 1, 2, "[%s] station is OK %s%s%s\n", netstaid,
                  (extreply) ? "{" : "", (extreply) ? extreply : "", (extreply) ? "}" : "");
        acceptsta++;
      }
      else if (!strncmp (readbuf, "ERROR\r", 6) && bytesread >= 7)
      {
        sl_log_r (slconn, 2, 0, "[%s] station not accepted %s%s%s\n", netstaid,
                  (extreply) ? "{" : "", (extreply) ? extreply : "", (extreply) ? "}" : "");
        /* Increment the loop control and skip to the next stream */
        curstream = curstream->next;
        continue;
      }
      else
      {
        sl_log_r (slconn, 2, 0, "[%s] invalid response to STATION command: %.*s\n",
                  netstaid, bytesread, readbuf);
        return -1;
      }
    }

    selptr = curstream->selectors;
    sellen = 0;

    /* Send the selector(s) and check the response(s) */
    if (curstream->selectors != 0)
    {
      while (1)
      {
        selptr += sellen;
        selptr += strspn (selptr, " ");
        sellen = strcspn (selptr, " ");

        if (sellen == 0)
        {
          break; /* end of while loop */
        }
        else
        {
          /* Build SELECT command, send it and receive response */
          sprintf (sendstr, "SELECT %.*s\r", sellen, selptr);
          sl_log_r (slconn, 1, 2, "[%s] sending: SELECT %.*s\n", netstaid, sellen,
                    selptr);

          bytesread = sl_senddata (slconn, (void *)sendstr, strlen (sendstr), netstaid,
                                   (slconn->batchmode == 2) ? (void *)NULL : readbuf,
                                   sizeof (readbuf));

          if (bytesread < 0)
          {
            return -1;
          }
          else if (bytesread == 0 && slconn->batchmode == 2)
          {
            acceptsel++;
          }
          else
          {
            /* Search for 2nd "\r" indicating extended reply message present */
            extreply = 0;
            if ((term1 = memchr (readbuf, '\r', bytesread)))
            {
              if ((term2 = memchr (term1 + 1, '\r', bytesread - (readbuf - term1) - 1)))
              {
                *term2   = '\0';
                extreply = term1 + 1;
              }
            }

            /* Check response to SELECT */
            if (!strncmp (readbuf, "OK\r", 3) && bytesread >= 4)
            {
              sl_log_r (slconn, 1, 2, "[%s] selector %.*s is OK %s%s%s\n", netstaid,
                        sellen, selptr, (extreply) ? "{" : "", (extreply) ? extreply : "", (extreply) ? "}" : "");
              acceptsel++;
            }
            else if (!strncmp (readbuf, "ERROR\r", 6) && bytesread >= 7)
            {
              sl_log_r (slconn, 2, 0, "[%s] selector %.*s not accepted %s%s%s\n", netstaid,
                        sellen, selptr, (extreply) ? "{" : "", (extreply) ? extreply : "", (extreply) ? "}" : "");
            }
            else
            {
              sl_log_r (slconn, 2, 0,
                        "[%s] invalid response to SELECT command: %.*s\n",
                        netstaid, bytesread, readbuf);
            return -1;
            }
          }
        }
      }

      /* Fail if none of the given selectors were accepted */
      if (!acceptsel)
      {
        sl_log_r (slconn, 2, 0, "[%s] no data stream selector(s) accepted\n",
                  netstaid);
        return -1;
      }
      else
      {
        sl_log_r (slconn, 1, 2, "[%s] %d selector(s) accepted\n", netstaid,
                  acceptsel);
      }

      acceptsel = 0; /* Reset the accepted selector count */

    } /* End of selector processing */

    /* Issue the DATA, FETCH or TIME action commands.  A specified start (and
       optionally, stop time) takes precedence over the resumption from any
       previous sequence number. */
    if (slconn->begin_time != NULL)
    {
      if (sl_checkversion (slconn, 2, 92) >= 0)
      {
        if (slconn->end_time == NULL)
        {
          sprintf (sendstr, "TIME %.30s\r", slconn->begin_time);
        }
        else
        {
          sprintf (sendstr, "TIME %.30s %.30s\r", slconn->begin_time,
                   slconn->end_time);
        }
        sl_log_r (slconn, 1, 1, "[%s] requesting specified time window\n",
                  netstaid);
      }
      else
      {
        sl_log_r (slconn, 2, 0,
                  "[%s] Protocol version (%d.%d) does not support TIME windows\n",
                  netstaid, slconn->proto_major, slconn->proto_minor);
      }
    }
    else if (curstream->seqnum != SL_UNSETSEQUENCE && slconn->resume)
    {
      char cmd[10];

      if (slconn->dialup)
      {
        sprintf (cmd, "FETCH");
      }
      else
      {
        sprintf (cmd, "DATA");
      }

      /* Append the last packet time if the feature is enabled and server is >= 2.93 */
      if (slconn->lastpkttime &&
          sl_checkversion (slconn, 2, 93) >= 0 &&
          strlen (curstream->timestamp))
      {
        /* Increment sequence number by 1 */
        sprintf (sendstr, "%s %0" PRIX64 " %.30s\r", cmd,
                 (curstream->seqnum + 1), curstream->timestamp);

        sl_log_r (slconn, 1, 1,
                  "[%s] resuming data from %0" PRIX64 " (Dec %" PRIu64 ") at %.30s\n",
                  slconn->sladdr, (curstream->seqnum + 1),
                  (curstream->seqnum + 1), curstream->timestamp);
      }
      else
      { /* Increment sequence number by 1 */
        sprintf (sendstr, "%s %0" PRIX64 "\r", cmd,
                 (curstream->seqnum + 1));

        sl_log_r (slconn, 1, 1,
                  "[%s] resuming data from %0" PRIX64 " (Dec %" PRIu64 ")\n", netstaid,
                  (curstream->seqnum + 1),
                  (curstream->seqnum + 1));
      }
    }
    else
    {
      if (slconn->dialup)
      {
        sprintf (sendstr, "FETCH\r");
      }
      else
      {
        sprintf (sendstr, "DATA\r");
      }

      sl_log_r (slconn, 1, 1, "[%s] requesting next available data\n", netstaid);
    }

    /* Send the TIME/DATA/FETCH command and receive response */
    bytesread = sl_senddata (slconn, (void *)sendstr, strlen (sendstr), netstaid,
                             (slconn->batchmode == 2) ? (void *)NULL : readbuf,
                             sizeof (readbuf));

    if (bytesread < 0)
    {
      return -1;
    }
    else if (bytesread > 0)
    {
      /* Search for 2nd "\r" indicating extended reply message present */
      extreply = 0;
      if ((term1 = memchr (readbuf, '\r', bytesread)))
      {
        if ((term2 = memchr (term1 + 1, '\r', bytesread - (readbuf - term1) - 1)))
        {
          *term2   = '\0';
          extreply = term1 + 1;
        }
      }

      /* Check response to DATA/FETCH/TIME request */
      if (!strncmp (readbuf, "OK\r", 3) && bytesread >= 4)
      {
        sl_log_r (slconn, 1, 2, "[%s] DATA/FETCH/TIME command is OK %s%s%s\n", netstaid,
                  (extreply) ? "{" : "", (extreply) ? extreply : "", (extreply) ? "}" : "");
      }
      else if (!strncmp (readbuf, "ERROR\r", 6) && bytesread >= 7)
      {
        sl_log_r (slconn, 2, 0, "[%s] DATA/FETCH/TIME command is not accepted %s%s%s\n", netstaid,
                  (extreply) ? "{" : "", (extreply) ? extreply : "", (extreply) ? "}" : "");
      }
      else
      {
        sl_log_r (slconn, 2, 0, "[%s] invalid response to DATA/FETCH/TIME command: %.*s\n",
                  netstaid, bytesread, readbuf);
        return -1;
      }
    }

    /* Point to the next stream */
    curstream = curstream->next;

  } /* End of stream and selector config (end of stream chain). */

  /* Fail if no stations were accepted */
  if (!acceptsta)
  {
    sl_log_r (slconn, 2, 0, "[%s] no station(s) accepted\n", slconn->sladdr);
    return -1;
  }
  else
  {
    sl_log_r (slconn, 1, 1, "[%s] %d station(s) accepted\n",
              slconn->sladdr, acceptsta);
  }

  /* Issue END action command */
  sprintf (sendstr, "END\r");
  sl_log_r (slconn, 1, 2, "[%s] sending: END\n", slconn->sladdr);
  if (sl_senddata (slconn, (void *)sendstr, strlen (sendstr),
                   slconn->sladdr, (void *)NULL, 0) < 0)
  {
    sl_log_r (slconn, 2, 0, "[%s] error sending END command\n", slconn->sladdr);
    return -1;
  }

  return slconn->link;
} /* End of negotiate_multi_v3() */

/***************************************************************************
 * negotiate_v4:
 *
 * Negotiate stream selection with protocol 4 and issue the END
 * command to start streaming.
 *
 * If 'curstream->selectors' != 0 then the string is parsed on space
 * and each selector is sent.
 *
 * If 'curstream->seqnum' != SL_UNSETSEQUENCE and the SLCD 'resume'
 * flag is true then data is requested starting at seqnum.
 *
 * Returns -1 on errors, otherwise returns the link descriptor.
 ***************************************************************************/
static SOCKET
negotiate_v4 (SLCD *slconn)
{
  int time_capability = 0; /* Flag: server supports TIME capability */
  int stationcnt      = 0; /* Station count */
  int errorcnt        = 0; /* Error count */
  int bytesread       = 0;
  int sellen          = 0;
  char *selptr;
  char *cp;
  char sendstr[10];  /* A buffer for small command strings */
  char readbuf[200]; /* A buffer for responses */
  char netstaid[22]; /* Network-station identifier */
  SLstream *curstream;

  struct cmd_s
  {
    char cmd[100];
    char nsid[22];
    struct cmd_s *next;
  };

  struct cmd_s *cmdlist = NULL;
  struct cmd_s *cmdtail = NULL;
  struct cmd_s *cmdptr = NULL;

  if (!slconn)
    return -1;

  /* Point to the stream chain */
  curstream = slconn->streams;

  /* Determine if server supports TIME capability */
  if (sl_hascapability (slconn, "TIME"))
  {
    time_capability = 1;
  }

  /* Loop through the stream chain */
  while (curstream != NULL)
  {
    /* A network-station identifier */
    snprintf (netstaid, sizeof (netstaid), "%s_%s",
              curstream->net, curstream->sta);

    /* Allocate new command in list */
    if ((cmdptr = (struct cmd_s *)malloc(sizeof(struct cmd_s))) == NULL)
    {
      sl_log_r (slconn, 2, 0, "%s() Cannot allocate memory\n", __func__);
      while (cmdlist)
      {
        cmdptr = cmdlist->next;
        free (cmdlist);
        cmdlist = cmdptr;
      }

      return -1;
    }

    if (!cmdlist)
    {
      cmdlist = cmdptr;
      cmdtail = cmdptr;
    }
    else
    {
      cmdtail->next = cmdptr;
      cmdtail = cmdptr;
    }

    strcpy (cmdtail->nsid, netstaid);
    cmdtail->next = NULL;

    /* Generate STATION command */
    snprintf (cmdtail->cmd, sizeof(cmdtail->cmd),
              "STATION %s_%s\r",
              curstream->net, curstream->sta);

    stationcnt++;

    selptr = curstream->selectors;
    sellen = 0;

    /* Send the selector(s) */
    if (curstream->selectors != 0)
    {
      while (1)
      {
        selptr += sellen;
        selptr += strspn (selptr, " ");
        sellen = strcspn (selptr, " ");

        if (sellen == 0)
          break; /* end of while loop */
        else
        {
          /* Allocate new command in list */
          if ((cmdptr = (struct cmd_s *)malloc(sizeof(struct cmd_s))) == NULL)
          {
            sl_log_r (slconn, 2, 0, "%s() Cannot allocate memory\n", __func__);
            while (cmdlist)
            {
              cmdptr = cmdlist->next;
              free (cmdlist);
              cmdlist = cmdptr;
            }

            return -1;
          }

          cmdtail->next = cmdptr;
          cmdtail = cmdptr;
          strcpy (cmdtail->nsid, netstaid);
          cmdtail->next = NULL;

          /* Generate SELECT command */
          snprintf (cmdtail->cmd, sizeof(cmdtail->cmd),
                    "SELECT %.*s\r",
                    sellen, selptr);
        }
      }
    } /* End of selector processing */

    /* Allocate new command in list */
    if ((cmdptr = (struct cmd_s *)malloc(sizeof(struct cmd_s))) == NULL)
    {
      sl_log_r (slconn, 2, 0, "%s() Cannot allocate memory\n", __func__);
      while (cmdlist)
      {
        cmdptr = cmdlist->next;
        free (cmdlist);
        cmdlist = cmdptr;
      }

      return -1;
    }

    cmdtail->next = cmdptr;
    cmdtail = cmdptr;
    strcpy (cmdtail->nsid, netstaid);
    cmdtail->next = NULL;

    /* Generate DATA or FETCH command with:
     *   - optional sequence number, INCREMENTED
     *   - optional time window if supported by server */
    if (time_capability && slconn->begin_time != NULL)
    {
      if (curstream->seqnum != SL_UNSETSEQUENCE)
      {
        snprintf (cmdtail->cmd, sizeof(cmdtail->cmd),
                  "%s %" PRIu64 "%s%s%s\r",
                  (slconn->dialup) ? "FETCH" : "DATA",
                  (curstream->seqnum + 1),
                  slconn->begin_time,
                  (slconn->end_time) ? " " : "",
                  (slconn->end_time) ? slconn->end_time : "");
      }
      else
      {
        snprintf (cmdtail->cmd, sizeof(cmdtail->cmd),
                  "%s -1 %s%s%s\r",
                  (slconn->dialup) ? "FETCH" : "DATA",
                  slconn->begin_time,
                  (slconn->end_time) ? " " : "",
                  (slconn->end_time) ? slconn->end_time : "");
      }
    }
    else
    {
      if (curstream->seqnum != SL_UNSETSEQUENCE)
      {
        snprintf (cmdtail->cmd, sizeof(cmdtail->cmd),
                  "%s %" PRIu64 "\r",
                  (slconn->dialup) ? "FETCH" : "DATA",
                  (curstream->seqnum + 1));
      }
      else
      {
        snprintf (cmdtail->cmd, sizeof(cmdtail->cmd),
                  "%s\r",
                  (slconn->dialup) ? "FETCH" : "DATA");
      }
    }

    /* Point to the next stream */
    curstream = curstream->next;
  } /* End of stream and selector config */

  /* Send all generated commands */
  cmdptr = cmdlist;
  while (cmdptr)
  {
    sl_log_r (slconn, 1, 2, "[%s] sending: %s\n",
              cmdptr->nsid, cmdptr->cmd);

    bytesread = sl_senddata (slconn, (void *)cmdptr->cmd,
                             strlen (cmdptr->cmd), cmdptr->nsid,
                             (void *)NULL, 0);

    if (bytesread < 0)
    {
      while (cmdlist)
      {
        cmdptr = cmdlist->next;
        free (cmdlist);
        cmdlist = cmdptr;
      }

      return -1;
    }

    cmdptr = cmdptr->next;
  }

  /* Receive all responses */
  cmdptr = cmdlist;
  while (cmdptr)
  {
    bytesread = sl_recvresp (slconn, readbuf, sizeof (readbuf), NULL, netstaid);

    if (bytesread < 0)
    {
      while (cmdlist)
      {
        cmdptr = cmdlist->next;
        free (cmdlist);
        cmdlist = cmdptr;
      }

      return -1;
    }

    /* Terminate command and response at first carriage return */
    if ((cp = strchr(cmdptr->cmd, '\r')))
      *cp = '\0';
    if ((cp = strchr(readbuf, '\r')))
      *cp = '\0';

    if (bytesread >= 2 && !strncmp (readbuf, "OK", 2))
    {
      sl_log_r (slconn, 1, 2, "[%s] Command OK (%s)\n",
                cmdptr->nsid, cmdptr->cmd);
    }
    else if (bytesread >= 5 && !strncmp (readbuf, "ERROR", 5))
    {
      sl_log_r (slconn, 2, 0, "[%s] Command not accepted (%s): %s\n",
                cmdptr->nsid, cmdptr->cmd, readbuf);
      errorcnt++;
    }
    else
    {
      sl_log_r (slconn, 2, 0, "[%s] invalid response to command (%s): %s\n",
                cmdptr->nsid, cmdptr->cmd, readbuf);
      errorcnt++;
    }

    cmdptr = cmdptr->next;
  }

  if (errorcnt == 0)
  {
    sl_log_r (slconn, 1, 1, "[%s] %d station(s) accepted\n",
              slconn->sladdr, stationcnt);

    /* Issue END command to finalize stream selection and start streaming */
    sprintf (sendstr, "END\r");
    sl_log_r (slconn, 1, 2, "[%s] sending: END\n", slconn->sladdr);
    if (sl_senddata (slconn, (void *)sendstr, strlen (sendstr),
                     slconn->sladdr, (void *)NULL, 0) < 0)
    {
      sl_log_r (slconn, 2, 0, "[%s] error sending END command\n", slconn->sladdr);
      errorcnt++;
    }
  }

  /* Free command list */
  while (cmdlist)
  {
    cmdptr = cmdlist->next;
    free (cmdlist);
    cmdlist = cmdptr;
  }

  return (errorcnt) ? -1 : slconn->link;
} /* End of negotiate_v4() */

/***************************************************************************
 * Check a socket for write ability using select() and read ability
 * using recv(... MSG_PEEK).  Time-out values are also passed (seconds
 * and microseconds) for the select() call.
 *
 * Returns:
 *  1 = success
 *  0 = if time-out expires
 * -1 = errors
 ***************************************************************************/
static int
checksock_int (SOCKET sock, int tosec, int tousec)
{
  int sret;
  int ret = -1; /* default is failure */
  char testbuf[1];
  fd_set checkset;
  struct timeval to;

  FD_ZERO (&checkset);
  FD_SET (sock, &checkset);

  to.tv_sec  = tosec;
  to.tv_usec = tousec;

  /* Check write ability with select() */
  if ((sret = select (sock + 1, NULL, &checkset, NULL, &to)) > 0)
    ret = 1;
  else if (sret == 0)
    ret = 0; /* time-out expired */

  /* Check read ability with recv() */
  if (ret && (recv (sock, testbuf, sizeof (char), MSG_PEEK)) <= 0)
  {
    if (!noblockcheck_int ())
      ret = 1; /* no data for non-blocking IO */
    else
      ret = -1;
  }

  return ret;
}

/***************************************************************************
 * Startup the network socket layer.  At the moment this is only meaningful
 * for the WIN platform.
 *
 * Returns -1 on errors and 0 on success.
 ***************************************************************************/
static int
sockstartup_int (void)
{
#if defined(SLP_WIN)
  WORD wVersionRequested;
  WSADATA wsaData;

  /* Check for Windows sockets version 2.2 */
  wVersionRequested = MAKEWORD (2, 2);

  if (WSAStartup (wVersionRequested, &wsaData))
    return -1;

#endif

  return 0;
}

/***************************************************************************
 * Connect a network socket.
 *
 * Returns -1 on errors and 0 on success.
 ***************************************************************************/
static int
sockconnect_int (SOCKET sock, struct sockaddr *inetaddr, int addrlen)
{
#if defined(SLP_WIN)
  if ((connect (sock, inetaddr, addrlen)) == SOCKET_ERROR)
  {
    if (WSAGetLastError () != WSAEWOULDBLOCK)
      return -1;
  }
#else
  if ((connect (sock, inetaddr, addrlen)) == -1)
  {
    if (errno != EINPROGRESS)
      return -1;
  }
#endif

  return 0;
}

/***************************************************************************
 * Close a network socket.
 *
 * Returns -1 on errors and 0 on success.
 ***************************************************************************/
static int
sockclose_int (SOCKET sock)
{
#if defined(SLP_WIN)
  return closesocket (sock);
#else
  return close (sock);
#endif
}

/***************************************************************************
 * Set a network socket to non-blocking.
 *
 * Returns -1 on errors and 0 on success.
 ***************************************************************************/
static int
socknoblock_int (SOCKET sock)
{
#if defined(SLP_WIN)
  u_long flag = 1;

  if (ioctlsocket (sock, FIONBIO, &flag) == -1)
    return -1;

#else
  int flags = fcntl (sock, F_GETFL, 0);

  flags |= O_NONBLOCK;
  if (fcntl (sock, F_SETFL, flags) == -1)
    return -1;

#endif

  return 0;
}

/***************************************************************************
 * Check global status for wether blocking would occur.
 *
 * Return -1 on error and 0 on success (meaning no data for a non-blocking
 * socket).
 ***************************************************************************/
static int
noblockcheck_int (void)
{
#if defined(SLP_WIN)
  if (WSAGetLastError () != WSAEWOULDBLOCK)
    return -1;

#else
  if (errno != EWOULDBLOCK)
    return -1;

#endif

  /* no data available for NONBLOCKing IO */
  return 0;
}

/***********************************************************************/ /**
 * @brief Set socket I/O timeout
 *
 * Set socket I/O timeout if such an option exists.  On WIN and
 * other platforms where SO_RCVTIMEO and SO_SNDTIMEO are defined this
 * sets the SO_RCVTIMEO and SO_SNDTIMEO socket options using
 * setsockopt() to the @a timeout value (specified in seconds).
 *
 * Solaris does not implelement socket-level timeout options.
 *
 * @param socket Network socket descriptor
 * @param timeout Alarm timeout in seconds
 *
 * @return -1 on error, 0 when not possible and 1 on success.
 ***************************************************************************/
static int
setsocktimeo_int (SOCKET socket, int timeout)
{
#if defined(SLP_WIN)
  int tval = timeout * 1000;

  if (setsockopt (socket, SOL_SOCKET, SO_RCVTIMEO, (char *)&tval, sizeof (tval)))
  {
    return -1;
  }
  tval = timeout * 1000;
  if (setsockopt (socket, SOL_SOCKET, SO_SNDTIMEO, (char *)&tval, sizeof (tval)))
  {
    return -1;
  }

#else
/* Set socket I/O timeouts if socket options are defined */
#if defined(SO_RCVTIMEO) && defined(SO_SNDTIMEO)
  struct timeval tval;

  tval.tv_sec  = timeout;
  tval.tv_usec = 0;

  if (setsockopt (socket, SOL_SOCKET, SO_RCVTIMEO, &tval, sizeof (tval)))
  {
    return -1;
  }
  if (setsockopt (socket, SOL_SOCKET, SO_SNDTIMEO, &tval, sizeof (tval)))
  {
    return -1;
  }
#else
  return 0;
#endif

#endif

  return 1;
}
