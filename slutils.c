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
 * Copyright (C) 2022:
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
static int receive_header (SLCD *slconn, uint8_t *buffer, uint32_t bytesavailable);
static int64_t receive_payload (SLCD *slconn, char *plbuffer, uint32_t plbuffersize,
                                uint8_t *buffer, int bytesavailable);
static int update_stream (SLCD *slconn, const char *payload);
static int64_t detect (const char *record, uint64_t recbuflen, char *payloadformat);


/**********************************************************************/ /**
 * @brief Managage a connection to a SeedLink server and collect packets
 *
 * Designed to run in a loop of a client program, this function manages
 * the connection to the server and returns received packets.  This
 * routine will send keepalives if configured for the connection and
 * can operate in blocking or non-blocking mode.
 *
 * The returned \a packetinfo contains the details including: sequence
 * number, payload length, payload type, and how much of the payload
 * has been returned so far.
 *
 * If the \a slconn.noblock flags is set, the function will return
 * quickly even if no data are available.  If the flag is not set,
 * the function will block and only return if data are available.
 *
 * If \a SLTOOLARGE is returned, the \a plbuffer is not large enough to
 * hold the payload.  The payload length is available at
 * \a packetinfo.payloadlength and the caller may choose to reallocate
 * the buffer to accommodate the payload.  Note that buffer may contain
 * partial payload data and should be preserved if reallocated,
 * specifically the first \a packetinfo.payloadcollected bytes.
 *
 * @param[in]  slconn   SeedLink connection description
 * @param[out] packetinfo  Pointer to pointer to ::SLpacketinfo describing payload
 * @param[out] plbuffer  Destination buffer for packet payload
 * @param[in]  plbuffersize  Length of destination buffer
 *
 * @returns An @ref collect-status code
 * @retval SLPACKET Complete packet returned
 * @retval SLTERMINATE Connection termination or error
 * @retval SLNOPACKET  No packet available, call again
 * @retval SLTOOLARGE  Payload is larger than allowed maximum
 ***************************************************************************/
