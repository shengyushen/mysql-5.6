/* Copyright (c) 2000, 2015, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/*
  Note that we can't have assertion on file descriptors;  The reason for
  this is that during mysql shutdown, another thread can close a file
  we are working on.  In this case we should just return read errors from
  the file descriptior.
*/

#include "vio_priv.h"

#ifdef HAVE_OPENSSL

#ifndef DBUG_OFF

static void
report_errors(SSL* ssl)
{
  unsigned long	l;
  const char *file;
  const char *data;
  int line, flags;
  char buf[512];

  DBUG_ENTER("report_errors");

  /* Peek at the error queue; don't use get_error so that the error is
     preserved for our caller. */
  if ((l= ERR_peek_error_line_data(&file,&line,&data,&flags)))
  {
    DBUG_PRINT("error", ("OpenSSL: %s:%s:%d:%s\n", ERR_error_string(l,buf),
			 file,line,(flags&ERR_TXT_STRING)?data:"")) ;
  }

  if (ssl)
    DBUG_PRINT("error", ("error: %s",
                         ERR_error_string(SSL_get_error(ssl, l), buf)));

  DBUG_PRINT("info", ("socket_errno: %d", socket_errno));
  DBUG_VOID_RETURN;
}

#endif


/**
  Obtain the equivalent system error status for the last SSL I/O operation.

  @param ssl_error  The result code of the failed TLS/SSL I/O operation.
*/

static void ssl_set_sys_error(int ssl_error)
{
  int error= 0;

  switch (ssl_error)
  {
  case SSL_ERROR_ZERO_RETURN:
    error= SOCKET_ECONNRESET;
    break;
  case SSL_ERROR_WANT_READ:
  case SSL_ERROR_WANT_WRITE:
#ifdef SSL_ERROR_WANT_CONNECT
  case SSL_ERROR_WANT_CONNECT:
#endif
#ifdef SSL_ERROR_WANT_ACCEPT
  case SSL_ERROR_WANT_ACCEPT:
#endif
    error= SOCKET_EWOULDBLOCK;
    break;
  case SSL_ERROR_SSL:
    /* Protocol error. */
#ifdef EPROTO
    error= EPROTO;
#else
    error= SOCKET_ECONNRESET;
#endif
    break;
  case SSL_ERROR_SYSCALL:
  case SSL_ERROR_NONE:
  default:
    break;
  };

  /* Set error status to a equivalent of the SSL error. */
  if (error)
  {
#ifdef _WIN32
    WSASetLastError(error);
#else
    errno= error;
#endif
  }
}


/**
  This function does two things:
    - it indicates whether a SSL I/O operation must be retried later;
    - it clears the OpenSSL error queue, thus the next OpenSSL-operation can be
      performed even after failed OpenSSL-call.

  @param vio  VIO object representing a SSL connection.
  @param ret  Value returned by a SSL I/O function.
  @param event[out]             The type of I/O event to wait/retry.
  @param ssl_errno_holder[out]  The SSL error code.

  @return Whether a SSL I/O operation should be deferred.
  @retval TRUE    Temporary failure, retry operation.
  @retval FALSE   Indeterminate failure.
*/

static my_bool ssl_should_retry(Vio *vio, int ret,
                                enum enum_vio_io_event *event,
                                unsigned long *ssl_errno_holder)
{
  int ssl_error;
  SSL *ssl= vio->ssl_arg;
  my_bool should_retry= TRUE;

  /* Retrieve the result for the SSL I/O operation. */
  ssl_error= SSL_get_error(ssl, ret);

  /* Retrieve the result for the SSL I/O operation. */
  switch (ssl_error)
  {
  case SSL_ERROR_WANT_READ:
    *event= VIO_IO_EVENT_READ;
    break;
  case SSL_ERROR_WANT_WRITE:
    *event= VIO_IO_EVENT_WRITE;
    break;
  default:
#ifndef DBUG_OFF  /* Debug build */
    report_errors(ssl);
#endif
    should_retry= FALSE;
    ssl_set_sys_error(ssl_error);
    break;
  }

  /*
    ERR_get_error() is actually the error we generally want to
    display if SSL_get_error indicates it is an SSL error (instead
    of, say, a network issue)
  */
  if (ssl_error == SSL_ERROR_SSL) {
    *ssl_errno_holder= ERR_get_error();
  } else {
    *ssl_errno_holder= ssl_error;
  }
  ERR_clear_error();

  return should_retry;
}


