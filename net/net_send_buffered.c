/****************************************************************************
 * net/net_send_buffered.c
 *
 *   Copyright (C) 2007-2014 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *           Jason Jiang  <jasonj@live.cn>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#if defined(CONFIG_NET) && defined(CONFIG_NET_TCP) && \
    defined(CONFIG_NET_TCP_WRITE_BUFFERS)

#if defined(CONFIG_DEBUG) && defined(CONFIG_NET_TCP_WRBUFFER_DEBUG)
/* Force debug output (from this file only) */

#  undef  CONFIG_DEBUG_NET
#  define CONFIG_DEBUG_NET 1
#endif

#include <sys/types.h>
#include <sys/socket.h>

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <debug.h>
#include <debug.h>

#include <arch/irq.h>
#include <nuttx/clock.h>
#include <nuttx/net/arp.h>
#include <nuttx/net/iob.h>
#include <nuttx/net/uip/uip-arch.h>

#include "net_internal.h"
#include "tcp/tcp.h"
#include "uip/uip_internal.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define TCPBUF ((struct uip_tcpip_hdr *)&dev->d_buf[UIP_LLH_LEN])

/* Debug */

#ifdef CONFIG_NET_TCP_WRBUFFER_DUMP
#  define BUF_DUMP(msg,buf,len) lib_dumpbuffer(msg,buf,len)
#else
#  define BUF_DUMP(msg,buf,len,offset)
#  undef  WRB_DUMP
#  define WRB_DUMP(msg,wrb,len,offset)
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Function: send_insert_seqment
 *
 * Description:
 *   Insert a new segment in a write buffer queue, keep the segment queue in
 *   ascending order of sequence number.
 *
 * Parameters:
 *   wrb   The segment to be inserted
 *   q     The write buffer queue in which to insert the segment
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Running at the interrupt level
 *
 ****************************************************************************/

static void send_insert_seqment(FAR struct tcp_wrbuffer_s *wrb,
                                FAR sq_queue_t *q)
{
  sq_entry_t *entry = (sq_entry_t*)wrb;
  sq_entry_t *insert = NULL;

  sq_entry_t *itr;
  for (itr = sq_peek(q); itr; itr = sq_next(itr))
    {
      FAR struct tcp_wrbuffer_s *wrb0 = (FAR struct tcp_wrbuffer_s*)itr;
      if (WRB_SEQNO(wrb0) < WRB_SEQNO(wrb))
        {
          insert = itr;
        }
      else
        {
          break;
        }
    }

  if (insert)
    {
      sq_addafter(insert, entry, q);
    }
  else
    {
      sq_addfirst(entry, q);
    }
}

/****************************************************************************
 * Function: lost_connection
 *
 * Description:
 *   The TCP connection has been lost.  Free all write buffers.
 *
 * Parameters:
 *   psock    The socket structure
 *   conn     The connection structure associated with the socket
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static inline void lost_connection(FAR struct socket *psock,
                                   FAR struct uip_conn *conn)
{
  FAR sq_entry_t *entry;
  FAR sq_entry_t *next;

  /* Do not allow any further callbacks */

  psock->s_sndcb->flags = 0;
  psock->s_sndcb->event = NULL;

  /* Free all queued write buffers */

  for (entry = sq_peek(&conn->unacked_q); entry; entry = next)
    {
      next = sq_next(entry);
      tcp_wrbuffer_release((FAR struct tcp_wrbuffer_s *)entry);
    }

  for (entry = sq_peek(&conn->write_q); entry; entry = next)
    {
      next = sq_next(entry);
      tcp_wrbuffer_release((FAR struct tcp_wrbuffer_s *)entry);
    }

  /* Reset write buffering variables */

  sq_init(&conn->unacked_q);
  sq_init(&conn->write_q);
  conn->sent = 0;
}

/****************************************************************************
 * Function: send_interrupt
 *
 * Description:
 *   This function is called from the interrupt level to perform the actual
 *   send operation when polled by the uIP layer.
 *
 * Parameters:
 *   dev      The structure of the network driver that caused the interrupt
 *   conn     The connection structure associated with the socket
 *   flags    Set of events describing why the callback was invoked
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Running at the interrupt level
 *
 ****************************************************************************/

