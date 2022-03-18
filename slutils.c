/***************************************************************************
 * slutils.c
 *
 * Routines for managing a connection with a SeedLink server
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
#include <stdlib.h>
#include <string.h>

#include "globmatch.h"
#include "libslink.h"
#include "mseedformat.h"

/* Function(s) only used in this source file */
static int receive_netstaid (SLCD *slconn, int bytesavailable);
static int receive_header (SLCD *slconn, int bytesavailable);
static int64_t receive_v3payload (SLCD *slconn, char *plbuffer, uint32_t plbuffersize,
                                  int bytesavailable);
static int64_t receive_v4payload (SLCD *slconn, char *plbuffer, uint32_t plbuffersize,
                                  int bytesavailable);
static int update_stream (SLCD *slconn, const char *payload);
static int64_t detect (const char *record, uint64_t recbuflen, char *payloadformat);
static int recvable (SOCKET sock);

/**********************************************************************/ /**
 * @brief Managage a connection to a SeedLink server and collect packets
 *
 * Designed to run in a loop of a client program, this function can
 * return full or partial packets.  The caller is responsible for
 * consuming the data on each return.  Keepalives will be sent if
 * configured for the connection. This routing can operate in blocking
 * or non-blocking mode.
 *
 * The \a plbuffer size will be increased up to \a maxbuffersize to
 * accommodate whole packets.  The system's realloc() is used to
 * reallocate the buffer.  The buffer's size is never reduced.
 *
 * \note The caller is responsible for freeing the memory allocated at
 * \a plbuffer.
 *
 * The returned \a packetinfo contains the details including: sequence
 * number, payload length, payload type, and how much of the payload
 * has been returned so far.
 *
 * If the \a slconn.noblock flags is set, the function will return
 * quickly even if no data are available, setting \a plbytes to 0 when
 * no data are returned.  If the flag is not set, the function will
 * block and only return if data are available.
 *
 * @param[in]  slconn  SeedLink connection description
 * @param[in]  packetinfo  Pointer to ::SLpacketinfo
 * @param[in|out] plbuffer Pointer to packet payload buffer
 * @param[in|out] plbuffersize Size of payload buffer
 * @param[in]  maxbuffersize Maximum size of payload buffer
 *
 * @returns An @ref collect-status code
 * @retval SLPACKET Complete packet returned
 * @retval SLTERMINATE Connection termination or error
 * @retval SLNOPACKET  No packet available
 ***************************************************************************/
int
sl_collect (SLCD *slconn, const SLpacketinfo **packetinfo,
            char **plbuffer, uint32_t *plbuffersize,
            uint32_t maxbuffersize)
{
  uint32_t payloadbytes = 0;
  uint32_t received = 0;
  uint32_t resize;

  if (!slconn || !packetinfo || !plbuffer || !plbuffersize)
    return SLTERMINATE;

  /* Initial allocation of payload buffer */
  if (*plbuffersize == 0)
  {
    /* Minimum of 512 or specified maximum */
    resize = (maxbuffersize < 512) ? maxbuffersize : 512;

    if ((*plbuffer = (char *)malloc(resize)) == NULL)
    {
      sl_log (2, 0, "%s() cannot allocate memory\n", __func__);
      *packetinfo = NULL;

      return SLTERMINATE;
    }

    slconn->stat->payload = *plbuffer;
    *plbuffersize = resize;
  }

  /* Loop with sl_receive() */
  while (sl_receive (slconn,
                     *plbuffer + received,
                     *plbuffersize - received,
                     &payloadbytes) != NULL)
  {
    received += payloadbytes;

    /* If no new payload received (non-blocking) return */
    if (payloadbytes == 0)
    {
      *packetinfo = NULL;
      return SLNOPACKET;
    }

    /* Check if payload is larger than allowed maximum */
    if (slconn->stat->packetinfo.payloadlength > maxbuffersize)
    {
      *packetinfo = &slconn->stat->packetinfo;
      return SLTOOLARGE;
    }
    /* Increase buffer size if with allowed maximum */
    else if (slconn->stat->packetinfo.payloadlength > *plbuffersize)
    {
      *plbuffer = (char *)realloc (*plbuffer, slconn->stat->packetinfo.payloadlength);

      if (*plbuffer == NULL)
      {
        sl_log (2, 0, "%s() cannot allocate memory\n", __func__);
        *packetinfo = NULL;

        return SLTERMINATE;
      }

      slconn->stat->payload = *plbuffer;
      *plbuffersize = slconn->stat->packetinfo.payloadlength;
    }
    /* Return complete packets */
    else if (slconn->stat->packetinfo.payloadlength == slconn->stat->packetinfo.payloadcollected)
    {
      *packetinfo = &slconn->stat->packetinfo;
      return SLPACKET;
    }
  }

  return SLTERMINATE;
}  /* End of sl_collect() */

/**********************************************************************/ /**
 * @brief Managage a connection to a SeedLink server and collect packets
 *
 * The sl_collect() function is a wrapper of this function that
 * manages a buffer for receiving complete packets and is recommended
 * for most uses.
 *
 * Designed to run in a loop of a client program, this function can
 * return full or partial packets.  The caller is responsible for
 * consuming the data on each return.  This routine will send
 * keepalives if configured for the connection and can operate in
 * blocking or non-blocking mode.
 *
 * \note While the function can operate with a small buffer in many
 * cases, it is recommended to use a buffer than can contain complete
 * packet payloads expected for efficiency and simplicity.
 *
 * The returned \a packetinfo contains the details including: sequence
 * number, payload length, payload type, and how much of the payload
 * has been returned so far.
 *
 * This function will not return more than a whole packet payload, the
 * caller does not need to buffer data for following packet.
 *
 * \note The function can return partial payload data and the client
 * must handle partial returns.  Detecting incomplete packets and
 * constructing whole packets is an exercise for the caller.  A
 * complete payload has been returned when \a packetinfo.payloadlength
 * and \a packetinfo.payloadcollected are equal.
 *
 * If the \a slconn.noblock flags is set, the function will return
 * quickly even if no data are available, setting \a plbytes to 0 when
 * no data are returned.  If the flag is not set, the function will
 * block and only return if data are available.
 *
 * The slconn->stats->payload pointer should be set to beginning of the
 * payload buffer to facilitate stream tracking and v3 payload detection.
 *
 * @param[in]  slconn   SeedLink connection description
 * @param[out] plbuffer Destination buffer for data packet payload
 * @param[in]  plbuffersize Length of destination buffer
 * @param[out] plbytes  Number of bytes returned in destination buffer
 *
 * @retval ::SLpacketinfo Description of current packet (if any), \a plbytes
 *    will be set to the length of payload data written to \a plbuffer,
 *    or 0 when no data is returned.
 * @retval NULL Connection termination or error, connection has been closed.
 ***************************************************************************/