size_t vio_ssl_read(Vio *vio, uchar *buf, size_t size)
{
  int ret;
  SSL *ssl= vio->ssl_arg;
  unsigned long ssl_errno_not_used;

  DBUG_ENTER("vio_ssl_read");

  while (1)
  {
    enum enum_vio_io_event event = VIO_IO_EVENT_READ;

    // Verify the socket is non blocking.
    DBUG_ASSERT(!vio_is_blocking(vio));

#ifndef HAVE_YASSL
    /*
      OpenSSL: check that the SSL thread's error queue is cleared. Otherwise
      SSL_read() returns an error from the error queue, when SSL_read() failed
      because it would block.
    */
    DBUG_ASSERT(ERR_peek_error() == 0);
#endif

    ret= SSL_read(ssl, buf, size);

    if (ret >= 0)
      break;

    /* Process the SSL I/O error. */
    if (!ssl_should_retry(vio, ret, &event, &ssl_errno_not_used))
      break;

    if (vio->ssl_is_nonblocking) {
      socket_errno = SOCKET_EWOULDBLOCK;
      switch (event) {
        case VIO_IO_EVENT_READ:  DBUG_RETURN(VIO_SOCKET_WANT_READ);
        case VIO_IO_EVENT_WRITE: DBUG_RETURN(VIO_SOCKET_WANT_WRITE);
        default:                 DBUG_RETURN(VIO_SOCKET_ERROR);
      }
    }

    /* Attempt to wait for an I/O event. */
    if (vio_socket_io_wait(vio, event))
    {
      if (vio_was_timeout(vio))
      {
        DBUG_RETURN(event == VIO_IO_EVENT_READ ? VIO_SOCKET_READ_TIMEOUT
                                               : VIO_SOCKET_WRITE_TIMEOUT);
      } else {
        DBUG_RETURN(VIO_SOCKET_ERROR);
      }
    }
  }

  DBUG_RETURN(ret < 0 ? -1 : ret);
}


size_t vio_ssl_write(Vio *vio, const uchar *buf, size_t size)
{
  int ret;
  SSL *ssl= vio->ssl_arg;
  unsigned long ssl_errno_not_used;

  DBUG_ENTER("vio_ssl_write");

  while (1)
  {
    enum enum_vio_io_event event = VIO_IO_EVENT_READ;

    // Verify the socket is non blocking.
    DBUG_ASSERT(!vio_is_blocking(vio));

#ifndef HAVE_YASSL
    /*
      OpenSSL: check that the SSL thread's error queue is cleared. Otherwise
      SSL_write() returns an error from the error queue, when SSL_write() failed
      because it would block.
    */
    DBUG_ASSERT(ERR_peek_error() == 0);
#endif

    ret= SSL_write(ssl, buf, size);

    if (ret >= 0)
      break;

    /* Process the SSL I/O error. */
    if (!ssl_should_retry(vio, ret, &event, &ssl_errno_not_used))
      break;

    if (vio->ssl_is_nonblocking) {
      socket_errno = SOCKET_EWOULDBLOCK;
      switch (event) {
        case VIO_IO_EVENT_READ:  DBUG_RETURN(VIO_SOCKET_WANT_READ);
        case VIO_IO_EVENT_WRITE: DBUG_RETURN(VIO_SOCKET_WANT_WRITE);
        default:                 DBUG_RETURN(VIO_SOCKET_ERROR);
      }
    }

    /* Attempt to wait for an I/O event. */
    if (vio_socket_io_wait(vio, event))
    {
      if (vio_was_timeout(vio))
      {
        DBUG_RETURN(event == VIO_IO_EVENT_READ ? VIO_SOCKET_READ_TIMEOUT
                                               : VIO_SOCKET_WRITE_TIMEOUT);
      } else {
        DBUG_RETURN(VIO_SOCKET_ERROR);
      }
    }
  }

  DBUG_RETURN(ret < 0 ? -1 : ret);
}

