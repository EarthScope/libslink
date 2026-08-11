/***************************************************************************
 * test_netprims.c: sl_connect()/sl_senddata()/sl_recvdata()/sl_recvresp()/
 * sl_poll()/sl_disconnect() against a bare, hand-rolled loopback listener
 * -- no HELLO, no protocol negotiation.  All of these are public
 * functions, so this drives them directly rather than through
 * sl_collect().
 ***************************************************************************/

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "libslink.h"
#include "slt.h"

/* Bind and listen on 127.0.0.1 with an OS-assigned port. Returns the
 * listening fd and, via *port, the port number chosen. */
static int
start_listener (int *port)
{
  struct sockaddr_in addr;
  socklen_t addrlen = sizeof (addr);
  int fd = socket (AF_INET, SOCK_STREAM, 0);

  if (fd < 0)
    return -1;

  memset (&addr, 0, sizeof (addr));
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
  addr.sin_port        = 0;

  if (bind (fd, (struct sockaddr *)&addr, sizeof (addr)) < 0)
  {
    close (fd);
    return -1;
  }

  if (listen (fd, 1) < 0)
  {
    close (fd);
    return -1;
  }

  if (getsockname (fd, (struct sockaddr *)&addr, &addrlen) < 0)
  {
    close (fd);
    return -1;
  }

  *port = ntohs (addr.sin_port);

  return fd;
}

static SLCD *
connect_to (int listenfd, int port, int *out_serverfd)
{
  SLCD *slconn = sl_initslcd ("t", NULL);
  char address[64];
  SOCKET rv;

  snprintf (address, sizeof (address), "127.0.0.1:%d", port);
  sl_set_serveraddress (slconn, address);

  rv = sl_connect (slconn, 0 /* no HELLO */);

  if (rv < 0)
  {
    sl_freeslcd (slconn);
    return NULL;
  }

  *out_serverfd = accept (listenfd, NULL, NULL);

  return slconn;
}

static void
test_connect_and_disconnect (void)
{
  int port, listenfd, serverfd = -1;
  SLCD *slconn;

  listenfd = start_listener (&port);
  SLT_ASSERT (listenfd >= 0, "test listener created");

  slconn = connect_to (listenfd, port, &serverfd);
  SLT_NOT_NULL (slconn, "sl_connect() succeeds against a live loopback listener");
  SLT_ASSERT (serverfd >= 0, "the listener accepted the connection");
  SLT_ASSERT (slconn->link >= 0, "the SLCD link descriptor is set after connecting");

  SLT_EQ_INT (sl_disconnect (slconn), -1, "sl_disconnect() returns -1 (historical convention)");
  SLT_EQ_INT (slconn->link, -1, "sl_disconnect() resets the link descriptor");

  SLT_EQ_INT (sl_disconnect (slconn), -1, "a second sl_disconnect() call is idempotent, not a crash");

  close (serverfd);
  close (listenfd);
  sl_freeslcd (slconn);
}

static void
test_senddata (void)
{
  int port, listenfd, serverfd = -1;
  SLCD *slconn;
  char buf[16] = {0};
  ssize_t n;

  listenfd = start_listener (&port);
  slconn   = connect_to (listenfd, port, &serverfd);
  SLT_NOT_NULL (slconn, "connected for sl_senddata() test");

  SLT_EQ_INT (sl_senddata (slconn, (void *)"PING\r\n", 6, "id", NULL, 0), 0,
             "sl_senddata() without a response request returns 0");

  n = recv (serverfd, buf, sizeof (buf), 0);
  SLT_EQ_INT ((int)n, 6, "the server side received the bytes sl_senddata() sent");
  SLT_EQ_INT (memcmp (buf, "PING\r\n", 6), 0, "the received bytes match exactly");

  close (serverfd);
  close (listenfd);
  sl_disconnect (slconn);
  sl_freeslcd (slconn);
}

static void
test_recvdata (void)
{
  int port, listenfd, serverfd = -1;
  SLCD *slconn;
  char buf[16] = {0};
  int64_t n;

  listenfd = start_listener (&port);
  slconn   = connect_to (listenfd, port, &serverfd);
  SLT_NOT_NULL (slconn, "connected for sl_recvdata() test");

  /* Nothing has been sent yet: a non-blocking read reports 0, not an error. */
  n = sl_recvdata (slconn, buf, sizeof (buf), "id");
  SLT_EQ_INT ((int)n, 0, "sl_recvdata() returns 0 when no data is available (non-blocking)");

  send (serverfd, "PONG", 4, 0);
  SLT_ASSERT (sl_poll (slconn, 1, 0, 2000) > 0, "sl_poll() reports readability once data arrives");

  n = sl_recvdata (slconn, buf, sizeof (buf), "id");
  SLT_EQ_INT ((int)n, 4, "sl_recvdata() returns the number of bytes available");
  SLT_EQ_INT (memcmp (buf, "PONG", 4), 0, "sl_recvdata() delivers the correct bytes");

  close (serverfd);
  close (listenfd);
  sl_disconnect (slconn);
  sl_freeslcd (slconn);
}