int
sl_collect (SLCD *slconn, const SLpacketinfo **packetinfo,
            char *plbuffer, uint32_t plbuffersize)
{
  int64_t bytesread;
  int64_t current_time;
  uint32_t bytesconsumed;
  uint32_t bytesavailable;
  int poll_state;

  if (!slconn || !packetinfo || (plbuffersize > 0 && !plbuffer))
    return SLTERMINATE;

  while (slconn->terminate < 2)
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
      /* Receive data into internal buffer */
      if (slconn->terminate == 0)
      {
        bytesread = sl_recvdata (slconn,
                                 slconn->recvbuffer + slconn->recvdatalen,
                                 sizeof (slconn->recvbuffer) - slconn->recvdatalen,
                                 slconn->sladdr);

        if (bytesread < 0)
        {
          break;
        }
        else if (bytesread > 0)
        {
          slconn->recvdatalen += bytesread;
        }
        else if (slconn->recvdatalen == 0) /* bytesread == 0 */
        {
          /* Wait up to 1/2 second when blocking, otherwise 1 millisecond */
          poll_state = sl_poll (slconn, 1, 0, (slconn->noblock) ? 1 : 500);

          if (poll_state < 0 && slconn->terminate == 0)
          {
            sl_log_r (slconn, 2, 0, "[%s] %s(): polling error: %s\n",
                      slconn->sladdr, __func__, sl_strerror ());
            break;
          }
        }
      }

      /* Process data in internal buffer */
      bytesconsumed = 0;

      /* Check for special cases of the server reporting end of streaming or errors
       * while awaiting a header (i.e. in between packets) */
      if (slconn->stat->stream_state == HEADER)
      {
        if (slconn->recvdatalen - bytesconsumed >= 3 &&
            memcmp (slconn->recvbuffer + bytesconsumed, "END", 3) == 0)
        {
          sl_log_r (slconn, 1, 1, "[%s] End of selected time window or stream (FETCH/dial-up mode)\n",
                    slconn->sladdr);

          bytesconsumed += 3;
          break;
        }

        if (slconn->recvdatalen - bytesconsumed >= 5 &&
            memcmp (slconn->recvbuffer + bytesconsumed, "ERROR", 5) == 0)
        {
          sl_log_r (slconn, 2, 0, "[%s] Server reported an error with the last command\n",
                    slconn->sladdr);

          bytesconsumed += 5;
          break;
        }
      }

      /* Read next header */
      if (slconn->stat->stream_state == HEADER)
      {
        bytesavailable = slconn->recvdatalen - bytesconsumed;

        if ((slconn->protocol & SLPROTO3X && bytesavailable >= SLHEADSIZE_V3) ||
            (slconn->protocol & SLPROTO40 && bytesavailable >= SLHEADSIZE_V4))
        {
          bytesread = receive_header (slconn,
                                      slconn->recvbuffer + bytesconsumed,
                                      bytesavailable);

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
              slconn->stat->stream_state           = NETSTAID;
            }
            else
            {
              slconn->stat->packetinfo.payloadcollected = 0;
              slconn->stat->stream_state                = PAYLOAD;
            }

            bytesconsumed += bytesread;
          }
        }
      } /* Done reading header */

      /* Read network-station ID */
      if (slconn->stat->stream_state == NETSTAID &&
          slconn->stat->packetinfo.netstaidlength > 0 &&
          (slconn->recvdatalen - bytesconsumed) >= slconn->stat->packetinfo.netstaidlength)
      {
        if (slconn->stat->packetinfo.netstaidlength > (sizeof (slconn->stat->packetinfo.netstaid) - 1))
        {
          sl_log_r (slconn, 2, 0,
                    "[%s] %s() received NET_STA ID is too large (%u) for buffer (%zu)\n",
                    slconn->sladdr, __func__,
                    slconn->stat->packetinfo.netstaidlength,
                    sizeof (slconn->stat->packetinfo.netstaid) - 1);

          break;
        }
        else
        {
          memcpy (slconn->stat->packetinfo.netstaid,
                  slconn->recvbuffer + bytesconsumed,
                  slconn->stat->packetinfo.netstaidlength);

          slconn->stat->packetinfo.netstaid[slconn->stat->packetinfo.netstaidlength] = '\0';

          /* Set state for payload collection */
          slconn->stat->packetinfo.payloadcollected = 0;
          slconn->stat->stream_state                = PAYLOAD;

          bytesconsumed += slconn->stat->packetinfo.netstaidlength;
        }
      } /* Done reading network-station ID */

      /* Read payload */
      if (slconn->stat->stream_state == PAYLOAD)
      {
        bytesavailable = slconn->recvdatalen - bytesconsumed;

        /* If payload length is known, return SLTOOLARGE if buffer is not sufficient */
        if (slconn->stat->packetinfo.payloadlength > 0 &&
            slconn->stat->packetinfo.payloadlength > plbuffersize)
        {
          /* Shift any remaining data in the buffer to the start */
          if (bytesconsumed > 0 && bytesconsumed < slconn->recvdatalen)
          {
            memmove (slconn->recvbuffer,
                     slconn->recvbuffer + bytesconsumed,
                     slconn->recvdatalen - bytesconsumed);
          }

          slconn->recvdatalen -= bytesconsumed;
          bytesconsumed = 0;

          *packetinfo = &slconn->stat->packetinfo;
          return SLTOOLARGE;
        }

        bytesread = receive_payload (slconn, plbuffer, plbuffersize,
                                     slconn->recvbuffer + bytesconsumed,
                                     bytesavailable);

        if (bytesread < 0)
        {
          break;
        }
        if (bytesread > 0)
        {
          slconn->stat->netto_time     = 0;
          slconn->stat->keepalive_time = 0;

          bytesconsumed += bytesread;
        }

        /* Payload is complete */
        if (slconn->stat->packetinfo.payloadlength > 0 &&
            slconn->stat->packetinfo.payloadcollected == slconn->stat->packetinfo.payloadlength)
        {
          /* Shift any remaining data in the buffer to the start */
          if (bytesconsumed > 0 && bytesconsumed < slconn->recvdatalen)
          {
            memmove (slconn->recvbuffer,
                     slconn->recvbuffer + bytesconsumed,
                     slconn->recvdatalen - bytesconsumed);
          }

          slconn->recvdatalen -= bytesconsumed;
          bytesconsumed = 0;

          /* Set state for header collection if payload is complete */
          slconn->stat->stream_state = HEADER;

          /* V3 Keepalive INFO responses are not returned to the caller */
          if (slconn->stat->query_state == KeepAliveQuery &&
              (slconn->stat->packetinfo.payloadformat == SLPAYLOAD_MSEED2INFOTERM ||
               slconn->stat->packetinfo.payloadformat == SLPAYLOAD_MSEED2INFO))
          {
            if (slconn->stat->packetinfo.payloadformat == SLPAYLOAD_MSEED2INFOTERM)
            {
              sl_log_r (slconn, 1, 2, "[%s] Keepalive message received\n", slconn->sladdr);

              slconn->stat->query_state = NoQuery;
            }
          }
          /* V4 Keepalive INFO responses are not returned to caller */
          else if (slconn->stat->query_state == KeepAliveQuery &&
                   slconn->stat->packetinfo.payloadformat == SLPAYLOAD_JSON &&
                   slconn->stat->packetinfo.payloadsubformat == SLPAYLOAD_JSON_INFO)
          {
            sl_log_r (slconn, 1, 2, "[%s] Keepalive message received\n", slconn->sladdr);

            slconn->stat->query_state = NoQuery;
          }
          /* All other payloads are returned to the caller */
          else
          {
            *packetinfo = &slconn->stat->packetinfo;
            return SLPACKET;
          }
        }
      } /* Done reading payload */

      /* If a viable amount of data exists but has not been consumed something is wrong with the stream */
      if (slconn->recvdatalen > SL_MIN_PAYLOAD && bytesconsumed == 0)
      {
        sl_log_r (slconn, 2, 0, "[%s] %s(): cannot process received data, terminating.\n",
                  slconn->sladdr, __func__);
        sl_log_r (slconn, 2, 0, "[%s]  recvdatalen: %u, stream_state: %d, bytesconsumed: %u\n",
                  slconn->sladdr, slconn->recvdatalen, slconn->stat->stream_state, bytesconsumed);
        break;
      }

      /* Shift any remaining data in the buffer to the start */
      if (bytesconsumed > 0 && bytesconsumed < slconn->recvdatalen)
      {
        memmove (slconn->recvbuffer,
                 slconn->recvbuffer + bytesconsumed,
                 slconn->recvdatalen - bytesconsumed);
      }

      slconn->recvdatalen -= bytesconsumed;
      bytesconsumed = 0;

      /* Set termination flag to level 2 if less than viable number of bytes in buffer */
      if (slconn->terminate == 1 && slconn->recvdatalen < SL_MIN_PAYLOAD)
      {
        slconn->terminate = 2;
      }
    } /* Done reading data in STREAMING state */

    /* Update timing variables */
    current_time = sl_nstime ();

    /* Check for network idle timeout */
    if (slconn->stat->conn_state == STREAMING &&
        slconn->netto && slconn->stat->netto_time &&
        slconn->stat->netto_time < current_time)
    {
      sl_log_r (slconn, 1, 0, "[%s] network timeout (%ds), reconnecting in %ds\n",
                slconn->sladdr, slconn->netto, slconn->netdly);
      sl_disconnect (slconn);
      slconn->link              = -1;
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

    /* Return if not waiting for data and no data in internal buffer */
    if (slconn->noblock && slconn->recvdatalen == 0)
    {
      *packetinfo = NULL;
      return SLNOPACKET;
    }

    /* Termination in any connection state but UP is immediate */
    if (slconn->terminate && slconn->stat->conn_state != UP)
    {
      break;
    }
  } /* End of primary loop */

  /* Terminating */
  sl_disconnect (slconn);
  slconn->link = -1;

  *packetinfo = NULL;
  return SLTERMINATE;
} /* End of sl_collect() */