const SLpacketinfo *
sl_receive (SLCD *slconn, char *plbuffer, uint32_t plbuffersize, uint32_t *plbytes)
{
  int64_t bytesread;
  int64_t current_time;
  int bytesavailable;

  struct timeval select_tv;
  fd_set select_fd;
  int select_ret;

  if (!slconn || (plbuffersize > 0 && !plbuffer))
    return NULL;

  /* Start the primary loop */
  while (!slconn->terminate)
  {
    current_time = sl_nstime();

    if (slconn->link == -1)
    {
      slconn->stat->conn_state = DOWN;
    }

    /* Throttle the loop while delaying */
    if (slconn->stat->conn_state == DOWN &&
        slconn->stat->netdly_time &&
        slconn->stat->netdly_time > current_time)
    {
      sl_usleep (500000);
    }

    /* Connect to server if disconnected */
    if (slconn->stat->conn_state == DOWN &&
        slconn->stat->netdly_time < current_time)
    {
      if (sl_connect (slconn, 1) != -1)
      {
        slconn->stat->conn_state = UP;
      }
      slconn->stat->netto_time     = 0;
      slconn->stat->netdly_time    = 0;
      slconn->stat->keepalive_time = 0;
    }

    /* Negotiate/configure the connection */
    if (slconn->stat->conn_state == UP)
    {
      if (slconn->streams)
      {
        if (sl_configlink (slconn) == -1)
        {
          sl_log_r (slconn, 2, 0, "[%s] %s(): negotiation with server failed\n",
                    slconn->sladdr, __func__);
          slconn->link              = sl_disconnect (slconn);
          slconn->stat->netdly_time = 0;
        }
      }

      slconn->stat->conn_state = STREAMING;
    }

    /* Send INFO request if one not in progress */
    if (slconn->stat->conn_state == STREAMING &&
        slconn->stat->query_state == NoQuery &&
        slconn->info)
    {
      if (sl_send_info (slconn, slconn->info, 1) != -1)
      {
        slconn->stat->query_state = InfoQuery;
      }
      else
      {
        slconn->stat->query_state = NoQuery;
      }

      slconn->info = NULL;
    }

    /* Read incoming data stream */
    if (slconn->stat->conn_state == STREAMING)
    {
      bytesread = 0;

      FD_ZERO (&select_fd);
      FD_SET ((unsigned int)slconn->link, &select_fd);
      select_tv.tv_sec  = 0;

      /* Wait up to 0.5 seconds when blocking, otherwise 100 usec */
      select_tv.tv_usec = (!slconn->noblock) ? 500000 : 100;

      select_ret = select ((slconn->link + 1), &select_fd, NULL, NULL, &select_tv);

      /* Check the return from select(), an interrupted system call error
         will be reported if a signal handler was used.  If the terminate
         flag is set this is not an error. */
      if (select_ret < 0 && !slconn->terminate)
      {
        sl_log_r (slconn, 2, 0, "[%s] %s(): select() error: %s\n",
                  slconn->sladdr, __func__, sl_strerror ());
        break;
      }

      /* Read available data */
      if (select_ret > 0 && FD_ISSET (slconn->link, &select_fd))
      {
        bytesavailable = recvable (slconn->link);

        if (bytesavailable < 0)
        {
          sl_log_r (slconn, 2, 0, "[%s] %s(): recvable() returned error\n",
                    slconn->sladdr, __func__);
          break;
        }

        bytesread = 0;

        /* Read next header */
        if (slconn->stat->stream_state == HEADER)
        {
          if ((slconn->proto_major <= 3 && bytesavailable >= SLHEADSIZE) ||
              (slconn->proto_major == 4 && bytesavailable >= SLHEADSIZE_EXT))
          {
            slconn->stat->packetinfo.netstaidlength = 0;
            slconn->stat->packetinfo.netstaid[0] = '\0';

            bytesread = receive_header (slconn, bytesavailable);

            if (bytesread < 0)
            {
              break;
            }
            else if (bytesread > 0)
            {
              /* Set state for network-station ID or payload collection */
              if (slconn->stat->packetinfo.netstaidlength > 0)
              {
                slconn->stat->packetinfo.netstaid[0] = '\0';
                slconn->stat->stream_state = NETSTAID;
              }
              else
              {
                slconn->stat->packetinfo.payloadcollected = 0;
                slconn->stat->stream_state = PAYLOAD;
              }

              bytesavailable -= bytesread;
            }
          }
          else
          {
            /* A narrow case of small data, check for special cases */
            if (bytesavailable > 0)
            {
              bytesread = recv (slconn->link,
                                slconn->stat->packetinfo.header,
                                sizeof(slconn->stat->packetinfo.header),
                                MSG_PEEK);

              if (bytesread >= 5 && !memcmp (slconn->stat->packetinfo.header, "ERROR", 5))
              {
                sl_log_r (slconn, 2, 0, "[%s] Server reported an error with the last command\n",
                          slconn->sladdr);
                break;
              }
              if (bytesread >= 3 && !memcmp (slconn->stat->packetinfo.header, "END", 3))
              {
                sl_log_r (slconn, 1, 1, "[%s] End of buffer or selected time window\n",
                          slconn->sladdr);
                break;
              }
            }
          }
        } /* Done reading header */

        /* Collect network-station ID */
        if (slconn->stat->stream_state == NETSTAID &&
            slconn->stat->packetinfo.netstaidlength > 0 &&
            bytesavailable >= slconn->stat->packetinfo.netstaidlength)
        {
          bytesread = receive_netstaid (slconn, bytesavailable);

          if (bytesread < 0)
          {
            break;
          }
          else if (bytesread > 0)
          {
            /* Set state for payload collection */
            slconn->stat->packetinfo.payloadcollected = 0;
            slconn->stat->stream_state = PAYLOAD;
            bytesavailable -= bytesread;
          }
        } /* Done reading network-station ID */

        /* Collect payload */
        if (slconn->stat->stream_state == PAYLOAD)
        {
          bytesread = 0;
          if (slconn->proto_major <= 3)
          {
            bytesread = receive_v3payload (slconn, plbuffer, plbuffersize, bytesavailable);
          }
          else if (slconn->proto_major == 4)
          {
            bytesread = receive_v4payload (slconn, plbuffer, plbuffersize, bytesavailable);
          }

          /* Set state for header collection if payload is complete */
          if (slconn->stat->packetinfo.payloadlength > 0 &&
              slconn->stat->packetinfo.payloadlength == slconn->stat->packetinfo.payloadcollected)
          {
            slconn->stat->stream_state = HEADER;
          }

          if (bytesread < 0) /* read() failed */
          {
            break;
          }
          else if (bytesread > 0) /* Return collected payload */
          {
            slconn->stat->netto_time     = 0;
            slconn->stat->keepalive_time = 0;

            *plbytes = bytesread;

            return &slconn->stat->packetinfo;
          }
        } /* Done reading payload */
      }
    } /* Done reading data */

    /* Update timing variables */
    current_time = sl_nstime ();

    /* Check for network idle timeout */
    if (slconn->stat->conn_state == STREAMING &&
        slconn->netto && slconn->stat->netto_time &&
        slconn->stat->netto_time < current_time)
    {
      sl_log_r (slconn, 1, 0, "[%s] network timeout (%ds), reconnecting in %ds\n",
                slconn->sladdr, slconn->netto, slconn->netdly);
      slconn->link              = sl_disconnect (slconn);
      slconn->stat->conn_state  = DOWN;
      slconn->stat->netto_time  = 0;
      slconn->stat->netdly_time = 0;
    }

    /* Check if keepalive packet needs to be sent */
    if (slconn->stat->conn_state == STREAMING &&
        slconn->stat->query_state == NoQuery &&
        slconn->keepalive && slconn->stat->keepalive_time &&
        slconn->stat->keepalive_time < current_time)
    {
      sl_log_r (slconn, 1, 2, "[%s] Sending keepalive message\n", slconn->sladdr);

      if (sl_send_info (slconn, "ID", 3) == -1)
      {
        break;
      }

      slconn->stat->query_state     = KeepAliveQuery;
      slconn->stat->keepalive_time = 0;
    }

    /* Network timeout */
    if (slconn->netto && slconn->stat->netto_time == 0)
    {
      slconn->stat->netto_time = current_time + SL_EPOCH2SLTIME (slconn->netto);
    }

    /* Network connection delay */
    if (slconn->netdly && slconn->stat->netdly_time == 0)
    {
      slconn->stat->netdly_time = current_time + SL_EPOCH2SLTIME (slconn->netdly);
    }

    /* Keepalive/heartbeat interval */
    if (slconn->keepalive && slconn->stat->keepalive_time == 0)
    {
      slconn->stat->keepalive_time = current_time + SL_EPOCH2SLTIME (slconn->keepalive);
    }

    /* Return if not waiting for data */
    if (slconn->noblock)
    {
      *plbytes = 0;
      return &slconn->stat->packetinfo;
    }
  } /* End of primary loop */

  /* Terminating, make sure connection is closed */
  if (slconn->link >= 0)
  {
    slconn->link = sl_disconnect (slconn);
  }

  return NULL;
} /* End of sl_receive() */


