/* net.h - small socket helpers */
#ifndef MS_NET_H
#define MS_NET_H
#include <stdint.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>

int  net_listen_tcp(int port, int backlog);      /* returns listening fd or -1 */
/* accept() with FD_CLOEXEC set atomically (accept4) so forked board scripts
 * (daynight/motion) don't inherit live client sockets; returns fd or -1 */
int  net_accept_cloexec(int lfd, struct sockaddr *sa, socklen_t *sl);
int  net_udp_socket(void);                        /* unbound udp socket */
int  net_set_nodelay(int fd);
/* SO_RCVTIMEO/SO_SNDTIMEO in seconds (0 = leave unset); recv()/send() then
 * fail with EAGAIN after that long blocked, so silent clients get dropped */
int  net_set_timeouts(int fd, int rcv_s, int snd_s);
int  net_sendall(int fd, const void *buf, int len); /* blocking full send, -1 err */
/* Scatter/gather twin of net_sendall(): blocking full send of iov[0..niov),
 * returning the total byte count written or -1. Exists so a caller can hand
 * the kernel a header and a payload that live in DIFFERENT buffers as ONE
 * syscall / one TCP segment, instead of memcpy'ing them together first
 * (rtsp.c's interleaved RTP path).
 *
 * PARTIAL WRITES are the whole difficulty and the reason this is a shared
 * helper rather than open-coded at the call site: TCP is a byte stream, so
 * sendmsg() may accept only part of the message when the socket buffer fills,
 * and the remainder must be re-issued from exactly the right offset. Getting
 * that wrong does not lose a packet, it splices garbage into the RTSP '$'
 * interleaved framing and desyncs the connection permanently (H-1) - the same
 * failure net_sendall()'s loop exists to prevent for a contiguous buffer.
 *
 * The caller's iov[] array IS MODIFIED IN PLACE (entries are advanced/consumed
 * as bytes are accepted); pass a scratch array, never a shared one. */
int  net_sendmsg_all(int fd, struct iovec *iov, int niov);
int  net_bind_udp_pair(int *rtp_fd, int *rtcp_fd, int base_port); /* even/odd */

#endif