static void
test_recvdata_on_closed_connection (void)
{
  int port, listenfd, serverfd = -1;
  SLCD *slconn;
  char buf[16];

  listenfd = start_listener (&port);
  slconn   = connect_to (listenfd, port, &serverfd);
  SLT_NOT_NULL (slconn, "connected for the closed-connection test");

  close (serverfd); /* server hangs up */

  /* Give the FIN a moment to arrive before reading. */
  sl_usleep (50000);

  SLT_EQ_INT ((int)sl_recvdata (slconn, buf, sizeof (buf), "id"), -1,
             "sl_recvdata() reports -1 once the peer has closed the connection");

  close (listenfd);
  sl_disconnect (slconn);
  sl_freeslcd (slconn);
}

static void
test_recvresp (void)
{
  int port, listenfd, serverfd = -1;
  SLCD *slconn;
  char resp[64];

  listenfd = start_listener (&port);
  slconn   = connect_to (listenfd, port, &serverfd);
  SLT_NOT_NULL (slconn, "connected for sl_recvresp() test");

  send (serverfd, "OK\r\n", 4, 0);

  SLT_EQ_INT (sl_recvresp (slconn, resp, sizeof (resp), "CMD\r\n", "id"), 4,
             "sl_recvresp() returns the number of bytes up to and including the CRLF terminator");
  SLT_EQ_INT (memcmp (resp, "OK\r\n", 4), 0, "sl_recvresp() captures the exact response bytes");

  close (serverfd);
  close (listenfd);
  sl_disconnect (slconn);
  sl_freeslcd (slconn);
}

static void
test_recvresp_split_across_reads (void)
{
  int port, listenfd, serverfd = -1;
  SLCD *slconn;
  char resp[64];

  listenfd = start_listener (&port);
  slconn   = connect_to (listenfd, port, &serverfd);
  SLT_NOT_NULL (slconn, "connected for the split-response test");

  /* Send the response in two pieces; sl_recvresp() reads one byte at a
   * time internally and must still assemble it correctly. */
  send (serverfd, "O", 1, 0);
  sl_usleep (20000);
  send (serverfd, "K\r\n", 3, 0);

  SLT_EQ_INT (sl_recvresp (slconn, resp, sizeof (resp), "CMD\r\n", "id"), 4,
             "sl_recvresp() assembles a response delivered across multiple TCP segments");
  SLT_EQ_INT (memcmp (resp, "OK\r\n", 4), 0, "the assembled response is correct");

  close (serverfd);
  close (listenfd);
  sl_disconnect (slconn);
  sl_freeslcd (slconn);
}

static void
test_poll_timeout (void)
{
  int port, listenfd, serverfd = -1;
  SLCD *slconn;

  listenfd = start_listener (&port);
  slconn   = connect_to (listenfd, port, &serverfd);
  SLT_NOT_NULL (slconn, "connected for sl_poll() timeout test");

  SLT_EQ_INT (sl_poll (slconn, 1, 0, 200), 0, "sl_poll() returns 0 when the timeout expires with no activity");

  close (serverfd);
  close (listenfd);
  sl_disconnect (slconn);
  sl_freeslcd (slconn);
}

static void
test_connect_refused (void)
{
  int port, probefd;
  SLCD *slconn;

  /* Reserve then immediately release a port so nothing is listening on it. */
  probefd = start_listener (&port);
  close (probefd);

  slconn = sl_initslcd ("t", NULL);
  {
    char address[64];
    snprintf (address, sizeof (address), "127.0.0.1:%d", port);
    sl_set_serveraddress (slconn, address);
  }

  SLT_ASSERT (sl_connect (slconn, 0) < 0, "sl_connect() fails against a port nothing is listening on");

  sl_freeslcd (slconn);
}

int
main (void)
{
  SLT_RUN (test_connect_and_disconnect);
  SLT_RUN (test_senddata);
  SLT_RUN (test_recvdata);
  SLT_RUN (test_recvdata_on_closed_connection);
  SLT_RUN (test_recvresp);
  SLT_RUN (test_recvresp_split_across_reads);
  SLT_RUN (test_poll_timeout);
  SLT_RUN (test_connect_refused);

  return SLT_REPORT ();
}