/***************************************************************************
 * receive_netstaid:
 *
 * Receive packet network-station ID following fixed-length header.
 * The value written to packetinfo.netstaid will be null-termianted.
 *
 * Returns:
 * bytes : Size of ID read
 * -1 :  on termination or error
 ***************************************************************************/
static int
receive_netstaid (SLCD *slconn, int bytesavailable)
{
  int64_t bytesread = 0;

  if (slconn->stat->packetinfo.netstaidlength >= bytesavailable)
  {
    return 0;
  }
  else if (slconn->stat->packetinfo.netstaidlength >
           (sizeof(slconn->stat->packetinfo.netstaid) - 1))
  {
    sl_log_r (slconn, 2, 0,
              "[%s] %s() received NET_STA ID is too large (%d) for buffer (%lu)\n",
              slconn->sladdr, __func__,
              slconn->stat->packetinfo.netstaidlength,
              sizeof (slconn->stat->packetinfo.netstaid));
    return -1;
  }

  bytesread = sl_recvdata (slconn, slconn->stat->packetinfo.netstaid,
                           slconn->stat->packetinfo.netstaidlength,
                           slconn->sladdr);

  if (bytesread < 0) /* recv() failed */
  {
    return -1;
  }
  else if (bytesread != slconn->stat->packetinfo.netstaidlength)
  {
    sl_log_r (slconn, 2, 0,
              "[%s] %s() read of %" PRId64 " bytes not the same as NET_STA ID length of %d\n",
              slconn->sladdr, __func__,
              bytesread,
              slconn->stat->packetinfo.netstaidlength);
    return -1;
  }

  slconn->stat->packetinfo.netstaid[bytesread] = '\0';

  return bytesread;
}  /* End of receive_netstaid() */


/***************************************************************************
 * receive_header:
 *
 * Receive packet header.
 *
 * Returns:
 * bytes : Size of header read
 * -1 :  on termination or error
 ***************************************************************************/
static int
receive_header (SLCD *slconn, int bytesavailable)
{
  int64_t bytesread = 0;
  uint32_t readsize = 0;
  char *tail = NULL;

  if (!slconn)
    return -1;

  if (slconn->proto_major <= 3 && bytesavailable >= SLHEADSIZE)
  {
    readsize = SLHEADSIZE;
    memset(slconn->stat->packetinfo.header + SLHEADSIZE, 0,
           sizeof(slconn->stat->packetinfo.header) - SLHEADSIZE);
  }
  else if (slconn->proto_major == 4 && bytesavailable >= SLHEADSIZE_EXT)
  {
    readsize = SLHEADSIZE_EXT;
  }
  else
  {
    sl_log_r (slconn, 2, 0, "[%s] %s() cannot determine read size, proto_major: %d, bytesavailable: %d\n",
              slconn->sladdr, __func__, slconn->proto_major, bytesavailable);
    return -1;
  }

  bytesread = sl_recvdata (slconn, slconn->stat->packetinfo.header,
                           readsize, slconn->sladdr);

  if (bytesread < 0) /* recv() failed */
  {
    return -1;
  }
  else if (bytesread > 0)
  {
    if (bytesread != readsize)
    {
      sl_log_r (slconn, 2, 0, "[%s] %s() incomplete header received, available: %d, read: %" PRId64 "\n",
                slconn->sladdr, __func__, bytesavailable, bytesread);
      return -1;
    }

    /* Catch special cases of stream interruption */
    if (bytesread >= 5 && !memcmp (slconn->stat->packetinfo.header, "ERROR", 5))
    {
      sl_log_r (slconn, 2, 0, "[%s] Server reported an error with the last command\n",
                slconn->sladdr);
      return -1;
    }
    if (bytesread >= 3 && !memcmp (slconn->stat->packetinfo.header, "END", 3))
    {
      sl_log_r (slconn, 1, 1, "[%s] End of buffer or selected time window\n",
                slconn->sladdr);
      return -1;
    }

    /* Parse v3 INFO header */
    if (!memcmp (slconn->stat->packetinfo.header, INFOSIGNATURE, 6))
    {
      slconn->stat->packetinfo.seqnum = SL_UNSETSEQUENCE;
      slconn->stat->packetinfo.payloadlength = 0;

      slconn->stat->packetinfo.payloadformat = (slconn->stat->packetinfo.header[SLHEADSIZE - 1] == '*') ?
        SLPAYLOAD_MSEED2INFO :
        SLPAYLOAD_MSEED2INFOTERM;
    }
    /* Parse v3 data header */
    else if (!memcmp (slconn->stat->packetinfo.header, SIGNATURE, 2))
    {
      slconn->stat->packetinfo.seqnum = strtoul (slconn->stat->packetinfo.header + 2, &tail, 16);

      if (*tail)
      {
        sl_log_r (slconn, 2, 0, "[%s] %s() cannot parse sequence number from v3 header: %8.8s\n",
                  slconn->sladdr, __func__, slconn->stat->packetinfo.header);
        return -1;
      }

      slconn->stat->packetinfo.payloadlength = 0;
      slconn->stat->packetinfo.payloadformat = SLPAYLOAD_UNKNOWN;
    }
    /* Parse v4 header */
    else if (!memcmp (slconn->stat->packetinfo.header, SIGNATURE_EXT, 2))
    {
      slconn->stat->packetinfo.payloadformat = slconn->stat->packetinfo.header[2];
      slconn->stat->packetinfo.payloadsubformat = slconn->stat->packetinfo.header[3];
      memcpy (&slconn->stat->packetinfo.payloadlength, slconn->stat->packetinfo.header + 4, 4);
      memcpy (&slconn->stat->packetinfo.seqnum, slconn->stat->packetinfo.header + 8, 8);
      memcpy (&slconn->stat->packetinfo.netstaidlength, slconn->stat->packetinfo.header + 16, 1);

      if (!sl_littleendianhost ())
      {
        sl_gswap8a (&slconn->stat->packetinfo.seqnum);
        sl_gswap4a (&slconn->stat->packetinfo.payloadlength);
      }
    }
    else
    {
      sl_log_r (slconn, 2, 0, "[%s] %s(): unexpected header signature not found (instead: %2.2s)\n",
                slconn->sladdr, __func__, slconn->stat->packetinfo.header);
      return -1;
    }
  }

  return bytesread;
} /* End of receive_header() */


