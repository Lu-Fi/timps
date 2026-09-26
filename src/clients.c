#include "clients.h"
#include "util.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

typedef struct {
    int                used;
    int                kind, chn;
    struct sockaddr_in peer;
    int64_t            since_us;
    /* 32 bits on purpose: MIPS32 has no native 64-bit store, and a rate only
     * needs the difference, which survives the wrap (~4 GB). */
    volatile uint32_t  bytes;
    uint32_t           q_bytes;   /* rate baseline, advanced by clients_json */
    int64_t            q_us;
    unsigned           kbps;
    char               agent[CLIENTS_AGENT_MAX];
} cl_entry;

static cl_entry        g_cl[CLIENTS_MAX];
static pthread_mutex_t g_cl_mx = PTHREAD_MUTEX_INITIALIZER;

static const char *const KIND[] = {
    "rtsp/udp", "rtsp/tcp", "rtsps", "fmp4", "mjpeg", "events", "webrtc", "srt",
};

void clients_agent_from(const char *hdrs, char *out, int cap)
{
    if (!out || cap <= 0) return;
    out[0] = 0;
    for (const char *p = hdrs; p && *p; ) {
        if (!strncasecmp(p, "User-Agent:", 11)) {
            p += 11;
            while (*p == ' ' || *p == '\t') p++;
            int n = 0;
            while (p[n] && p[n] != '\r' && p[n] != '\n' && n < cap - 1) { out[n] = p[n]; n++; }
            out[n] = 0;
            return;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
}

int clients_add(int kind, const struct sockaddr_in *peer, int chn, const char *agent)
{
    int id = -1;
    int64_t now = ms_now_us();
    pthread_mutex_lock(&g_cl_mx);
    for (int i = 0; i < CLIENTS_MAX; i++)
        if (!g_cl[i].used) {
            cl_entry *e = &g_cl[i];
            memset(e, 0, sizeof *e);
            e->kind = kind;
            e->chn = chn;
            if (peer) e->peer = *peer;
            if (agent) {
                strncpy(e->agent, agent, sizeof e->agent - 1);
                e->agent[sizeof e->agent - 1] = 0;
            }
            e->since_us = e->q_us = now;
            e->used = 1;
            id = i;
            break;
        }
    pthread_mutex_unlock(&g_cl_mx);
    return id;
}

void clients_del(int id)
{
    if (id < 0 || id >= CLIENTS_MAX) return;
    pthread_mutex_lock(&g_cl_mx);
    g_cl[id].used = 0;
    pthread_mutex_unlock(&g_cl_mx);
}

void clients_bytes(int id, int n)
{
    if (id >= 0 && id < CLIENTS_MAX && n > 0) g_cl[id].bytes += (uint32_t)n;
}

int clients_json(char *out, int cap)
{
    int len = snprintf(out, (size_t)cap, "{\"clients\":[");
    int64_t now = ms_now_us();
    int first = 1;
    pthread_mutex_lock(&g_cl_mx);
    for (int i = 0; i < CLIENTS_MAX && len < cap; i++) {
        cl_entry *e = &g_cl[i];
        if (!e->used) continue;
        /* rate over at least 1 s, so several pollers can't shrink the window
         * to noise; the first read averages since the connection began */
        uint32_t b = e->bytes;
        int64_t dt = now - e->q_us;
        if (dt >= 1000000) {
            e->kbps = (unsigned)((uint64_t)(uint32_t)(b - e->q_bytes) * 8000 / (uint64_t)dt);
            e->q_bytes = b;
            e->q_us = now;
        }
        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &e->peer.sin_addr, ip, sizeof ip);
        char ag[CLIENTS_AGENT_MAX * 2];
        ms_json_esc(e->agent, ag, sizeof ag);
        len += snprintf(out + len, (size_t)(cap - len),
            "%s{\"ip\":\"%s\",\"port\":%u,\"proto\":\"%s\",\"chn\":%d,"
            "\"since_s\":%lld,\"kbps\":%u,\"agent\":\"%s\"}",
            first ? "" : ",", ip, (unsigned)ntohs(e->peer.sin_port),
            (e->kind >= 0 && e->kind < (int)(sizeof KIND / sizeof KIND[0])) ? KIND[e->kind] : "?",
            e->chn, (long long)((now - e->since_us) / 1000000), e->kbps, ag);
        first = 0;
    }
    pthread_mutex_unlock(&g_cl_mx);
    if (len < cap) len += snprintf(out + len, (size_t)(cap - len), "]}");
    return len < cap ? len : -1;
}
