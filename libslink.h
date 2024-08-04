/***************************************************************************
 * libslink.h:
 *
 * Interface declarations for the SeedLink library (libslink).
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
 * Copyright (C) 2024:
 * @author Chad Trabant, EarthScope Data Services
 ***************************************************************************/

#ifndef LIBSLINK_H
#define LIBSLINK_H 1

#ifdef __cplusplus
extern "C" {
#endif

#define LIBSLINK_RELEASE "2024.210"    /**< libslink release date */
#define LIBSLINK_VERSION_MAJOR  3      /**< libslink major version */
#define LIBSLINK_VERSION_MINOR  0      /**< libslink minor version */
#define LIBSLINK_VERSION_PATCH  0      /**< libslink patch version */
#define LIBSLINK_STRINGIFY(a)   LIBSLINK_XSTRINGIFY(a)
#define LIBSLINK_XSTRINGIFY(a)  #a
/** @def LIBSLINK_VERSION
    @brief libslink version string */
#define LIBSLINK_VERSION        LIBSLINK_STRINGIFY(LIBSLINK_VERSION_MAJOR) "." \
                                LIBSLINK_STRINGIFY(LIBSLINK_VERSION_MINOR) "." \
                                LIBSLINK_STRINGIFY(LIBSLINK_VERSION_PATCH) "DEV"


/** @defgroup seedlink-connection SeedLink Connection */
/** @defgroup connection-state Connection State */
/** @defgroup logging Central Logging */
/** @defgroup utility-functions General Utility Functions */


/* C99 standard headers */
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>
#include <ctype.h>

#if defined(WIN32) || defined(_WIN32) || defined(WIN64) || defined(_WIN64)
  #define SLP_WIN 1
#endif

/* Set platform specific features, Windows, Solaris, then everything else */
#if defined(SLP_WIN)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #include <process.h>
  #include <io.h>

  #define R_OK 4

  /* For MSVC 2012 and earlier define standard int types, otherwise use inttypes.h */
  #if defined(_MSC_VER) && _MSC_VER <= 1700
    typedef signed char int8_t;
    typedef unsigned char uint8_t;
    typedef signed short int int16_t;
    typedef unsigned short int uint16_t;
    typedef signed int int32_t;
    typedef unsigned int uint32_t;
    typedef signed __int64 int64_t;
    typedef unsigned __int64 uint64_t;
  #else
    #include <inttypes.h>
  #endif

  #if defined(_MSC_VER)
    #if !defined(PRId64)
      #define PRId64 "I64d"
    #endif
    #if !defined(SCNd64)
      #define SCNd64 "I64d"
    #endif

    #define strdup _strdup
    #define read _read
    #define write _write
    #define open _open
    #define close _close
    #define snprintf _snprintf
    #define vsnprintf _vsnprintf
    #define strncasecmp _strnicmp
    #define access _access
  #endif

#elif defined(__sun__) || defined(__sun)
  #include <unistd.h>
  #include <inttypes.h>
  #include <errno.h>
  #include <sys/types.h>
  #include <sys/stat.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netdb.h>
  #include <sys/time.h>
  #include <sys/utsname.h>
  #include <pwd.h>
  #include <sys/ioctl.h>

#else
  #include <unistd.h>
  #include <inttypes.h>
  #include <errno.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netdb.h>
  #include <sys/time.h>
  #include <sys/utsname.h>
  #include <pwd.h>
  #include <sys/ioctl.h>

#endif

/* Use int for SOCKET if not already defined */
#ifndef SOCKET
/** @def SOCKET
    @brief A portable type for a socket descriptor */
  #define SOCKET int
#endif

/** @addtogroup logging
    @brief Central logging functions for the library and calling programs

    This logging facility is used for all log messages produced by
    the library.

    The logging can be configured to send messages to arbitrary
    functions, referred to as \c log_print() and \c diag_print().
    This allows output to be re-directed to other logging systems if
    needed.

    It is also possible to assign prefixes to log messages for
    identification, referred to as \c logprefix and \c errprefix.

    @anchor logging-levels
    Three message levels are recognized:
    - 0 : Normal log messages, printed using \c log_print() with \c logprefix
    - 1  : Diagnostic messages, printed using \c diag_print() with \c logprefix
    - 2+ : Error messages, printed using \c diag_print() with \c errprefix

    It is the task of the \c sl_log(), \c sl_ms_log_l(), and
    \c sl_ms_log_rl() functions to format a message using printf
    conventions and pass the formatted string to the appropriate
    printing function.

    @{ */

/** @brief Logging parameters */
typedef struct SLlog_s
{
  void (*log_print) (const char *);  /**< Log message printing function */
  const char *logprefix;             /**< Log message message prefix */
  void (*diag_print) (const char *); /**< Warning & error message printing function */
  const char *errprefix;             /**< Warning & error message prefix */
  int verbosity;                     /**< Logging verbosity */
} SLlog;
/** @} */

/** @addtogroup seedlink-connection
    @brief Definitions and functions related to SeedLink connections
    @{ */

#define SL_DEFAULT_HOST  "localhost" /**< Default host for libslink */
#define SL_DEFAULT_PORT  "18000"     /**< Default port for libslink */
#define SL_SECURE_PORT   "18500"     /**< Recognized TLS port */

#define SLHEADSIZE_V3        8       /**< V3 SeedLink header size */
#define SLHEADSIZE_V4       17       /**< V4 SeedLink header size */
#define SIGNATURE_V3        "SL"     /**< V3 SeedLink header signature */
#define SIGNATURE_V4        "SE"     /**< V4 SeedLink header signature */
#define INFOSIGNATURE       "SLINFO" /**< SeedLink V3 INFO packet signature */
#define MAX_LOG_MSG_LENGTH  200      /**< Maximum length of log messages */
#define SL_MIN_PAYLOAD      64       /**< Minimum data for payload detection and tracking */
#define SL_MAX_PAYLOAD      16384    /**< Maximum data for payload detection and tracking */
#define SL_MAX_NETSTAID     22       /**< Maximum length of NET_STA station ID */

/** Protocols recognized by the library */
typedef enum
{
  UNSET_PROTO = 0, /**< Unset value */
  SLPROTO3X   = 1, /**< SeedLink 3.x */
  SLPROTO40   = 2, /**< SeedLink 4.0 */
} LIBPROTOCOL;

/** @page payload-types
    @brief Packet payload format and sub-format type values
**/
#define SLPAYLOAD_UNKNOWN        0  //!< Unknown payload
#define SLPAYLOAD_MSEED2INFO     1  //!< miniSEED 2 with INFO payload
#define SLPAYLOAD_MSEED2INFOTERM 2  //!< miniSEED 2 with INFO payload (terminated)
#define SLPAYLOAD_MSEED2        '2' //!< miniSEED 2
#define SLPAYLOAD_MSEED3        '3' //!< miniSEED 3
#define SLPAYLOAD_JSON          'J' //!< JSON payload
#define SLPAYLOAD_XML           'X' //!< XML payload

#define SLPAYLOAD_JSON_INFO     'I' //!< JSON payload, subformat SeedLink INFO
#define SLPAYLOAD_JSON_ERROR    'E' //!< JSON payload, subformat SeedLink ERROR

/** @page collect-status
    @brief Return values for sl_collect()
**/
#define SLPACKET                 1  //!< Complete packet returned
#define SLTERMINATE              0  //!< Error or connection termination
#define SLNOPACKET              -1  //!< No packet available for non-blocking
#define SLTOOLARGE              -2  //!< Received packet is too large for buffer

/** @def SL_UNSETSEQUENCE
    @brief Representation for unset sequence values. **/
#define SL_UNSETSEQUENCE UINT64_MAX

/** @def SL_ALLDATASEQUENCE
    @brief Representation for a sequence value meaning "all data". **/
#define SL_ALLDATASEQUENCE (UINT64_MAX - 1)

/** @def SLTMODULUS
    @brief Define the high precision time tick interval as 1/modulus seconds
    corresponding to **nanoseconds**. **/
#define SLTMODULUS 1000000000

/** @def SLTERROR
    @brief Error code for routines that normally return a high precision time.
    The time value corresponds to '1902-1-1 00:00:00.00000000'. **/
#define SLTERROR -2145916800000000000LL

/** @def SL_EPOCH2SLTIME
    @brief macro to convert Unix/POSIX epoch time to high precision epoch time */
#define SL_EPOCH2SLTIME(X) (X) * (int64_t) SLTMODULUS

/** @def SL_SLTIME2EPOCH
    @brief Macro to convert high precision epoch time to Unix/POSIX epoch time */
#define SL_SLTIME2EPOCH(X) (X) / SLTMODULUS

/** @def sl_dtime
    @brief Macro to return current time as double epoch, replace legacy function */
#define sl_dtime(X) SL_SLTIME2EPOCH((double)sl_nstime())

/** @brief SeedLink packet information */
typedef struct slpacketinfo_s
{
  uint64_t seqnum;              /**< Packet sequence number */
  uint32_t payloadlength;       /**< Packet payload length */
  uint32_t payloadcollected;    /**< Packet payload collected so far */
  char     netstaid[SL_MAX_NETSTAID]; /**< Station ID in NET_STA format */
  char     payloadformat;       /**< Packet payload format */
  char     payloadsubformat;    /**< Packet payload subformat */
  uint8_t  netstaidlength;      /**< Station ID length */
} SLpacketinfo;

/** @brief Stream information */
typedef struct slstream_s
{
  char     netstaid[SL_MAX_NETSTAID]; /**< Station ID in NET_STA format */
  char    *selectors;	          /**< SeedLink style selectors for this station */
  uint64_t seqnum;              /**< SeedLink sequence number for this station */
  char     timestamp[32];       /**< Time stamp of last packet received */
  struct   slstream_s *next;    /**< The next station in the chain */
} SLstream;

/** @brief Connection state information */
typedef struct stat_s
{
  SLpacketinfo packetinfo;      /**< Client-specific packet tracking */
  int64_t keepalive_time;       /**< Keepalive time stamp */
  int64_t netto_time;           /**< Network timeout time stamp */
  int64_t netdly_time;          /**< Network re-connect delay time stamp */

  /** Connection state */
  enum
  {
    DOWN, UP, STREAMING
  } conn_state;

  /** Stream state */
  enum
  {
    HEADER, NETSTAID, PAYLOAD
  } stream_state;

  /** INFO query state */
  enum
  {
    NoQuery, InfoQuery, KeepAliveQuery
  } query_state;

} SLstat;

/** @brief SeedLink connection description */
typedef struct slcd_s
{
  char       *sladdr;           /**< Caller-supplied address of SeedLink server */
  char       *slhost;           /**< The host of SeedLink server */
  char       *slport;           /**< The port of SeedLink server */
  char       *clientname;       /**< Client program name */
  char       *clientversion;    /**< Client program version */
  char       *start_time;     	/**< Start of time window */
  char       *end_time;		      /**< End of time window */
  int         keepalive;        /**< Interval to send keepalive/heartbeat (seconds) */
  int         iotimeout;        /**< Timeout for network I/O operations (seconds) */
  int         netto;            /**< Idle network timeout (seconds) */
  int         netdly;           /**< Network reconnect delay (seconds) */
  const char *(*auth_value)(const char *server, void *auth_data); /**< Authorization callback, return authorization value */
  void      (*auth_finish)(const char *server, void *auth_data); /**< Authorization finish, to free data, etc. */
  void       *auth_data;        /**< Authorization callback data */
  SLstream   *streams;		      /**< Pointer to list of streams */
  const char *info;             /**< INFO request to send */
  int8_t      noblock;          /**< Control blocking on collection */
  int8_t      dialup;           /**< Boolean flag to indicate dial-up mode */
  int8_t      batchmode;        /**< Batch mode (1 - requested, 2 - activated) */
  int8_t      lastpkttime;      /**< Boolean flag to control last packet time usage */
  int8_t      terminate;        /**< Flag to control connection termination */
  int8_t      resume;           /**< Boolean flag to control resuming with seq. numbers */
  int8_t      multistation;     /**< Boolean flag to indicate v3 multistation mode */

  SOCKET      link;             /**< The network socket descriptor */
  LIBPROTOCOL protocol;         /**< Protocol in use */
  uint32_t    server_protocols; /**< Server protocol versions supported by library */
  char       *capabilities;     /**< HELLO capabilities supported by server (incomplete) */
  char       *caparray;         /**< Array of capabilities */
  int         tls;              /**< TLS connection flag */
  void       *tlsctx;           /**< TLS context */
  SLstat     *stat;             /**< Connection state information */
  SLlog      *log;              /**< Logging parameters */

  uint8_t     recvbuffer[SL_MAX_PAYLOAD]; /**< Network receive buffer */
  uint32_t    recvdatalen;      /**< Length of data in receive buffer */
} SLCD;

extern int sl_collect (SLCD *slconn, const SLpacketinfo **packetinfo,
                       char *plbuffer, uint32_t plbuffersize);
extern SLCD *sl_newslcd (const char *clientname, const char *clientversion);
extern void sl_freeslcd (SLCD *slconn);
extern int sl_setclientname (SLCD *slconn, const char *name, const char *version);
extern int sl_setserveraddress (SLCD *slconn, const char *server_address);
extern int sl_settimewindow (SLCD *slconn, const char *start_time, const char *end_time);
extern int sl_setauthparams (SLCD *slconn,
                             const char *(*auth_value) (const char *server, void *auth_data),
                             void (*auth_finish) (const char *server, void *auth_data),
                             void *auth_data);
extern int sl_setkeepalive (SLCD *slconn, int keepalive);
extern int sl_setiotimeout (SLCD *slconn, int iotimeout);
extern int sl_setidletimeout (SLCD *slconn, int idletimeout);
extern int sl_setreconnectdelay (SLCD *slconn, int reconnectdelay);
extern int sl_setblockingmode (SLCD *slconn, int nonblock);
extern int sl_setdialupmode (SLCD *slconn, int dialup);
extern int sl_setbatchmode (SLCD *slconn, int batchmode);
extern int sl_addstream (SLCD *slconn, const char *netstaid,
                         const char *selectors, uint64_t seqnum,
                         const char *timestamp);
#define sl_setuniparams sl_setallstationparams /* For backwards compatibility */
extern int sl_setallstationparams (SLCD *slconn, const char *selectors,
                                   uint64_t seqnum, const char *timestamp);
extern int sl_request_info (SLCD *slconn, const char *infostr);
extern int sl_hascapability (SLCD *slconn, char *capability);
extern void sl_terminate (SLCD *slconn);
extern void sl_printslcd (SLCD *slconn);
extern int sl_read_streamlist (SLCD *slconn, const char *streamfile,
                               const char *defselect);
extern int sl_parse_streamlist (SLCD *slconn, const char *streamlist,
                                const char *defselect);
extern int sl_configlink (SLCD *slconn);
extern int sl_send_info (SLCD *slconn, const char *info_level,
                         int verbose);
extern SOCKET sl_connect (SLCD *slconn, int sayhello);
extern int sl_disconnect (SLCD *slconn);
extern int sl_ping (SLCD *slconn, char *serverid, char *site);
extern int sl_senddata (SLCD *slconn, void *buffer, size_t buflen,
                        const char *ident, void *resp, int resplen);
extern int64_t sl_recvdata (SLCD *slconn, void *buffer, size_t maxbytes,
                            const char *ident);
extern int sl_recvresp (SLCD *slconn, void *buffer, size_t maxbytes,
                        const char *command, const char *ident);
extern int sl_poll (SLCD *slconn, int readability, int writability, int timeout_ms);
/** @} */

/** @addtogroup logging
    @{ */

#if defined(__GNUC__) || defined(__clang__)
__attribute__((__format__ (__printf__, 3, 4)))
#endif
extern int sl_log (int level, int verb, const char *format, ...);
#if defined(__GNUC__) || defined(__clang__)
__attribute__((__format__ (__printf__, 4, 5)))
#endif
extern int sl_log_r (const SLCD * slconn, int level, int verb, const char *format, ...);
#if defined(__GNUC__) || defined(__clang__)
__attribute__ ((__format__ (__printf__, 4, 5)))
#endif

extern int sl_log_rl (const SLlog *log, int level, int verb, const char *format, ...);
extern void sl_loginit (int verbosity,
                        void (*log_print) (const char *), const char *logprefix,
                        void (*diag_print) (const char *), const char *errprefix);
extern void sl_loginit_r (SLCD *slconn, int verbosity,
                          void (*log_print) (const char *), const char *logprefix,
                          void (*diag_print) (const char *), const char *errprefix);
extern SLlog *sl_loginit_rl (SLlog *log, int verbosity,
                             void (*log_print) (const char *), const char *logprefix,
                             void (*diag_print) (const char *), const char *errprefix);
/** @} */

/** @addtogroup connection-state
    @brief Basic functionality for saving and recovering connections

    @{ */
extern int sl_recoverstate (SLCD *slconn, const char *statefile);
extern int sl_savestate (SLCD *slconn, const char *statefile);
/** @} */

/** @addtogroup utility-functions
    @brief General utilities

    Utilities and portable wrappers where system differences are present

    @{ */
extern int sl_payload_summary (const SLlog *log, const SLpacketinfo *packetinfo,
                               const char *plbuffer, uint32_t plbuffer_size,
                               char *summary, size_t summary_size);
extern int sl_payload_info (const SLlog *log, const SLpacketinfo *packetinfo,
                 const char *plbuffer, uint32_t plbuffer_size,
                 char *sourceid, size_t sourceid_size,
                 char *starttimestr, size_t starttimestr_size,
                 double *samplerate,  uint32_t *samplecount);
extern uint8_t sl_littleendianhost (void);
extern int sl_doy2md (int year, int jday, int *month, int *mday);
extern char *sl_protocol_details (LIBPROTOCOL protocol, uint8_t *major, uint8_t *minor);
extern const char *sl_formatstr (char format, char subformat);
extern const char *sl_strerror(void);
extern int64_t sl_nstime (void);
extern char *sl_isodatetime (char *isodatetime, const char *datetime);
extern char *sl_commadatetime (char *commadatetime, const char *datetime);
extern char *sl_v3to4selector (char *v4selector, int v4selectorlength, const char *selector);
extern void sl_usleep(unsigned long int useconds);
extern int sl_strncpclean (char *dest, const char *source, int length);

/** In-place byte swapping of 2 byte quantity */
static inline void
sl_gswap2 (void *data2)
{
  uint16_t dat;

  memcpy (&dat, data2, 2);

  dat = ((dat & 0xff00) >> 8) | ((dat & 0x00ff) << 8);

  memcpy (data2, &dat, 2);
}

/** In-place byte swapping of 4 byte quantity */
static inline void
sl_gswap4 (void *data4)
{
  uint32_t dat;

  memcpy (&dat, data4, 4);

  dat = ((dat & 0xff000000) >> 24) | ((dat & 0x000000ff) << 24) |
        ((dat & 0x00ff0000) >>  8) | ((dat & 0x0000ff00) <<  8);

  memcpy (data4, &dat, 4);
}

/** In-place byte swapping of 8 byte quantity */
static inline void
sl_gswap8 (void *data8)
{
  uint64_t dat;

  memcpy (&dat, data8, sizeof(uint64_t));

  dat = ((dat & 0xff00000000000000) >> 56) | ((dat & 0x00000000000000ff) << 56) |
        ((dat & 0x00ff000000000000) >> 40) | ((dat & 0x000000000000ff00) << 40) |
        ((dat & 0x0000ff0000000000) >> 24) | ((dat & 0x0000000000ff0000) << 24) |
        ((dat & 0x000000ff00000000) >>  8) | ((dat & 0x00000000ff000000) <<  8);

  memcpy (data8, &dat, sizeof(uint64_t));
}
/** @} */

#ifdef __cplusplus
}
#endif

#endif /* LIBSLINK_H */