/***************************************************************************
 * receive_v3payload:
 *
 * Receive packet payload for v3 protocol.
 *
 * The supplied buffer must be large enough for payload detection,
 * defined as SL_MIN_BUFFER bytes.
 *
 * Returns
 * bytes : Number of bytes written to 'plbuffer' on success
 * -1 :  on termination or error
 ***************************************************************************/
int64_t
receive_v3payload (SLCD *slconn, char *plbuffer, uint32_t plbuffersize,
                   int bytesavailable)
{
  int64_t bytesread = 0;
  uint32_t collected = 0;
  uint32_t readsize;
  uint32_t nextpow2;
  char payloadformat = SLPAYLOAD_UNKNOWN;
  int64_t detectedlength;

  if (!slconn || !plbuffer)
    return -1;

  if (!slconn->stat->payload)
  {
    sl_log_r (slconn, 2, 0, "[%s] %s(): required slconn->stat->payload pointer not set.\n",
              slconn->sladdr, __func__);
    return -1;
  }

  if (slconn->stat->packetinfo.payloadcollected == 0)
  {
    /* Initial payload read buffer must be large enough for detection */
    if (plbuffersize < SL_MIN_BUFFER)
    {
      sl_log_r (slconn, 2, 0, "[%s] %s(): provided buffer is too small (%u), must be at least %u bytes\n",
                slconn->sladdr, __func__, plbuffersize, SL_MIN_BUFFER);
      return -1;
    }

    /* Initial payload read must be at least SL_MIN_BUFFER */
    if (bytesavailable < SL_MIN_BUFFER)
    {
      return 0;
    }
  }

  while (bytesavailable > 0 && !slconn->terminate)
  {
    /* It is important that this routine does not read more than a single
       packet payload from the socket (there are other readers), so we employ
       logic to minimize the reads until we know the packet length.

       Strategy to read minimum needed for packet:
       - if miniSEED 3, SL_MIN_BUFFER is enough to determine the length
       - if miniSEED 2, increment read limit to next possible record length

       Packet length is unknown when set to 0.
    */
    if (slconn->stat->packetinfo.payloadlength == 0)
    {
      /* Collect up to SL_MIN_BUFFER initially */
      if (slconn->stat->packetinfo.payloadcollected < SL_MIN_BUFFER)
      {
        readsize = SL_MIN_BUFFER - slconn->stat->packetinfo.payloadcollected;
      }
      /* Otherwise, find bytes remaining to next power of 2 length */
      else
      {
        for (nextpow2 = 32; nextpow2 < slconn->stat->packetinfo.payloadcollected;)
          nextpow2 *= 2;

        readsize = nextpow2 - slconn->stat->packetinfo.payloadcollected;
      }
    }
    /* Packet length is known: read remaining */
    else if (slconn->stat->packetinfo.payloadlength > 0 &&
             slconn->stat->packetinfo.payloadlength > slconn->stat->packetinfo.payloadcollected)
    {
      readsize = slconn->stat->packetinfo.payloadlength - slconn->stat->packetinfo.payloadcollected;
    }
    else
    {
      sl_log_r (slconn, 2, 0, "[%s] %s(): cannot determine read size, payloadlength: %u, payloadcollected: %u\n",
                slconn->sladdr, __func__, slconn->stat->packetinfo.payloadlength, slconn->stat->packetinfo.payloadcollected);
      return -1;
    }

    /* Fail if payload length unknown and not enough buffer to continue detection */
    if (slconn->stat->packetinfo.payloadlength == 0 &&
        (collected + readsize) > plbuffersize)
    {
      sl_log_r (slconn, 2, 0, "[%s] %s(): provided buffer size insufficient for payload detection in %u bytes\n",
                slconn->sladdr, __func__, (collected + readsize));
      return -1;
    }

    /* Reduce read size to remaining buffer if smaller */
    if ((collected + readsize) > plbuffersize)
    {
      readsize = plbuffersize - collected;
    }

    bytesread = sl_recvdata (slconn, plbuffer + collected, readsize, slconn->sladdr);

    if (bytesread < 0) /* recv() failed */
    {
      return-1;
    }
    else if (bytesread > 0) /* Process collected payload */
    {
      slconn->stat->packetinfo.payloadcollected += bytesread;
      collected += bytesread;
      bytesavailable -= bytesread;

      /* Detect payload type and length if not yet determined */
      if (slconn->stat->packetinfo.payloadlength == 0 &&
          slconn->stat->packetinfo.payloadcollected >= SL_MIN_BUFFER)
      {
        detectedlength = detect (slconn->stat->payload,
                                 slconn->stat->packetinfo.payloadcollected,
                                 &payloadformat);

        /* Return error if no recognized payload detected */
        if (detectedlength < 0)
        {
          sl_log_r (slconn, 2, 0,
                    "[%s] %s(): non-miniSEED packet received for v3 protocol!?! Terminating.\n",
                    slconn->sladdr, __func__);
          return -1;
        }
        /* Update packet info if length detected */
        else if (detectedlength > 0)
        {
          if (slconn->stat->packetinfo.payloadformat == SLPAYLOAD_UNKNOWN)
          {
            slconn->stat->packetinfo.payloadformat = payloadformat;
          }

          slconn->stat->packetinfo.payloadlength = detectedlength;
        }
      }

      /* Handle payload of known length */
      if (slconn->stat->packetinfo.payloadlength > 0)
      {
        /* Update streaming tracking if initial payload */
        if (slconn->stat->packetinfo.payloadcollected == collected &&
            slconn->stat->packetinfo.payloadcollected >= SL_MIN_BUFFER)
        {
          if (update_stream (slconn, slconn->stat->payload) == -1)
          {
            sl_log_r (slconn, 2, 0, "[%s] %s(): cannot update stream tracking\n",
                      slconn->sladdr, __func__);
            return -1;
          }
        }

        /* Keepalive INFO responses are not returned to caller */
        if (slconn->stat->query_state == KeepAliveQuery &&
            (slconn->stat->packetinfo.payloadformat == SLPAYLOAD_MSEED2INFOTERM ||
             slconn->stat->packetinfo.payloadformat == SLPAYLOAD_MSEED2INFO))
        {
          collected = 0;
        }

        /* Payload is complete */
        if (slconn->stat->packetinfo.payloadcollected == slconn->stat->packetinfo.payloadlength)
        {
          if (slconn->stat->packetinfo.payloadformat == SLPAYLOAD_MSEED2INFOTERM)
          {
            if (slconn->stat->query_state == KeepAliveQuery)
            {
              sl_log_r (slconn, 1, 2, "[%s] Keepalive message received\n", slconn->sladdr);
            }

            slconn->stat->query_state = NoQuery;
          }
        }

        /* Return received data to caller */
        break;
      }
    }
  }

  return collected;
} /* End of receive_v3payload() */


