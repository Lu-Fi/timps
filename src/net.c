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

/* accept() a TCP client with FD_CLOEXEC set (F2): the listeners and UDP media
 * sockets above are already close-on-exec, but plain accept() does not inherit
 * the flag, so board scripts forked by daynight/motion kept copies of every
 * live RTSP/HTTP client socket (a closed client then stayed half-open, no FIN,
 * for the script's lifetime). accept4(SOCK_CLOEXEC) sets it atomically - no
 * window for another thread's fork() to slip between accept() and fcntl() -
 * with a plain accept()+fcntl() fallback for kernels without accept4. */
int net_accept_cloexec(int lfd, struct sockaddr *sa, socklen_t *sl)
{
    int fd = accept4(lfd, sa, sl, SOCK_CLOEXEC);
    if (fd < 0 && (errno == ENOSYS || errno == EINVAL)) {
        fd = accept(lfd, sa, sl);
        if (fd >= 0) net_cloexec(fd);
    }
    return fd;
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

int net_sendmsg_all(int fd, struct iovec *iov, int niov)
{
    int total = 0;
    for (int i = 0; i < niov; i++) total += (int)iov[i].iov_len;
    int done = 0;
    while (done < total) {
        struct msghdr m;
        memset(&m, 0, sizeof m);
        m.msg_iov    = iov;
        /* no cast: msg_iovlen is size_t on glibc, int on uClibc-ng */
        m.msg_iovlen = niov;
        /* MSG_NOSIGNAL like net_sendall(): main.c ignores SIGPIPE globally, but
         * this helper must not depend on that staying true. */
        ssize_t n = sendmsg(fd, &m, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;   /* incl. SO_SNDTIMEO expiry: caller treats a short
                          * write as a torn frame and stops the session */
        }
        done += (int)n;
        /* Consume the accepted bytes off the FRONT of the iovec array: whole
         * entries drop out, the one straddling the boundary is advanced. */
        size_t left = (size_t)n;
        while (left > 0 && niov > 0) {
            if (left >= iov->iov_len) {
                left -= iov->iov_len;
                iov++; niov--;
            } else {
                iov->iov_base = (uint8_t *)iov->iov_base + left;
                iov->iov_len -= left;
                left = 0;
            }
        }
    }
    return total;
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
