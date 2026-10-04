#include "clients.h"
#include "log.h"
#include "util.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

/* An id is slot | generation << CL_SLOT_BITS: a straggling clients_bytes()
 * from a thread that already called clients_del() then misses instead of
 * crediting whoever reused the slot. */
#define CL_SLOT_BITS 8
typedef char cl_slots_fit[CLIENTS_MAX <= (1 << CL_SLOT_BITS) ? 1 : -1];

typedef struct cl_entry cl_entry;
struct cl_entry {
    int                used;
    volatile unsigned  gen;       /* 0 = free */
    int                kind, chn;
    struct sockaddr_in peer;
    int64_t            since_us;
    /* 64-bit total as two words (MIPS32 has no 64-bit store); only the rare
     * carry is guarded by seq, so the per-send cost stays one add */
    volatile uint32_t  lo, hi, seq;
    uint64_t           q_bytes;   /* rate baseline, advanced by clients_json */
    volatile int32_t   lat_us;    /* EWMA, -1 = no video frame measured */
    volatile uint32_t  drops;
    int64_t            q_us;
    unsigned           kbps;
    char               agent[CLIENTS_AGENT_MAX];
};

static uint64_t cl_total(const cl_entry *e);

static cl_entry        g_cl[CLIENTS_MAX];
static pthread_mutex_t g_cl_mx = PTHREAD_MUTEX_INITIALIZER;
static unsigned        g_gen;     /* under g_cl_mx */

static cl_entry *cl_get(int id)
{
    if (id < 0) return NULL;
    unsigned slot = (unsigned)id & ((1u << CL_SLOT_BITS) - 1);
    if (slot >= CLIENTS_MAX) return NULL;
    cl_entry *e = &g_cl[slot];
    return e->gen == (unsigned)id >> CL_SLOT_BITS ? e : NULL;
}

static const char *const KIND[] = {
    "rtsp/udp", "rtsp/tcp", "rtsps", "fmp4", "mjpeg", "events", "webrtc", "srt",
};

#define MOD "CLIENT"
#define KNAME(k) ((k) >= 0 && (k) < (int)(sizeof KIND / sizeof KIND[0]) ? KIND[k] : "?")
/* every WebUI tab holds a few /events streams; keep those out of the INFO log */
#define CL_LOG(k, ...) log_printf((k) == CLI_EVENTS ? LOG_DEBUG : LOG_INFO, MOD, __VA_ARGS__)

static const char *cl_ip(const cl_entry *e, char *buf, int cap)
{
    if (!inet_ntop(AF_INET, &e->peer.sin_addr, buf, (socklen_t)cap)) snprintf(buf, (size_t)cap, "?");
    return buf;
}

void clients_agent_from(const char *hdrs, char *out, int cap)
{
    if (!out || cap <= 0) return;
    out[0] = 0;
    for (const char *p = hdrs; p && *p; ) {
        if (!strncasecmp(p, "User-Agent:", 11)) {
            p += 11;
            while (*p == ' ' || *p == '\t') p++;
            int n = 0;
            /* it is logged verbatim: no terminal escapes or other control
             * bytes from a client into anyone's log viewer */
            while (p[n] && p[n] != '\r' && p[n] != '\n' && n < cap - 1) {
                unsigned char ch = (unsigned char)p[n];
                out[n] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '?';
                n++;
            }
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
            e->lat_us = -1;
            e->used = 1;
            g_gen = (g_gen + 1) & ((1u << (31 - CL_SLOT_BITS)) - 1);
            if (!g_gen) g_gen = 1;
            e->gen = g_gen;
            id = (int)(g_gen << CL_SLOT_BITS | (unsigned)i);
            break;
        }
    pthread_mutex_unlock(&g_cl_mx);
    char ip[INET_ADDRSTRLEN];
    if (id >= 0) {
        const cl_entry *e = &g_cl[id & ((1 << CL_SLOT_BITS) - 1)];
        CL_LOG(kind, "+ %s %s:%u chn=%d agent=\"%s\"", KNAME(kind), cl_ip(e, ip, sizeof ip),
               (unsigned)ntohs(e->peer.sin_port), chn, e->agent);
    }
    else
        LOGW(MOD, "table full, %s client not listed", KNAME(kind));
    return id;
}

void clients_del(int id)
{
    cl_entry *e = cl_get(id);
    if (!e) return;
    char ip[INET_ADDRSTRLEN];
    CL_LOG(e->kind, "- %s %s:%u after %llds, %llu bytes", KNAME(e->kind), cl_ip(e, ip, sizeof ip),
           (unsigned)ntohs(e->peer.sin_port), (long long)((ms_now_us() - e->since_us) / 1000000),
           (unsigned long long)cl_total(e));
    pthread_mutex_lock(&g_cl_mx);
    e->used = 0;
    e->gen = 0;
    pthread_mutex_unlock(&g_cl_mx);
}

void clients_bytes(int id, int n)
{
    cl_entry *e = cl_get(id);
    if (!e || n <= 0) return;
    uint32_t lo = e->lo + (uint32_t)n;
    if (lo >= e->lo) { e->lo = lo; return; }
    e->seq++;
    __sync_synchronize();
    e->lo = lo;
    e->hi++;
    __sync_synchronize();
    e->seq++;
}

void clients_latency(int id, int64_t us)
{
    cl_entry *e = cl_get(id);
    if (!e || us < 0 || us > 10000000) return;
    int32_t v = (int32_t)us, o = e->lat_us;
    e->lat_us = o < 0 ? v : o + (v - o) / 8;
}

void clients_drops(int id, unsigned n)
{
    cl_entry *e = cl_get(id);
    if (e) e->drops += n;
}

static uint64_t cl_total(const cl_entry *e)
{
    uint32_t s, lo, hi;
    do {
        s = e->seq;
        __sync_synchronize();
        lo = e->lo;
        hi = e->hi;
        __sync_synchronize();
    } while ((s & 1) || s != e->seq);
    return (uint64_t)hi << 32 | lo;
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
        uint64_t b = cl_total(e);
        int64_t dt = now - e->q_us;
        if (dt >= 1000000) {
            e->kbps = (unsigned)((b - e->q_bytes) * 8000 / (uint64_t)dt);
            e->q_bytes = b;
            e->q_us = now;
        }
        char ip[INET_ADDRSTRLEN];
        cl_ip(e, ip, sizeof ip);
        char ag[CLIENTS_AGENT_MAX * 2];
        ms_json_esc(e->agent, ag, sizeof ag);
        len += snprintf(out + len, (size_t)(cap - len),
            "%s{\"ip\":\"%s\",\"port\":%u,\"proto\":\"%s\",\"chn\":%d,"
            "\"since_s\":%lld,\"kbps\":%u,\"bytes\":%llu,\"lat_ms\":%d,\"drops\":%u,\"agent\":\"%s\"}",
            first ? "" : ",", ip, (unsigned)ntohs(e->peer.sin_port),
            KNAME(e->kind),
            e->chn, (long long)((now - e->since_us) / 1000000), e->kbps,
            (unsigned long long)b, e->lat_us < 0 ? -1 : (int)((e->lat_us + 500) / 1000),
            (unsigned)e->drops, ag);
        first = 0;
    }
    pthread_mutex_unlock(&g_cl_mx);
    if (len < cap) len += snprintf(out + len, (size_t)(cap - len), "]}");
    return len < cap ? len : -1;
}