/***************************************************************************
 * receive_v4payload:
 *
 * Receive packet payload for v4 protocol
 *
 * Returns
 * bytes : Number of bytes written to 'plbuffer' on success
 * -1 :  on termination or error
 ***************************************************************************/
static int64_t
receive_v4payload (SLCD *slconn, char *plbuffer, uint32_t plbuffersize,
                   int bytesavailable)
{
  int64_t bytesread = 0;
  uint32_t collected = 0;
  uint32_t readsize;

  if (!slconn || !plbuffer)
    return -1;

  if (!slconn->stat->payload)
  {
    sl_log_r (slconn, 2, 0, "[%s] %s(): required slconn->stat->payload pointer not set.\n",
              slconn->sladdr, __func__);
    return -1;
  }

  while (bytesavailable > 0 && !slconn->terminate)
  {
    /* Read size: remaining payload */
    readsize = slconn->stat->packetinfo.payloadlength - slconn->stat->packetinfo.payloadcollected;

    /* Reduce read size to remaining buffer if smaller */
    if ((collected + readsize) > plbuffersize)
    {
      readsize = plbuffersize - collected;
    }

    bytesread = sl_recvdata (slconn, plbuffer + collected, readsize, slconn->sladdr);

    if (bytesread < 0) /* recv() failed */
    {
      return -1;
    }
    else if (bytesread > 0) /* Process collected payload */
    {
      slconn->stat->packetinfo.payloadcollected += bytesread;
      collected += bytesread;
      bytesavailable -= bytesread;

      /* Update stream tracking on initial read of packets */
      if (slconn->stat->packetinfo.payloadcollected == collected &&
          slconn->stat->packetinfo.payloadcollected >= SL_MIN_BUFFER)
      {
        if (update_stream (slconn, slconn->stat->payload) == -1)
        {
          sl_log_r (slconn, 2, 0, "[%s] %s(): cannot update stream tracking\n",
                    slconn->sladdr, __func__);
          return -1;
        }
      }

      /* Keepalive INFO responses, not returned to caller */
      if (slconn->stat->packetinfo.payloadformat == SLPAYLOAD_JSON &&
          slconn->stat->packetinfo.payloadsubformat == SLPAYLOAD_JSON_INFO &&
          slconn->stat->query_state == KeepAliveQuery)
      {
        collected = 0;
      }

      /* Payload is complete */
      if (slconn->stat->packetinfo.payloadcollected == slconn->stat->packetinfo.payloadlength)
      {
        if (slconn->stat->packetinfo.payloadformat == SLPAYLOAD_JSON &&
            slconn->stat->packetinfo.payloadsubformat == SLPAYLOAD_JSON_INFO)
        {
          if (slconn->stat->query_state == KeepAliveQuery)
          {
            sl_log_r (slconn, 1, 2, "[%s] Keepalive message received\n", slconn->sladdr);
          }

          slconn->stat->query_state = NoQuery;
        }
      }

      /* Return received data to caller */
      break;
    }
  }

  return collected;
} /* End of receive_v4payload() */

/***************************************************************************
 * update_stream:
 *
 * Update the appropriate stream chain entries.  Length of the payload
 * must be at least enough to determine stream details.
 *
 * The slconn->stat->packetinfo.netstaid value is also populated from
 * the payload if not already set.
 *
 * Returns 0 if successfully updated and -1 if not found or error.
 ***************************************************************************/