static uint16_t send_interrupt(FAR struct uip_driver_s *dev, FAR void *pvconn,
                               FAR void *pvpriv, uint16_t flags)
{
  FAR struct uip_conn *conn = (FAR struct uip_conn *)pvconn;
  FAR struct socket *psock = (FAR struct socket *)pvpriv;

  nllvdbg("flags: %04x\n", flags);

  /* If this packet contains an acknowledgement, then update the count of
   * acknowledged bytes.
   */

  if ((flags & UIP_ACKDATA) != 0)
    {
      FAR struct tcp_wrbuffer_s *wrb;
      FAR sq_entry_t *entry;
      FAR sq_entry_t *next;
      uint32_t ackno;

      ackno = uip_tcpgetsequence(TCPBUF->ackno);
      nllvdbg("ACK: ackno=%u flags=%04x\n", ackno, flags);

      /* Look at every write buffer in the unacked_q.  The unacked_q
       * holds write buffers that have been entirely sent, but which
       * have not yet been ACKed.
       */

      for (entry = sq_peek(&conn->unacked_q); entry; entry = next)
        {
          uint32_t lastseq;

          /* Check of some or all of this write buffer has been ACKed. */

          next = sq_next(entry);
          wrb = (FAR struct tcp_wrbuffer_s*)entry;

          /* If the ACKed sequence number is greater than the start
           * sequence number of the write buffer, then some or all of
           * the write buffer has been ACKed.
           */

          if (ackno > WRB_SEQNO(wrb))
            {
              /* Get the sequence number at the end of the data */

              lastseq = WRB_SEQNO(wrb) + WRB_PKTLEN(wrb);
              nllvdbg("ACK: wrb=%p seqno=%u lastseq=%u pktlen=%u ackno=%u\n",
                      wrb, WRB_SEQNO(wrb), lastseq, WRB_PKTLEN(wrb), ackno);

              /* Has the entire buffer been ACKed? */

              if (ackno >= lastseq)
                {
                  nllvdbg("ACK: wrb=%p Freeing write buffer\n", wrb);

                  /* Yes... Remove the write buffer from ACK waiting queue */

                  sq_rem(entry, &conn->unacked_q);

                  /* And return the write buffer to the pool of free buffers */

                  tcp_wrbuffer_release(wrb);
                }
              else
                {
                  unsigned int trimlen;

                  /* No, then just trim the ACKed bytes from the beginning
                   * of the write buffer.  This will free up some I/O buffers
                   * that can be reused while are still sending the last
                   * buffers in the chain.
                   */

                  trimlen = ackno - WRB_SEQNO(wrb);
                  if (trimlen > WRB_SENT(wrb))
                    {
                      /* More data has been ACKed then we have sent? */

                      trimlen = WRB_SENT(wrb);
                    }

                  nllvdbg("ACK: wrb=%p trim %u bytes\n", wrb, trimlen);

                  WRB_TRIM(wrb, trimlen);
                  WRB_SEQNO(wrb) = ackno;
                  WRB_SENT(wrb) -= trimlen;

                  /* Set the new sequence number for what remains */

                  nllvdbg("ACK: wrb=%p seqno=%u pktlen=%u\n",
                          wrb, WRB_SEQNO(wrb), WRB_PKTLEN(wrb));
                }
            }
        }

      /* A special case is the head of the write_q which may be partially
       * sent and so can still have un-ACKed bytes that could get ACKed
       * before the entire write buffer has even been sent.
       */

      wrb = (FAR struct tcp_wrbuffer_s*)sq_peek(&conn->write_q);
      if (wrb && WRB_SENT(wrb) > 0 && ackno > WRB_SEQNO(wrb))
        {
          uint32_t nacked;

          /* Number of bytes that were ACKed */

          nacked = ackno - WRB_SEQNO(wrb);
          if (nacked > WRB_SENT(wrb))
            {
              /* More data has been ACKed then we have sent? ASSERT? */

              nacked = WRB_SENT(wrb);
            }

          nllvdbg("ACK: wrb=%p seqno=%u nacked=%u sent=%u ackno=%u\n",
                  wrb, WRB_SEQNO(wrb), nacked, WRB_SENT(wrb), ackno);

          /* Trim the ACKed bytes from the beginning of the write buffer. */

          WRB_TRIM(wrb, nacked);
          WRB_SEQNO(wrb) = ackno;
          WRB_SENT(wrb) -= nacked;

          nllvdbg("ACK: wrb=%p seqno=%u pktlen=%u sent=%u\n",
                  wrb, WRB_SEQNO(wrb), WRB_PKTLEN(wrb), WRB_SENT(wrb));
        }
    }

  /* Check for a loss of connection */

  else if ((flags & (UIP_CLOSE | UIP_ABORT | UIP_TIMEDOUT)) != 0)
    {
      nllvdbg("Lost connection: %04x\n", flags);

      /* Report not connected */

      net_lostconnection(psock, flags);

      /* Free write buffers and terminate polling */

      lost_connection(psock, conn);
      return flags;
    }

   /* Check if we are being asked to retransmit data */

   else if ((flags & UIP_REXMIT) != 0)
    {
      FAR struct tcp_wrbuffer_s *wrb;
      FAR sq_entry_t *entry;

      nllvdbg("REXMIT: %04x\n", flags);

      /* If there is a partially sent write buffer at the head of the
       * write_q?  Has anything been sent from that write buffer?
       */

      wrb = (FAR struct tcp_wrbuffer_s *)sq_peek(&conn->write_q);
      nllvdbg("REXMIT: wrb=%p sent=%u\n", wrb, wrb ? WRB_SENT(wrb) : 0);

      if (wrb != NULL && WRB_SENT(wrb) > 0)
        {
          FAR struct tcp_wrbuffer_s *tmp;
          uint16_t sent;

          /* Yes.. Reset the number of bytes sent sent from the write buffer */

          sent = WRB_SENT(wrb);
          if (conn->unacked > sent)
            {
              conn->unacked -= sent;
            }
          else
            {
              conn->unacked = 0;
            }

          if (conn->sent > sent)
            {
              conn->sent -= sent;
            }
          else
            {
              conn->sent = 0;
            }

          WRB_SENT(wrb) = 0;
          nllvdbg("REXMIT: wrb=%p sent=%u, conn unacked=%d sent=%d\n",
                  wrb, WRB_SENT(wrb), conn->unacked, conn->sent);

          /* Increment the retransmit count on this write buffer. */

          if (++WRB_NRTX(wrb) >= UIP_MAXRTX)
            {
              nlldbg("Expiring wrb=%p nrtx=%u\n", wrb, WRB_NRTX(wrb));

              /* The maximum retry count as been exhausted. Remove the write
               * buffer at the head of the queue.
               */

              tmp = (FAR struct tcp_wrbuffer_s *)sq_remfirst(&conn->write_q);
              DEBUGASSERT(tmp == wrb);
              UNUSED(wrb);

              /* And return the write buffer to the free list */

              tcp_wrbuffer_release(wrb);

              /* NOTE expired is different from un-ACKed, it is designed to
               * represent the number of segments that have been sent,
               * retransmitted, and un-ACKed, if expired is not zero, the
               * connection will be closed.
               *
               * field expired can only be updated at UIP_ESTABLISHED state
               */

              conn->expired++;
            }
        }
      
      /* Move all segments that have been sent but not ACKed to the write
       * queue again note, the un-ACKed segments are put at the head of the
       * write_q so they can be resent as soon as possible.
       */

      while ((entry = sq_remlast(&conn->unacked_q)) != NULL)
        {
          wrb = (FAR struct tcp_wrbuffer_s*)entry;
          uint16_t sent;

          /* Reset the number of bytes sent sent from the write buffer */

          sent = WRB_SENT(wrb);
          if (conn->unacked > sent)
            {
              conn->unacked -= sent;
            }
          else
            {
              conn->unacked = 0;
            }

          if (conn->sent > sent)
            {
              conn->sent -= sent;
            }
          else
            {
              conn->sent = 0;
            }

          WRB_SENT(wrb) = 0;
          nllvdbg("REXMIT: wrb=%p sent=%u, conn unacked=%d sent=%d\n",
                  wrb, WRB_SENT(wrb), conn->unacked, conn->sent);

          /* Free any write buffers that have exceed the retry count */

          if (++WRB_NRTX(wrb) >= UIP_MAXRTX)
            {
              nlldbg("Expiring wrb=%p nrtx=%u\n", wrb, WRB_NRTX(wrb));

              /* Return the write buffer to the free list */

              tcp_wrbuffer_release(wrb);

              /* NOTE expired is different from un-ACKed, it is designed to
               * represent the number of segments that have been sent,
               * retransmitted, and un-ACKed, if expired is not zero, the
               * connection will be closed.
               *
               * field expired can only be updated at UIP_ESTABLISHED state
               */

              conn->expired++;
              continue;
            }
          else
            {
              /* Insert the write buffer into the write_q (in sequence
               * number order).  The retransmission will occur below
               * when the write buffer with the lowest sequenc number
               * is pulled from the write_q again.
               */

              nllvdbg("REXMIT: Moving wrb=%p nrtx=%u\n", wrb, WRB_NRTX(wrb));

              send_insert_seqment(wrb, &conn->write_q);
            }
        }
    }

  /* Check if the outgoing packet is available (it may have been claimed
   * by a sendto interrupt serving a different thread).
   */

  if (dev->d_sndlen > 0)
    {
      /* Another thread has beat us sending data, wait for the next poll */

      return flags;
    }

  /* We get here if (1) not all of the data has been ACKed, (2) we have been
   * asked to retransmit data, (3) the connection is still healthy, and (4)
   * the outgoing packet is available for our use.  In this case, we are
   * now free to send more data to receiver -- UNLESS the buffer contains
   * unprocessed incoming data.  In that event, we will have to wait for the
   * next polling cycle.
   */

  if ((conn->tcpstateflags & UIP_ESTABLISHED) &&
      (flags & (UIP_POLL | UIP_REXMIT)) &&
      !(sq_empty(&conn->write_q)))
    {
      /* Check if the destination IP address is in the ARP table.  If not,
       * then the send won't actually make it out... it will be replaced with
       * an ARP request.
       *
       * NOTE 1: This could be an expensive check if there are a lot of
       * entries in the ARP table.
       *
       * NOTE 2: If we are actually harvesting IP addresses on incoming IP
       * packets, then this check should not be necessary; the MAC mapping
       * should already be in the ARP table.
       */

#if defined(CONFIG_NET_ETHERNET) && !defined(CONFIG_NET_ARP_IPIN)
      if (arp_find(conn->ripaddr) != NULL)
#endif
        {
          FAR struct tcp_wrbuffer_s *wrb;
          size_t sndlen;

          /* Peek at the head of the write queue (but don't remove anything
           * from the write queue yet).  We know from the above test that
           * the write_q is not empty.
           */

          wrb = (FAR struct tcp_wrbuffer_s *)sq_peek(&conn->write_q);
          DEBUGASSERT(wrb);

          /* Get the amount of data that we can send in the next packet.
           * We will send either the remaining data in the buffer I/O
           * buffer chain, or as much as will fit given the MSS and current
           * window size.
           */

          sndlen = WRB_PKTLEN(wrb) - WRB_SENT(wrb);
          if (sndlen > uip_mss(conn))
            {
              sndlen = uip_mss(conn);
            }

          if (sndlen > conn->winsize)
            {
              sndlen = conn->winsize;
            }

          nllvdbg("SEND: wrb=%p pktlen=%u sent=%u sndlen=%u\n",
                  wrb, WRB_PKTLEN(wrb), WRB_SENT(wrb), sndlen);

          /* Is this the first we have tried to send from this
           * write buffer?
           */

          if (WRB_SENT(wrb) == 0)
            {
              /* Yes.. Set the sequence number for this segment.  If
               * we are retransmitting, then the sequence number will
               * already be set for this write buffer.
               */
           
              if (WRB_SEQNO(wrb) == (unsigned)-1)
                {
                  WRB_SEQNO(wrb) = conn->isn + conn->sent;
                }

              /* The TCP stack updates sndseq on receipt of ACK *before*
               * this function is called. In that case sndseq will point
               * to the next unacknowledged byte (which might have already
               * been sent). We will overwrite the value of sndseq here
               * before the packet is sent.
               */

              uip_tcpsetsequence(conn->sndseq, WRB_SEQNO(wrb));
            }

          /* Then set-up to send that amount of data with the offset
           * corresponding to the amount of data already sent. (this
           * won't actually happen until the polling cycle completes).
           */

          uip_iobsend(dev, WRB_IOB(wrb), sndlen, WRB_SENT(wrb));

          /* Remember how much data we send out now so that we know
           * when everything has been acknowledged.  Just increment
           * the amount of data sent. This will be needed in
           * sequence number calculations.
           */

          conn->unacked += sndlen;
          conn->sent    += sndlen;

          nllvdbg("SEND: wrb=%p nrtx=%u unacked=%u sent=%u\n",
                  wrb, WRB_NRTX(wrb), conn->unacked, conn->sent);

          /* Increment the count of bytes sent from this write buffer */

          WRB_SENT(wrb) += sndlen;

          nllvdbg("SEND: wrb=%p sent=%u pktlen=%u\n",
                  wrb, WRB_SENT(wrb), WRB_PKTLEN(wrb));

          /* Remove the write buffer from the write queue if the
           * last of the data has been sent from the buffer.
           */

          DEBUGASSERT(WRB_SENT(wrb) <= WRB_PKTLEN(wrb));
          if (WRB_SENT(wrb) >= WRB_PKTLEN(wrb))
            {
              FAR struct tcp_wrbuffer_s *tmp;

              nllvdbg("ACK: wrb=%p Move to unacked_q\n", wrb);

              tmp = (FAR struct tcp_wrbuffer_s *)sq_remfirst(&conn->write_q);
              DEBUGASSERT(tmp == wrb);
              UNUSED(tmp);

              /* Put the I/O buffer chain in the un-acked queue; the
               * segment is waiting for ACK again
               */

              send_insert_seqment(wrb, &conn->unacked_q);
            }

          /* Only one data can be sent by low level driver at once,
           * tell the caller stop polling the other connection.
           */

          flags &= ~UIP_POLL;
        }
    }

  /* Continue waiting */

  return flags;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Function: psock_send
 *
 * Description:
 *   The send() call may be used only when the socket is in a connected state
 *   (so that the intended recipient is known). The only difference between
 *   send() and write() is the presence of flags. With zero flags parameter,
 *   send() is equivalent to write(). Also, send(sockfd,buf,len,flags) is
 *   equivalent to sendto(sockfd,buf,len,flags,NULL,0).
 *
 * Parameters:
 *   psock    An instance of the internal socket structure.
 *   buf      Data to send
 *   len      Length of data to send
 *   flags    Send flags
 *
 * Returned Value:
 *   On success, returns the number of characters sent.  On  error,
 *   -1 is returned, and errno is set appropriately:
 *
 *   EAGAIN or EWOULDBLOCK
 *     The socket is marked non-blocking and the requested operation
 *     would block.
 *   EBADF
 *     An invalid descriptor was specified.
 *   ECONNRESET
 *     Connection reset by peer.
 *   EDESTADDRREQ
 *     The socket is not connection-mode, and no peer address is set.
 *   EFAULT
 *      An invalid user space address was specified for a parameter.
 *   EINTR
 *      A signal occurred before any data was transmitted.
 *   EINVAL
 *      Invalid argument passed.
 *   EISCONN
 *     The connection-mode socket was connected already but a recipient
 *     was specified. (Now either this error is returned, or the recipient
 *     specification is ignored.)
 *   EMSGSIZE
 *     The socket type requires that message be sent atomically, and the
 *     size of the message to be sent made this impossible.
 *   ENOBUFS
 *     The output queue for a network interface was full. This generally
 *     indicates that the interface has stopped sending, but may be
 *     caused by transient congestion.
 *   ENOMEM
 *     No memory available.
 *   ENOTCONN
 *     The socket is not connected, and no target has been given.
 *   ENOTSOCK
 *     The argument s is not a socket.
 *   EOPNOTSUPP
 *     Some bit in the flags argument is inappropriate for the socket
 *     type.
 *   EPIPE
 *     The local end has been shut down on a connection oriented socket.
 *     In this case the process will also receive a SIGPIPE unless
 *     MSG_NOSIGNAL is set.
 *
 * Assumptions:
 *
 ****************************************************************************/

ssize_t psock_send(FAR struct socket *psock, FAR const void *buf, size_t len,
                   int flags)
{
  uip_lock_t save;
  ssize_t    result = 0;
  int        err;
  int        ret = OK;

  if (!psock || psock->s_crefs <= 0)
    {
      err = EBADF;
      goto errout;
    }

  if (psock->s_type != SOCK_STREAM || !_SS_ISCONNECTED(psock->s_flags))
    {
      err = ENOTCONN;
      goto errout;
    }

  /* Dump the incoming buffer */

  BUF_DUMP("psock_send", buf, len);

  /* Set the socket state to sending */

  psock->s_flags = _SS_SETSTATE(psock->s_flags, _SF_SEND);

  save = uip_lock();

  if (len > 0)
    {
      struct uip_conn *conn = (struct uip_conn*)psock->s_conn;

      /* Allocate resources to receive a callback */

      if (!psock->s_sndcb)
        {
          psock->s_sndcb = uip_tcpcallbackalloc(conn);
        }

      /* Test if the callback has been allocated */

      if (!psock->s_sndcb)
        {
          /* A buffer allocation error occurred */

          ndbg("ERROR: Failed to allocate callback\n");
          result = -ENOMEM;
        }
      else
        {
          FAR struct tcp_wrbuffer_s *wrb;

          /* Set up the callback in the connection */

          psock->s_sndcb->flags = (UIP_ACKDATA | UIP_REXMIT |UIP_POLL | \
                                  UIP_CLOSE | UIP_ABORT | UIP_TIMEDOUT);
          psock->s_sndcb->priv  = (void*)psock;
          psock->s_sndcb->event = send_interrupt;

          /* Allocate an write buffer */

          wrb = tcp_wrbuffer_alloc();
          if (wrb)
            {
              /* Initialize the write buffer */

              WRB_SEQNO(wrb) = (unsigned)-1;
              WRB_NRTX(wrb)  = 0;
              WRB_COPYIN(wrb, (FAR uint8_t *)buf, len);

              /* Dump I/O buffer chain */

              WRB_DUMP("I/O buffer chain", wrb, WRB_PKTLEN(wrb), 0);

              /* send_interrupt() will send data in FIFO order from the
               * conn->write_q
               */

              sq_addlast(&wrb->wb_node, &conn->write_q);
              nvdbg("Queued WRB=%p pktlen=%u write_q(%p,%p)\n",
                    wrb, WRB_PKTLEN(wrb),
                    conn->write_q.head, conn->write_q.tail);

              /* Notify the device driver of the availability of TX data */

              netdev_txnotify(conn->ripaddr);
              result = len;
            }

          /* A buffer allocation error occurred */

          else
            {
              ndbg("ERROR: Failed to allocate write buffer\n");
              result = -ENOMEM;
            }
        }
    }

  uip_unlock(save);

  /* Set the socket state to idle */

  psock->s_flags = _SS_SETSTATE(psock->s_flags, _SF_IDLE);

  /* Check for a errors.  Errors are signaled by negative errno values
   * for the send length
   */

  if (result < 0)
    {
      err = result;
      goto errout;
    }

  /* If uip_lockedwait failed, then we were probably reawakened by a signal.
   * In this case, uip_lockedwait will have set errno appropriately.
   */

  if (ret < 0)
    {
      err = -ret;
      goto errout;
    }

  /* Return the number of bytes actually sent */

  return result;

errout:
  set_errno(err);
  return ERROR;
}

#endif /* CONFIG_NET && CONFIG_NET_TCP && CONFIG_NET_TCP_WRITE_BUFFERS */
