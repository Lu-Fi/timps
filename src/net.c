#include "net.h"
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/* Mark a fd close-on-exec so child processes we spawn (e.g. the backchannel's
 * popen("/bin/iac")) don't inherit our listen/media sockets - otherwise a
 * timps restart during a talk session can't rebind its ports. */
static void net_cloexec(int fd)
{
    int f = fcntl(fd, F_GETFD, 0);
    if (f >= 0) fcntl(fd, F_SETFD, f | FD_CLOEXEC);
}

int net_set_nonblock(int fd, int on)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    if (on) fl |= O_NONBLOCK; else fl &= ~O_NONBLOCK;
    return fcntl(fd, F_SETFL, fl);
}

int net_set_nodelay(int fd)
{
    int one = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

/* H1/H2: bounded socket I/O for accepted control connections. Without these,
 * a client that connects and then goes silent (or stops reading) parks the
 * per-connection thread forever in recv()/send(), pinning one of the few
 * client slots until process restart (trivial slot-exhaustion DoS). Legit
 * clients read/write continuously and never hit these. 0 = leave unset. */
int net_set_timeouts(int fd, int rcv_s, int snd_s)
{
    struct timeval tv;
    int rc = 0;
    if (rcv_s > 0) {
        tv.tv_sec = rcv_s; tv.tv_usec = 0;
        rc |= setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    }
    if (snd_s > 0) {
        tv.tv_sec = snd_s; tv.tv_usec = 0;
        rc |= setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    }
    return rc;
}

int net_listen_tcp(int port, int backlog)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr*)&sa, sizeof sa) < 0) { close(fd); return -1; }
    if (listen(fd, backlog) < 0) { close(fd); return -1; }
    net_cloexec(fd);
    return fd;
}

int net_udp_socket(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd >= 0) net_cloexec(fd);
    return fd;
}

int net_sendall(int fd, const void *buf, int len)
{
    const uint8_t *p = (const uint8_t*)buf;
    int off = 0;
    while (off < len) {
        int n = send(fd, p+off, len-off, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && (errno==EINTR)) continue;
            return -1;
        }
        off += n;
    }
    return off;
}

int net_bind_udp_pair(int *rtp_fd, int *rtcp_fd, int base_port)
{
    int r = net_udp_socket(), c = net_udp_socket();
    if (r<0||c<0){ if(r>=0)close(r); if(c>=0)close(c); return -1; }
    struct sockaddr_in sa; memset(&sa,0,sizeof sa);
    sa.sin_family=AF_INET; sa.sin_addr.s_addr=htonl(INADDR_ANY);
    sa.sin_port=htons((uint16_t)base_port);
    if (bind(r,(struct sockaddr*)&sa,sizeof sa)<0){ close(r);close(c); return -1; }
    sa.sin_port=htons((uint16_t)(base_port+1));
    if (bind(c,(struct sockaddr*)&sa,sizeof sa)<0){ close(r);close(c); return -1; }
    *rtp_fd=r; *rtcp_fd=c;
    return 0;
}