/***************************************************************************
 * receive_header:
 *
 * Receive packet header.
 *
 * Returns:
 * bytes : Size of header read
 * -1 :  on error
 ***************************************************************************/
static int
receive_header (SLCD *slconn, uint8_t *buffer, uint32_t bytesavailable)
{
  uint32_t bytesread = 0;
  char sequence[7] = {0};
  char *tail = NULL;

  if (!slconn)
    return -1;

  /* Zero the destination packet info structure */
  memset (&slconn->stat->packetinfo, 0, sizeof (SLpacketinfo));

  if (slconn->protocol & SLPROTO3X && bytesavailable >= SLHEADSIZE_V3)
  {
    /* Parse v3 INFO header */
    if (memcmp (buffer, INFOSIGNATURE, 6) == 0)
    {
      slconn->stat->packetinfo.seqnum        = SL_UNSETSEQUENCE;
      slconn->stat->packetinfo.payloadlength = 0;
      slconn->stat->packetinfo.payloadformat = (buffer[SLHEADSIZE_V3 - 1] == '*') ? SLPAYLOAD_MSEED2INFO : SLPAYLOAD_MSEED2INFOTERM;
    }
    /* Parse v3 data header */
    else if (memcmp (buffer, SIGNATURE_V3, 2) == 0)
    {
      memcpy (sequence, buffer + 2, 6);
      slconn->stat->packetinfo.seqnum = strtoul (sequence, &tail, 16);

      if (*tail)
      {
        sl_log_r (slconn, 2, 0, "[%s] %s() cannot parse sequence number from v3 header: %8.8s\n",
                  slconn->sladdr, __func__, buffer + 2);
        return -1;
      }

      slconn->stat->packetinfo.payloadlength = 0;
      slconn->stat->packetinfo.payloadformat = SLPAYLOAD_UNKNOWN;
    }
    else
    {
      sl_log_r (slconn, 2, 0, "[%s] %s(): unexpected V3 header signature found: %2.2s)\n",
                slconn->sladdr, __func__, buffer);
      return -1;
    }

    bytesread = SLHEADSIZE_V3;
  }
  else if (slconn->protocol & SLPROTO40 && bytesavailable >= SLHEADSIZE_V4)
  {
    /* Parse v4 header */
    if (memcmp (buffer, SIGNATURE_V4, 2) == 0)
    {
      slconn->stat->packetinfo.payloadformat    = buffer[2];
      slconn->stat->packetinfo.payloadsubformat = buffer[3];
      memcpy (&slconn->stat->packetinfo.payloadlength, buffer + 4, 4);
      memcpy (&slconn->stat->packetinfo.seqnum, buffer + 8, 8);
      memcpy (&slconn->stat->packetinfo.netstaidlength, buffer + 16, 1);

      if (!sl_littleendianhost ())
      {
        sl_gswap8 (&slconn->stat->packetinfo.seqnum);
        sl_gswap4 (&slconn->stat->packetinfo.payloadlength);
      }
    }
    else
    {
      sl_log_r (slconn, 2, 0, "[%s] %s(): unexpected V4 header signature found: %2.2s)\n",
                slconn->sladdr, __func__, buffer);
      return -1;
    }

    bytesread = SLHEADSIZE_V4;
  }
  else
  {
    sl_log_r (slconn, 2, 0, "[%s] %s(): unexpected header signature found (instead: %2.2s)\n",
              slconn->sladdr, __func__, buffer);
    return -1;
  }

  return bytesread;
} /* End of receive_header() */