static int
update_stream (SLCD *slconn, const char *payload)
{
  SLstream *curstream;
  int swapflag = 0;
  int updates  = 0;

  uint16_t year;
  uint16_t yday;
  uint16_t fsec;
  uint32_t nsec;
  uint8_t hour;
  uint8_t min;
  uint8_t sec;
  int month = 0;
  int mday  = 0;

  char timestamp[64] = {0};
  char *cp;
  int count;

  /* Do no updates for info and error packets */
  if (slconn->stat->packetinfo.payloadformat == SLPAYLOAD_MSEED2INFO ||
      slconn->stat->packetinfo.payloadformat == SLPAYLOAD_MSEED2INFOTERM ||
      (slconn->stat->packetinfo.payloadformat == SLPAYLOAD_JSON &&
       (slconn->stat->packetinfo.payloadsubformat == SLPAYLOAD_JSON_INFO ||
        slconn->stat->packetinfo.payloadsubformat == SLPAYLOAD_JSON_ERROR)))
  {
    return 0;
  }

  /* miniSEED 2 */
  if (slconn->stat->packetinfo.payloadformat == SLPAYLOAD_MSEED2)
  {
    /* Copy time fields from fixed header */
    memcpy (&year, payload + 20, sizeof (uint16_t));
    memcpy (&yday, payload + 22, sizeof (uint16_t));
    hour = payload[24];
    min = payload[25];
    sec = payload[26];
    memcpy (&fsec, payload + 28, sizeof (uint16_t));

    /* Determine if byte swapping is needed by testing for bogus year/day values */
    if (year < 1900 || year > 2100 || yday < 1 || yday > 366)
      swapflag = 1;

    if (swapflag)
    {
      sl_gswap2a (&year);
      sl_gswap2a (&yday);
      sl_gswap2a (&fsec);
    }

    sl_doy2md (year, yday, &month, &mday);

    snprintf (timestamp, sizeof(timestamp),
              "%04d-%02d-%02dT%02d:%02d:%02d.%04dZ",
              year, month, mday, hour, min, sec, fsec);

    /* Generate NET_STA string if not already set */
    if (slconn->stat->packetinfo.netstaidlength == 0)
    {
      count = sl_strncpclean (slconn->stat->packetinfo.netstaid,
                              payload + 18, 2);
      slconn->stat->packetinfo.netstaid[count++] = '_';
      count += sl_strncpclean (slconn->stat->packetinfo.netstaid + count,
                               payload + 8, 5);
      slconn->stat->packetinfo.netstaidlength = count;
    }
  }
  /* miniSEED 3 */
  else if (slconn->stat->packetinfo.payloadformat == SLPAYLOAD_MSEED3)
  {
    /* Copy time fields from fixed header */
    memcpy (&year, payload + 8, sizeof (uint16_t));
    memcpy (&yday, payload + 10, sizeof (uint16_t));
    hour = payload[12];
    min = payload[13];
    sec = payload[14];
    memcpy (&nsec, payload + 4, sizeof (uint32_t));

    /* Determine if byte swapping is needed by testing for host endianness */
    if (!sl_littleendianhost())
      swapflag = 1;

    if (swapflag)
    {
      sl_gswap2a (&year);
      sl_gswap2a (&yday);
      sl_gswap4a (&nsec);
    }

    sl_doy2md (year, yday, &month, &mday);

    snprintf (timestamp, sizeof(timestamp),
              "%04d-%02d-%02dT%02d:%02d:%02d.%09dZ",
              year, month, mday, hour, min, sec, nsec);

    /* Extract NET_STA string from FDSN Source Identifier */
    if (slconn->stat->packetinfo.netstaidlength == 0 &&
        payload[33] > 10 &&
        !memcmp (payload + 40, "FDSN:", 5))
    {
      /* Copy from ':' to 2nd '_' */
      if ((cp = strchr (payload + 45, '_')))
      {
        if ((cp = strchr (cp + 1, '_')))
        {
          slconn->stat->packetinfo.netstaidlength = cp - payload + 40;

          if (slconn->stat->packetinfo.netstaidlength <
              sizeof (slconn->stat->packetinfo.netstaid))
          {
            memcpy (slconn->stat->packetinfo.netstaid,
                    payload + 40,
                    slconn->stat->packetinfo.netstaidlength);
          }
        }
      }
    }
  }

  curstream = slconn->streams;

  /* For uni-station mode */
  if (curstream != NULL &&
      strcmp (curstream->netstaid, UNINETSTAID) == 0)
  {
    curstream->seqnum = slconn->stat->packetinfo.seqnum;
    strcpy (curstream->timestamp, timestamp);

    return 0;
  }

  /* For multi-station mode, search the stream chain and update all matching entries */
  while (curstream != NULL)
  {
    /* Use glob matching to match wildcarded station ID codes */
    if (sl_globmatch (slconn->stat->packetinfo.netstaid, curstream->netstaid))
    {
      curstream->seqnum = slconn->stat->packetinfo.seqnum;
      strcpy (curstream->timestamp, timestamp);

      updates++;
    }

    curstream = curstream->next;
  }

  /* If no updates then no match was found */
  if (updates == 0)
    sl_log_r (slconn, 2, 0, "[%s] unexpected data received: %s\n",
              slconn->sladdr, slconn->stat->packetinfo.netstaid);

  return (updates == 0) ? -1 : 0;
} /* End of update_stream() */

/***********************************************************************/ /**
 * @brief Initialize a new ::SLCD
 *
 * Allocate a new ::SLCD and initialize values to default startup
 * values.
 *
 * The \a clientname must be specified and should be a string
 * describing the name of the client program. The \a clientversion is
 * optional and should be the version of the client program.  These
 * values are passed directly to sl_setclientname().
 *
 * @returns An initialized ::SLCD on success, NULL on error.
 ***************************************************************************/
SLCD *
sl_newslcd (const char *clientname, const char *clientversion)
{
  SLCD *slconn;

  slconn = (SLCD *)malloc (sizeof (SLCD));

  if (slconn == NULL)
  {
    sl_log_r (NULL, 2, 0, "%s(): error allocating memory\n", __func__);
    return NULL;
  }

  /* Set defaults */
  slconn->streams    = NULL;
  slconn->sladdr     = NULL;
  slconn->begin_time = NULL;
  slconn->end_time   = NULL;

  slconn->noblock      = 0;
  slconn->dialup       = 0;
  slconn->batchmode    = 0;

  slconn->lastpkttime  = 1;
  slconn->terminate    = 0;
  slconn->resume       = 1;
  slconn->multistation = 0;

  slconn->keepalive = 0;
  slconn->iotimeout = 60;
  slconn->netto     = 600;
  slconn->netdly    = 30;

  slconn->link          = -1;
  slconn->capabilities  = NULL;
  slconn->caparray      = NULL;
  slconn->info          = NULL;
  slconn->clientname    = NULL;
  slconn->clientversion = NULL;
  slconn->proto_major   = 0;
  slconn->proto_minor   = 0;
  slconn->server_major  = 0;
  slconn->server_minor  = 0;

  /* Allocate the associated persistent state struct */
  slconn->stat = (SLstat *)malloc (sizeof (SLstat));

  if (slconn->stat == NULL)
  {
    sl_log_r (NULL, 2, 0, "%s(): error allocating memory\n", __func__);
    free (slconn);
    return NULL;
  }

  slconn->stat->packetinfo.header[0] = '\0';
  slconn->stat->packetinfo.seqnum = SL_UNSETSEQUENCE;
  slconn->stat->packetinfo.payloadlength = 0;
  slconn->stat->packetinfo.payloadcollected = 0;
  slconn->stat->packetinfo.payloadformat = SLPAYLOAD_UNKNOWN;

  slconn->stat->netto_time     = 0;
  slconn->stat->netdly_time    = 0;
  slconn->stat->keepalive_time = 0;

  slconn->stat->payload      = NULL;

  slconn->stat->conn_state   = DOWN;
  slconn->stat->stream_state = HEADER;
  slconn->stat->query_state  = NoQuery;

  slconn->log = NULL;

  if (clientname && sl_setclientname(slconn, clientname, clientversion))
  {
    sl_freeslcd (slconn);
    return NULL;
  }

  return slconn;
} /* End of sl_newslconn() */


/***************************************************************************
 * sl_freeslcd:
 *
 * Free all memory associated with a SLCD struct including the
 * associated stream chain and persistent connection state.
 *
 ***************************************************************************/