#ifdef HAVE_YASSL

/* Emulate a blocking recv() call with vio_read(). */
static long yassl_recv(void *ptr, void *buf, size_t len)
{
  return vio_read(ptr, buf, len);
}


/* Emulate a blocking send() call with vio_write(). */
static long yassl_send(void *ptr, const void *buf, size_t len)
{
  return vio_write(ptr, buf, len);
}

#endif

int vio_ssl_shutdown(Vio *vio)
{
  int r= 0;
  SSL *ssl= (SSL*)vio->ssl_arg;
  DBUG_ENTER("vio_ssl_shutdown");

  if (ssl)
  {
    /*
    THE SSL standard says that SSL sockets must send and receive a close_notify
    alert on socket shutdown to avoid truncation attacks. However, this can
    cause problems since we often hold a lock during shutdown and this IO can
    take an unbounded amount of time to complete. Since our packets are self
    describing with length, we aren't vunerable to these attacks. Therefore,
    we just shutdown by closing the socket (quiet shutdown).
    */
    SSL_set_quiet_shutdown(ssl, 1); 
    
    switch ((r= SSL_shutdown(ssl))) {
    case 1:
      /* Shutdown successful */
      break;
    case 0:
      /*
        Shutdown not yet finished - since the socket is going to
        be closed there is no need to call SSL_shutdown() a second
        time to wait for the other side to respond
      */
      break;
    default: /* Shutdown failed */
      DBUG_PRINT("vio_error", ("SSL_shutdown() failed, error: %d",
                               SSL_get_error(ssl, r)));
      break;
    }
  }
  DBUG_RETURN(vio_shutdown(vio));
}


void vio_ssl_delete(Vio *vio)
{
  if (!vio)
    return; /* It must be safe to delete null pointer */

  if (vio->inactive == FALSE)
    vio_ssl_shutdown(vio); /* Still open, close connection first */

  if (vio->ssl_arg)
  {
    SSL_free((SSL*) vio->ssl_arg);
    vio->ssl_arg= 0;
  }

  vio_delete(vio);
}


/** SSL handshake handler. */
typedef int (*ssl_handshake_func_t)(SSL*);


/**
  Loop and wait until a SSL handshake is completed.

  @param vio    VIO object representing a SSL connection.
  @param ssl    SSL structure for the connection.
  @param func   SSL handshake handler.
  @param ssl_errno_holder[out]  The SSL error code.
  @param vio_action[out]        For nonblocking SSL.

  @return Return value is 0 on success.
          (size_t) -1 on error
          (size_t) -2 or -3 for nonblocking actions
          (size_t) -4 or -5 for read/write timeout
*/