/***************************************************************************
 * receive_payload:
 *
 * Copy payload data to supplied buffer.
 *
 * The supplied buffer must be large enough for stream tracking and payload
 * detection, defined as SL_MIN_PAYLOAD bytes.
 *
 * Returns
 * bytes : Number of bytes consumed on success
 * -1 :  on error
 ***************************************************************************/
int64_t
receive_payload (SLCD *slconn, char *plbuffer, uint32_t plbuffersize,
                 uint8_t *buffer, int bytesavailable)
{
  SLpacketinfo *packetinfo = NULL;
  uint32_t bytestoconsume = 0;
  int64_t detectedlength;
  char payloadformat = SLPAYLOAD_UNKNOWN;

  if (!slconn || !plbuffer)
    return -1;

  packetinfo = &slconn->stat->packetinfo;

  /* Return for more data if the minimum for detection/updates is not available */
  if (bytesavailable < SL_MIN_PAYLOAD)
  {
    return 0;
  }

  /* If payload length is unknown, consume up to 128 bytes */
  if (packetinfo->payloadlength == 0)
  {
    bytestoconsume = (bytesavailable < 128) ? bytesavailable : 128;
  }
  /* If remaining payload is smaller than available, consume remaining */
  else if ((packetinfo->payloadlength - packetinfo->payloadcollected) < bytesavailable)
  {
    bytestoconsume = packetinfo->payloadlength - packetinfo->payloadcollected;
  }
  /* Otherwise, all available data is payload */
  else
  {
    bytestoconsume = bytesavailable;
  }

  if (bytestoconsume > plbuffersize - packetinfo->payloadcollected)
  {
    sl_log_r (slconn, 2, 0, "[%s] %s(): provided buffer size (%u) is insufficient for payload (%u)\n",
              slconn->sladdr, __func__, plbuffersize,
              (packetinfo->payloadlength == 0) ? bytestoconsume : packetinfo->payloadlength);
    return -1;
  }

  /* Copy payload data from internal buffer to payload buffer */
  memcpy (plbuffer + packetinfo->payloadcollected, buffer, bytestoconsume);
  packetinfo->payloadcollected += bytestoconsume;

  /* If payload length is not yet known for V3, try to detect from payload */
  if (slconn->protocol & SLPROTO3X && packetinfo->payloadlength == 0)
  {
    detectedlength = detect (plbuffer, packetinfo->payloadcollected, &payloadformat);

    /* Return error if no recognized payload detected */
    if (detectedlength < 0)
    {
      sl_log_r (slconn, 2, 0,
                "[%s] %s(): non-miniSEED packet received for v3 protocol! Terminating.\n",
                slconn->sladdr, __func__);
      return -1;
    }
    /* Update packet info if length detected */
    else if (detectedlength > 0)
    {
      if (packetinfo->payloadformat == SLPAYLOAD_UNKNOWN)
      {
        packetinfo->payloadformat = payloadformat;
      }

      packetinfo->payloadlength = detectedlength;
    }
  }

  /* Handle payload of known length */
  if (packetinfo->payloadlength > 0)
  {
    /* Update streaming tracking if initial payload */
    if (packetinfo->payloadcollected == bytestoconsume &&
        packetinfo->payloadcollected >= SL_MIN_PAYLOAD)
    {
      if (update_stream (slconn, plbuffer) == -1)
      {
        sl_log_r (slconn, 2, 0, "[%s] %s(): cannot update stream tracking\n",
                  slconn->sladdr, __func__);
        return -1;
      }
    }
  }

  return bytestoconsume;
} /* End of receive_payload() */


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

  /* No updates for info and error packets */
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
      sl_gswap2 (&year);
      sl_gswap2 (&yday);
      sl_gswap2 (&fsec);
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
      sl_gswap2 (&year);
      sl_gswap2 (&yday);
      sl_gswap4 (&nsec);
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

  slconn->capabilities  = NULL;
  slconn->caparray      = NULL;
  slconn->info          = NULL;
  slconn->clientname    = NULL;
  slconn->clientversion = NULL;
  slconn->protocol      = UNSET_PROTO;
  slconn->server_protocols = 0;

  slconn->link        = -1;
  slconn->tls         = 0;
  slconn->tlsctx      = NULL;

  /* Allocate the associated persistent state struct */
  if ((slconn->stat = (SLstat *)malloc (sizeof (SLstat))) == NULL)
  {
    sl_log_r (NULL, 2, 0, "%s(): error allocating memory\n", __func__);
    free (slconn);
    return NULL;
  }

  slconn->stat->packetinfo.seqnum = SL_UNSETSEQUENCE;
  slconn->stat->packetinfo.payloadlength = 0;
  slconn->stat->packetinfo.payloadcollected = 0;
  slconn->stat->packetinfo.payloadformat = SLPAYLOAD_UNKNOWN;

  slconn->stat->netto_time     = 0;
  slconn->stat->netdly_time    = 0;
  slconn->stat->keepalive_time = 0;

  slconn->stat->conn_state   = DOWN;
  slconn->stat->stream_state = HEADER;
  slconn->stat->query_state  = NoQuery;

  slconn->log = NULL;

  slconn->recvdatalen = 0;

  /* Store copies of client name and version */
  if (clientname && sl_setclientname (slconn, clientname, clientversion))
  {
    sl_freeslcd (slconn);
    return NULL;
  }

  return slconn;
} /* End of sl_newslcd() */


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

  free (slconn->sladdr);
  free (slconn->begin_time);
  free (slconn->end_time);
  free (slconn->capabilities);
  free (slconn->caparray);
  free (slconn->clientname);
  free (slconn->clientversion);
  free (slconn->stat);
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

  free (slconn->clientname);
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
 * @param[in] capability Capabilty string to search for (case sensitive)
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

  if (buflen < SL_MIN_PAYLOAD)
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