void
sl_freeslcd (SLCD *slconn)
{
  SLstream *curstream;
  SLstream *nextstream;

  curstream = slconn->streams;

  /* Traverse the stream chain and free memory */
  while (curstream != NULL)
  {
    nextstream = curstream->next;

    if (curstream->selectors != NULL)
      free (curstream->selectors);
    free (curstream);

    curstream = nextstream;
  }

  if (slconn->sladdr != NULL)
    free (slconn->sladdr);

  if (slconn->begin_time != NULL)
    free (slconn->begin_time);

  if (slconn->end_time != NULL)
    free (slconn->end_time);

  if (slconn->capabilities != NULL)
    free (slconn->capabilities);

  if (slconn->caparray != NULL)
    free (slconn->caparray);

  if (slconn->clientname != NULL)
    free (slconn->clientname);

  if (slconn->clientversion != NULL)
    free (slconn->clientversion);

  if (slconn->stat != NULL)
    free (slconn->stat);

  if (slconn->log != NULL)
    free (slconn->log);

  free (slconn);
} /* End of sl_freeslcd() */

/***********************************************************************/ /**
 * @brief Set client name and version reported to server (v4 only)
 *
 * Set the program name and, optionally, version that will be send to
 * the server in protocol v4 version.  These values will be combined
 * into a value with the pattern:
 *   NAME[/VERSION]
 *
 * @retval  0 : success
 * @retval -1 : error
 ***************************************************************************/
int
sl_setclientname (SLCD *slconn, const char *name, const char *version)
{
  if (!slconn || !name)
    return -1;

  if (slconn->clientname)
    free (slconn->clientname);

  if (slconn->clientversion)
    free (slconn->clientversion);

  slconn->clientname = strdup (name);

  if (slconn->clientname == NULL)
  {
    sl_log_r (NULL, 2, 0, "%s(): error allocating memory\n", __func__);
    return -1;
  }

  if (version)
  {
    slconn->clientversion = strdup (version);

    if (slconn->clientversion == NULL)
    {
      sl_log_r (NULL, 2, 0, "%s(): error allocating memory\n", __func__);
      return -1;
    }
  }

  return 0;
} /* End of sl_setclientname() */

/***************************************************************************
 * sl_addstream:
 *
 * Add a new stream entry to the stream chain for the given SLCD
 * struct.  No checking is done for duplicate streams.
 *
 *  - selectors should be NULL if there are none to use
 *  - seqnum should be SL_UNSETSEQUENCE to start at the next data
 *  - timestamp should be NULL if it should not be used
 *
 * Returns 0 if successfully added or -1 on error.
 ***************************************************************************/
int
sl_addstream (SLCD *slconn, const char *netstaid,
              const char *selectors, uint64_t seqnum,
              const char *timestamp)
{
  SLstream *curstream;
  SLstream *newstream;
  SLstream *laststream = NULL;

  curstream = slconn->streams;

  /* Sanity, check for a uni-station mode entry */
  if (curstream)
  {
    if (strcmp (curstream->netstaid, UNINETSTAID) == 0)
    {
      sl_log_r (slconn, 2, 0, "[%s] %s(): uni-station mode already configured!\n",
                slconn->sladdr, __func__);
      return -1;
    }
  }

  /* Search the stream chain */
  while (curstream != NULL)
  {
    laststream = curstream;
    curstream  = curstream->next;
  }

  newstream = (SLstream *)malloc (sizeof (SLstream));

  if (newstream == NULL)
  {
    sl_log_r (slconn, 2, 0, "%s(): error allocating memory\n", __func__);
    return -1;
  }

  strncpy (newstream->netstaid, netstaid, sizeof (newstream->netstaid) - 1);

  if (selectors == 0 || selectors == NULL)
    newstream->selectors = 0;
  else
    newstream->selectors = strdup (selectors);

  newstream->seqnum = seqnum;

  if (timestamp == 0 || timestamp == NULL)
    newstream->timestamp[0] = '\0';
  else
    strncpy (newstream->timestamp, timestamp, sizeof(newstream->timestamp) - 1);

  /* Convert old comma-delimited date-time to ISO-compatible format if needed
   * Example: '2021,11,19,17,23,18' => '2021-11-18T17:23:18.0Z' */
  if (newstream->timestamp[0])
  {
    if (sl_isodatetime(newstream->timestamp, newstream->timestamp) == NULL)
    {
      sl_log_r (slconn, 2, 0, "%s(): could not parse timestamp for %s entry: '%s'\n",
                __func__, netstaid, newstream->timestamp);
      return -1;
    }
  }

  newstream->next = NULL;

  if (slconn->streams == NULL)
  {
    slconn->streams = newstream;
  }
  else if (laststream)
  {
    laststream->next = newstream;
  }

  slconn->multistation = 1;

  return 0;
} /* End of sl_addstream() */

/***************************************************************************
 * sl_setuniparams:
 *
 * Set the parameters for a uni-station mode connection for the
 * given SLCD struct.  If the stream entry already exists, overwrite
 * the previous settings.
 * Also sets the multistation flag to 0 (false).
 *
 *  - selectors should be 0 if there are none to use
 *  - seqnum should be SL_UNSETSEQUENCE to start at the next data
 *  - timestamp should be 0 if it should not be used
 *
 * Returns 0 if successfully added or -1 on error.
 ***************************************************************************/
int
sl_setuniparams (SLCD *slconn, const char *selectors,
                 uint64_t seqnum, const char *timestamp)
{
  SLstream *newstream;

  newstream = slconn->streams;

  if (newstream == NULL)
  {
    newstream = (SLstream *)malloc (sizeof (SLstream));

    if (newstream == NULL)
    {
      sl_log_r (slconn, 2, 0, "%s(): error allocating memory\n", __func__);
      return -1;
    }
  }
  else if (strcmp (newstream->netstaid, UNINETSTAID) != 0)
  {
    sl_log_r (slconn, 2, 0, "[%s] %s(): multi-station mode already configured!\n",
              slconn->sladdr, __func__);
    return -1;
  }

  strncpy (newstream->netstaid, UNINETSTAID, sizeof (newstream->netstaid));

  if (selectors == 0 || selectors == NULL)
    newstream->selectors = 0;
  else
    newstream->selectors = strdup (selectors);

  newstream->seqnum = seqnum;

  if (timestamp == 0 || timestamp == NULL)
    newstream->timestamp[0] = '\0';
  else
    strncpy (newstream->timestamp, timestamp, sizeof(newstream->timestamp) - 1);

  newstream->next = NULL;

  slconn->streams = newstream;

  slconn->multistation = 0;

  return 0;
} /* End of sl_setuniparams() */

/***************************************************************************
 * sl_request_info:
 *
 * Add an INFO request to the SeedLink Connection Description.
 *
 * Returns 0 if successful and -1 if error.
 ***************************************************************************/