static size_t ssl_handshake_loop(Vio *vio, SSL *ssl,
                              ssl_handshake_func_t func,
                              unsigned long *ssl_errno_holder)
{
  size_t ret = -1;
  DBUG_ENTER(__func__);

  vio->ssl_arg= ssl;

  /* Initiate the SSL handshake. */
  while (1)
  {
    enum enum_vio_io_event event = VIO_IO_EVENT_READ;

#ifndef HAVE_YASSL
    /*
      OpenSSL: check that the SSL thread's error queue is cleared. Otherwise
      SSL-handshake-function returns an error from the error queue, when the
      function failed because it would block.
    */
    DBUG_ASSERT(ERR_peek_error() == 0);
#endif
    int handshake_ret;
    handshake_ret = func(ssl);
    if (handshake_ret >= 1) {
      ret = 0;
      break;
    }

    /* Process the SSL I/O error. */
    if (!ssl_should_retry(vio, handshake_ret, &event, ssl_errno_holder))
      break;

    if (vio->ssl_is_nonblocking) {
      socket_errno = SOCKET_EWOULDBLOCK;
      switch (event) {
        case VIO_IO_EVENT_READ:
          ret = VIO_SOCKET_WANT_READ;
          break;
        case VIO_IO_EVENT_WRITE:
          ret = VIO_SOCKET_WANT_WRITE;
          break;
        default:
          ret = VIO_SOCKET_ERROR;
      }
      break;
    }

    /* Wait for I/O so that the handshake can proceed. */
    if (vio_socket_io_wait(vio, event))
    {
      if (vio_was_timeout(vio))
      {
        DBUG_RETURN(event == VIO_IO_EVENT_READ ? VIO_SOCKET_READ_TIMEOUT
                                               : VIO_SOCKET_WRITE_TIMEOUT);
      } else {
        DBUG_RETURN(VIO_SOCKET_ERROR);
      }
    }
  }

  vio->ssl_arg= NULL;

  DBUG_RETURN(ret);
}

/*
 * Initiates the SSL structure.
 * @return Return value is 1 on failure, or 0 on success.
 */
static int ssl_init(SSL **out_ssl,
                    struct st_VioSSLFd *ptr,
                    Vio *vio,
                    long timeout,
                    SSL_SESSION* ssl_session,
                    unsigned long *ssl_errno_holder)
{
  DBUG_ENTER("ssl_init");
  SSL *ssl;
  my_socket sd= mysql_socket_getfd(vio->mysql_socket);

  /* Declared here to make compiler happy */
#if !defined(HAVE_YASSL) && !defined(DBUG_OFF)
  int j, n;
#endif

  DBUG_PRINT("enter", ("ptr: 0x%lx, sd: %d  ctx: 0x%lx, session=0x%lx",
                       (long) ptr, sd, (long) ptr->ssl_context,
                       (long) ssl_session));
  if (!(ssl= SSL_new(ptr->ssl_context)))
  {
    DBUG_PRINT("error", ("SSL_new failure"));
    *ssl_errno_holder= ERR_get_error();
    ERR_clear_error();
    DBUG_RETURN(1);
  }
  if (ssl_session != NULL) {
    if (!SSL_set_session(ssl, ssl_session)) {
#ifndef DBUG_OFF
      DBUG_PRINT("error", ("SSL_set_session failed"));
      report_errors(ssl);
#endif
      ERR_clear_error();
    } else {
      DBUG_PRINT("info", ("reused existing session %ld",
                          (long)ssl_session));
    }
  }

  DBUG_PRINT("info", ("ssl: 0x%lx timeout: %ld", (long) ssl, timeout));
  SSL_SESSION_set_timeout(SSL_get_session(ssl), timeout);
  SSL_set_fd(ssl, sd);
#if !defined(HAVE_YASSL) && defined(SSL_OP_NO_COMPRESSION)
  SSL_set_options(ssl, SSL_OP_NO_COMPRESSION); /* OpenSSL >= 1.0 only */
#elif OPENSSL_VERSION_NUMBER >= 0x00908000L /* workaround for OpenSSL 0.9.8 */
  sk_SSL_COMP_zero(SSL_COMP_get_compression_methods());
#endif

#if !defined(HAVE_YASSL) && !defined(DBUG_OFF)
  {
    STACK_OF(SSL_COMP) *ssl_comp_methods = NULL;
    ssl_comp_methods = SSL_COMP_get_compression_methods();
    n= sk_SSL_COMP_num(ssl_comp_methods);
    DBUG_PRINT("info", ("Available compression methods:\n"));
    if (n == 0)
      DBUG_PRINT("info", ("NONE\n"));
    else
      for (j = 0; j < n; j++)
      {
        SSL_COMP *c = sk_SSL_COMP_value(ssl_comp_methods, j);
#if defined(OPENSSL_IS_BORINGSSL) || OPENSSL_VERSION_NUMBER < 0x10100000L
        DBUG_PRINT("info", ("  %d: %s\n", c->id, c->name));
#else
        DBUG_PRINT("info", ("  %d: %s\n",
              SSL_COMP_get_id(c), SSL_COMP_get0_name(c)));
#endif
      }
  }
#endif

#ifndef HAVE_YASSL
  SSL_set_options(ssl,
                  SSL_OP_NO_SSLv2 |
                  SSL_OP_NO_SSLv3 |
                  SSL_OP_SINGLE_DH_USE
                  );
#endif

  /*
    Since yaSSL does not support non-blocking send operations, use
    special transport functions that properly handles non-blocking
    sockets. These functions emulate the behavior of blocking I/O
    operations by waiting for I/O to become available.
  */
#ifdef HAVE_YASSL
  /* Set first argument of the transport functions. */
  yaSSL_transport_set_ptr(ssl, vio);
  /* Set functions to use in order to send and receive data. */
  yaSSL_transport_set_recv_function(ssl, yassl_recv);
  yaSSL_transport_set_send_function(ssl, yassl_send);
#endif
 *out_ssl = ssl;
 DBUG_RETURN(0);
}

