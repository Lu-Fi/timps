/* net.h - small socket helpers */
#ifndef MS_NET_H
#define MS_NET_H
#include <stdint.h>
#include <netinet/in.h>

int  net_listen_tcp(int port, int backlog);      /* returns listening fd or -1 */
int  net_udp_socket(void);                        /* unbound udp socket */
int  net_set_nonblock(int fd, int on);
int  net_set_nodelay(int fd);
/* SO_RCVTIMEO/SO_SNDTIMEO in seconds (0 = leave unset); recv()/send() then
 * fail with EAGAIN after that long blocked, so silent clients get dropped */
int  net_set_timeouts(int fd, int rcv_s, int snd_s);
int  net_sendall(int fd, const void *buf, int len); /* blocking full send, -1 err */
int  net_bind_udp_pair(int *rtp_fd, int *rtcp_fd, int base_port); /* even/odd */

#endif