int
sl_request_info (SLCD *slconn, const char *infostr)
{
  if (slconn->info != NULL)
  {
    sl_log_r (slconn, 2, 0, "[%s] Cannot request INFO '%.20s', another is pending\n",
              slconn->sladdr, infostr);
    return -1;
  }
  else
  {
    slconn->info = infostr;
    return 0;
  }
} /* End of sl_request_info() */

/***************************************************************************
 * @brief Check if server capabilities include specified value
 *
 * The server capabilities returned during connection negotiation are
 * searched for matches to the specified \a capability.
 *
 * @param[in] slconn     SeedLink connection description
 * @param[in] capability Capabilty string to search for
 *
 * @retval 0 Capability is not supported or unknown
 * @retval >0 Capability is supported
 ***************************************************************************/
int
sl_hascapability (SLCD *slconn, char *capability)
{
  int length;
  int start;
  int idx;

  if (!slconn || !capability)
    return 0;

  if (!slconn->capabilities)
    return 0;

  length = strlen (slconn->capabilities);
  /* Create capabilities array if needed */
  if (slconn->caparray == NULL)
  {
    /* Copy and replace spaces with terminating NULLs */
    slconn->caparray = strdup(slconn->capabilities);

    for (idx = 0; idx < length; idx++)
    {
      if (slconn->caparray[idx] == ' ')
        slconn->caparray[idx] = '\0';
    }
  }

  /* Search capabilities array for a matching entry */
  for (idx = 0, start = -1; idx < length; idx++)
  {
    /* Determine if at the start of a capability flag:
       either initial state or following a terminating NULL */
    if (slconn->caparray[idx] == '\0')
      start = -1;
    else if (start == -1)
      start = 1;
    else
      start = 0;

    if (start == 1 && strcmp (slconn->caparray + idx, capability) == 0)
      return 1;
  }

  return 0;
} /* End of sl_hascapablity() */

/***************************************************************************
 * sl_terminate:
 *
 * Set the terminate flag in the SLCD.
 ***************************************************************************/
void
sl_terminate (SLCD *slconn)
{
  sl_log_r (slconn, 1, 1, "[%s] Terminating connection\n", slconn->sladdr);

  slconn->terminate = 1;
} /* End of sl_terminate() */

/***************************************************************/ /**
 * @brief Detect miniSEED record in buffer
 *
 * Determine if the buffer contains a miniSEED data record by
 * verifying known signatures (fields with known limited values).
 *
 * If miniSEED 2.x is detected, search the record up to recbuflen
 * bytes for a 1000 blockette. If no blockette 1000 is found, search
 * at 64-byte offsets for the fixed section of the next header,
 * thereby implying the record length.
 *
 * @param[in] buffer Buffer to test for known data types
 * @param[in] buflen Length of buffer
 * @param[out] payloadformat Payload type detected
 *
 * @retval -1 Data record not detected or error
 * @retval 0 Data record detected but could not determine length
 * @retval >0 Size of the record in bytes
 *********************************************************************/
static int64_t
detect (const char *buffer, uint64_t buflen, char *payloadformat)
{
  uint8_t swapflag = 0; /* Byte swapping flag */
  int64_t reclen = -1;  /* Size of record in bytes */

  uint16_t blkt_offset; /* Byte offset for next blockette */
  uint16_t blkt_type;
  uint16_t next_blkt;
  const char *nextfsdh;

  if (!buffer || !payloadformat)
    return -1;

  /* Buffer must be at least SL_MIN_BUFFER */
  if (buflen < SL_MIN_BUFFER)
    return -1;

  /* Check for valid header, set format version */
  *payloadformat = SLPAYLOAD_UNKNOWN;
  if (MS3_ISVALIDHEADER (buffer))
  {
    *payloadformat = SLPAYLOAD_MSEED3;

    //TODO swap for operation on big endian sid:8, extra:16, payload:32
    reclen = MS3FSDH_LENGTH                   /* Length of fixed portion of header */
             + *pMS3FSDH_SIDLENGTH (buffer)   /* Length of source identifier */
             + *pMS3FSDH_EXTRALENGTH (buffer) /* Length of extra headers */
             + *pMS3FSDH_DATALENGTH (buffer); /* Length of data payload */
  }
  else if (MS2_ISVALIDHEADER (buffer))
  {
    *payloadformat = SLPAYLOAD_MSEED2;
    reclen = 0;

    /* Check to see if byte swapping is needed by checking for sane year and day */
    if (!MS_ISVALIDYEARDAY (*pMS2FSDH_YEAR(buffer), *pMS2FSDH_DAY(buffer)))
      swapflag = 1;

    blkt_offset = HO2u(*pMS2FSDH_BLOCKETTEOFFSET (buffer), swapflag);

    /* Loop through blockettes as long as number is non-zero and viable */
    while (blkt_offset != 0 &&
           blkt_offset > 47 &&
           blkt_offset <= buflen)
    {
      memcpy (&blkt_type, buffer + blkt_offset, 2);
      memcpy (&next_blkt, buffer + blkt_offset + 2, 2);

      if (swapflag)
      {
        sl_gswap2 (&blkt_type);
        sl_gswap2 (&next_blkt);
      }

      /* Found a 1000 blockette, not truncated */
      if (blkt_type == 1000 &&
          (int)(blkt_offset + 8) <= buflen)
      {
        /* Field 3 of B1000 is a uint8_t value describing the buffer
         * length as 2^(value).  Calculate 2-raised with a shift. */
        reclen = (unsigned int)1 << *pMS2B1000_RECLEN(buffer+blkt_offset);

        break;
      }

      /* Safety check for invalid offset */
      if (next_blkt != 0 && (next_blkt < 4 || (next_blkt - 4) <= blkt_offset))
      {
        sl_log (2, 0, "Invalid miniSEED2 blockette offset (%d) less than or equal to current offset (%d)\n",
                next_blkt, blkt_offset);
        return -1;
      }

      blkt_offset = next_blkt;
    }

    /* If record length was not determined by a 1000 blockette scan the buffer
     * and search for the next record header. */
    if (reclen == -1)
    {
      nextfsdh = buffer + 64;

      /* Check for record header or blank/noise record at 64-byte offsets */
      while (((nextfsdh - buffer) + 48) < buflen)
      {
        if (MS2_ISVALIDHEADER (nextfsdh))
        {
          reclen = nextfsdh - buffer;

          break;
        }

        nextfsdh += 64;
      }
    }
  } /* End of miniSEED 2.x detection */

  return reclen;
} /* End of detect() */

/***************************************************************************
 * Determine the number of bytes available for reading on the
 * specified socket.
 *
 * Returns number of bytes available for recv() on socket on success,
 * -1 on error.
 ***************************************************************************/
static int
recvable (SOCKET sock)
{
#if defined(SLP_WIN)
  u_long available = 0;

  if (ioctlsocket(sock, FIONREAD, &available))
    return -1;

  return available;
#else
  int available = -1;

  if (ioctl(sock, FIONREAD, &available) < 0)
    return -1;

  return available;
#endif
}