/*
 * Completes the sslconnect procedures by setting VIO to SSL and debug
 * related information regarding the certificate.
 *
 * @return Return value is 1 on failure, or 0 on success.
 */
static int ssl_finish(SSL *ssl, Vio *vio) {
  DBUG_ENTER("ssl_finish");
  /*
    Connection succeeded. Install new function handlers,
    change type, set sd to the fd used when connecting
    and set pointer to the SSL structure
  */
  struct sockaddr_storage remote = vio->remote;
  if (vio_reset(vio, VIO_TYPE_SSL, SSL_get_fd(ssl), ssl, 0))
    DBUG_RETURN(1);
  vio->remote = remote;

#ifndef DBUG_OFF
  {
    /* Print some info about the peer */
    X509 *cert;
    char buf[512];

    DBUG_PRINT("info",("SSL connection succeeded"));
    DBUG_PRINT("info",("Using cipher: '%s'" , SSL_get_cipher_name(ssl)));

    if ((cert= SSL_get_peer_certificate (ssl)))
    {
      DBUG_PRINT("info",("Peer certificate:"));
      X509_NAME_oneline(X509_get_subject_name(cert), buf, sizeof(buf));
      DBUG_PRINT("info",("\t subject: '%s'", buf));
      X509_NAME_oneline(X509_get_issuer_name(cert), buf, sizeof(buf));
      DBUG_PRINT("info",("\t issuer: '%s'", buf));
      X509_free(cert);
    }
    else
      DBUG_PRINT("info",("Peer does not have certificate."));

    if (SSL_get_shared_ciphers(ssl, buf, sizeof(buf)))
    {
      DBUG_PRINT("info",("shared_ciphers: '%s'", buf));
    }
    else
      DBUG_PRINT("info",("no shared ciphers!"));
  }
#endif
  DBUG_RETURN(0);
}

/**
 * Establishes the SSL connection.
 * First, it initiates the SSL struct it not already set.
 * Then calls the handshake loop. When the ssl context is nonblocking,
 * this function will return `-1` and socket_errno SOCKET_EWOULDBLOCK
 * to signal that more polls will be required until the operation finishes.
 * In the end, this checks the certificate.
 *
 * @param ssl_errno_holder[out]  The SSL error code.
 *
 * @return Return value VIO_SOCKET_* action
 *    -1 -> error
 *    -2 -> wants read
 *    -3 -> wants write
 *    -4 -> read timeout
 *    -5 -> write timeout
 *    0  -> success
 */
