/* net.h - small socket helpers */
#ifndef MS_NET_H
#define MS_NET_H
#include <stdint.h>
#include <netinet/in.h>

int  net_listen_tcp(int port, int backlog);      /* returns listening fd or -1 */
int  net_udp_socket(void);                        /* unbound udp socket */
int  net_set_nonblock(int fd, int on);
int  net_set_nodelay(int fd);
int  net_sendall(int fd, const void *buf, int len); /* blocking full send, -1 err */
int  net_bind_udp_pair(int *rtp_fd, int *rtcp_fd, int base_port); /* even/odd */

#endif
