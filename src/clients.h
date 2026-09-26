#ifndef MS_CLIENTS_H
#define MS_CLIENTS_H
/* Who is streaming what: one entry per streaming consumer (RTSP session,
 * fMP4 / MJPEG / SSE connection, WebRTC session, SRT receiver) with its peer
 * address, kind, source channel, start time and a byte counter. Served as
 * GET /control?clients=1. One-shot requests (/control, snapshots) are not
 * listed.
 *
 * Cost on the hot path is one plain add per send: each entry is written only
 * by the thread that owns it, and the rate is derived when the list is read. */

#include <netinet/in.h>

enum {
    CLI_RTSP_UDP, CLI_RTSP_TCP, CLI_RTSPS,
    CLI_FMP4, CLI_MJPEG, CLI_EVENTS,
    CLI_WEBRTC, CLI_SRT,
};

/* RTSP 8 + HTTP 16 + WebRTC 4 + SRT 8; a full table only drops the listing */
#ifndef CLIENTS_MAX
#define CLIENTS_MAX 40
#endif

/* browsers put their name after ~100 chars of Mozilla/AppleWebKit boilerplate */
#define CLIENTS_AGENT_MAX 160
/* worst case for clients_json(): every agent fully escaped */
#define CLIENTS_JSON_CAP (CLIENTS_MAX * (CLIENTS_AGENT_MAX * 2 + 160) + 32)

/* returns an id for clients_bytes()/clients_del(), or -1 (table full).
 * agent: the client's User-Agent (or NULL), truncated to CLIENTS_AGENT_MAX-1 */
int  clients_add(int kind, const struct sockaddr_in *peer, int chn, const char *agent);
void clients_del(int id);
/* owner thread only; id -1 is a no-op */
void clients_bytes(int id, int n);
/* copy the User-Agent value out of a raw request header block ("" if none) */
void clients_agent_from(const char *hdrs, char *out, int cap);
/* {"clients":[{"ip","port","proto","chn","since_s","kbps","bytes","agent"},..]};
 * -1 if cap is too small */
int  clients_json(char *out, int cap);

#endif