static int ssl_do(struct st_VioSSLFd *ptr,
                  Vio *vio,
                  long timeout,
                  SSL_SESSION* ssl_session,
                  ssl_handshake_func_t func,
                  SSL **p_ssl,
                  unsigned long *ssl_errno_holder)
{
  DBUG_ENTER("ssl_do");

  SSL *ssl = NULL;
  /*
   * For Async operations `p_ssl` is non-NULL, when `p_ssl` is already pointing
   * to a valid SSL*, `ssl` is initialized with that and `*p_ssl` is set to
   * NULL. We only let `*p_ssl` non-NULL if the operation is nonblocking and
   * not completed yet.
   */
  if (p_ssl != NULL && *p_ssl != NULL) {
    ssl = *p_ssl;
    *p_ssl = NULL;
  } else {
    if (ssl_init(
            &ssl, ptr, vio, timeout,
            ssl_session, ssl_errno_holder)) {
        DBUG_RETURN(-1);
    }
  }

  size_t loop_ret;
  if ((loop_ret = ssl_handshake_loop(vio, ssl, func, ssl_errno_holder)))
  {
    if (loop_ret != VIO_SOCKET_ERROR && p_ssl != NULL) {
      // This is a non-blocking operation, and hasn't yet completed.
      // Save `ssl` for future calls.
      *p_ssl = ssl;
      DBUG_RETURN( (int) loop_ret); // Don't free SSL
    }
    // We should have a case there loop_ret isn't VIO_SOCKET_ERROR and p_ssl is
    // NULL. If that happens, it might mean that `sslaccept` now is using
    // nonblocking calls and that is supported in this function.
    DBUG_ASSERT(loop_ret == VIO_SOCKET_ERROR || (loop_ret != VIO_SOCKET_ERROR && !vio_is_blocking(vio)));

#ifndef DBUG_OFF  /* Debug build */
    report_errors(ssl);
#endif
    DBUG_PRINT("error", ("SSL_connect/accept failure"));
    SSL_free(ssl);
    DBUG_RETURN(-1);
  }

  DBUG_PRINT("info", ("reused session: %ld", (long)SSL_session_reused(ssl)));
  int r = ssl_finish(ssl, vio);
  DBUG_RETURN(r ? -1 : 0);
}


int sslaccept(struct st_VioSSLFd *ptr, Vio *vio, long timeout,
              unsigned long *ssl_errno_holder)
{
  DBUG_ENTER("sslaccept");
  vio_set_blocking(vio, FALSE);
  DBUG_RETURN(
      ssl_do(
        ptr, vio, timeout, NULL, SSL_accept, NULL, ssl_errno_holder
      ) == 0 ? 0 : 1);
}

/*
 * Return values are the same as `ssl_do`.
 */
int sslconnect(struct st_VioSSLFd *ptr, Vio *vio, long timeout,
               SSL_SESSION* ssl_session, SSL** ssl,
               unsigned long *ssl_errno_holder)
{
  DBUG_ENTER("sslconnect");
  DBUG_RETURN(ssl_do(ptr, vio, timeout, ssl_session,
                     SSL_connect, ssl, ssl_errno_holder));
}

my_bool vio_ssl_has_data(Vio *vio)
{
  return SSL_pending(vio->ssl_arg) > 0 ? TRUE : FALSE;
}

int vio_ssl_set_blocking(Vio *vio, my_bool status)
{
  DBUG_ENTER("vio_ssl_set_blocking");
  int ret;
  ret = vio_set_blocking(vio, FALSE);
  vio->ssl_is_nonblocking = !status;
  DBUG_RETURN(ret);
}

#endif /* HAVE_OPENSSL */
