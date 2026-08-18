/* util.h - common small helpers, byte writers, time */
#ifndef MS_UTIL_H
#define MS_UTIL_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

/* monotonic microseconds.
 *
 * MS_CLOCK_SCALE (sim builds ONLY - never define for a target build): an
 * opt-in whole-process virtual clock for the day/night replay harness
 * (scripts/dn-replay.py, design-notes section 6 step 2). A dawn incident
 * spans hours; -DMS_CLOCK_SCALE=30 makes virtual time run 30x real time so
 * a 4 h scenario replays in 8 min. This function is the single monotonic
 * chokepoint the whole daemon reads, so multiplying here scales EVERY
 * deadline, dwell, hysteresis window and EMA schedule coherently; the
 * matching division in ms_stopgate_wait() (util.c) keeps the periodic
 * threads ticking at their configured VIRTUAL cadence (500 virtual ms =
 * 500/SCALE real ms), so the NUMBER of samples per window - what the
 * smoothing/stability logic actually depends on - is exactly preserved.
 * That is why no DN_* compile-time constant and no config timing needs
 * compressing (the 2026-08-14 verification compressed config timings 15x by
 * hand instead, which distorts tick counts and was adequate only for a
 * targeted check). Pass via the sim target's SIM_CFLAGS hook:
 *   make sim SIM_CFLAGS="-DMS_CLOCK_SCALE=30" */
static inline int64_t ms_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t us = (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
#ifdef MS_CLOCK_SCALE
    us *= (int64_t)(MS_CLOCK_SCALE);
#endif
    return us;
}

/* big-endian writers used by RTP and MP4 muxers */
static inline void wr_be16(uint8_t *p, uint16_t v){ p[0]=v>>8; p[1]=v; }
static inline void wr_be24(uint8_t *p, uint32_t v){ p[0]=v>>16; p[1]=v>>8; p[2]=v; }
static inline void wr_be32(uint8_t *p, uint32_t v){ p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }
static inline void wr_be64(uint8_t *p, uint64_t v){ for(int i=0;i<8;i++) p[i]=(uint8_t)(v>>(56-8*i)); }


/* growable byte buffer (used to assemble MP4 boxes) */
typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
    int      err;   /* sticky: set once any ms_buf_* call fails to grow the
                      * buffer (OOM). Once set, box_close()/fragment() must
                      * not patch size/offset fields into it - the buffer's
                      * content is short some bytes, so `pos` no longer
                      * points at what the caller thinks it does. */
} ms_buf;

int  ms_buf_init(ms_buf *b, size_t cap);
int  ms_buf_reserve(ms_buf *b, size_t extra);
int  ms_buf_put(ms_buf *b, const void *src, size_t n);
int  ms_buf_u8(ms_buf *b, uint8_t v);
int  ms_buf_be16(ms_buf *b, uint16_t v);
int  ms_buf_be32(ms_buf *b, uint32_t v);
void ms_buf_free(ms_buf *b);
/* reuse a persistent buffer: len=0, err=0, and shrink the backing store back to
 * `soft` if a rare huge frame grew it past that, so per-connection/-recorder
 * buffers don't stay ballooned. Normal frames fit under `soft` -> no realloc. */
void ms_buf_reset(ms_buf *b, size_t soft);

/* base64 encode; returns bytes written (excludes NUL). dst must hold
 * >= ((n+2)/3)*4 + 1 bytes. */
int ms_base64(char *dst, const uint8_t *src, int n);

/* ---- thread creation with an explicit stack size (S3) --------------------
 * Without an attribute, uClibc-ng NPTL sizes every thread stack from
 * RLIMIT_STACK (typically 8 MB of VA each). Measured stack peaks across the
 * daemon's threads are ~16 KB, so give each thread type an explicit, still
 * generous budget instead. Note the stack also hosts the static TLS block
 * (__thread data, ~8 KB worst case here) and a guard page.
 *
 *   MS_STACK_UTIL   accept loops and light periodic workers (timelapse,
 *                   daynight): small locals, no codecs, no vendor libs.
 *   MS_STACK_STREAM encoder/mux/vendor-lib threads (video/JPEG/audio/OSD/
 *                   motion/rotate/record/SRT): IMP SDK + faac calls; raptor
 *                   runs the same vendor SDK on 128 KB stacks in production.
 *   MS_STACK_CONN   per-connection and codec-heavy threads (RTSP/HTTP
 *                   clients with an in-thread mbedTLS handshake, speaker
 *                   playback with libopus decode, whose VAR_ARRAYS build
 *                   can use tens of KB of stack).
 */
#define MS_STACK_UTIL   (64 * 1024)
#define MS_STACK_STREAM (128 * 1024)
#define MS_STACK_CONN   (256 * 1024)

#include <pthread.h>
/* pthread_create with an explicit stack size; falls back to default
 * attributes if the sized create fails. Returns 0 on success (like
 * pthread_create). Detach handling stays with the caller. */
int ms_thread_create(pthread_t *t, size_t stack, void *(*fn)(void *), void *arg);

/* ---- stop gate (P-02) -----------------------------------------------------
 * A one-shot "sleep until the next interval OR until stop is requested"
 * primitive for the daemon's periodic worker threads. It replaces the old
 * slice-sleeps (usleep in 100-300 ms chunks, re-checking a stop flag each
 * chunk) that woke ~25x/s in aggregate purely to stay responsive to shutdown.
 * A worker now blocks on a CLOCK_MONOTONIC pthread_cond_timedwait and wakes at
 * most ONCE per real interval, or immediately when ms_stopgate_stop() is
 * called - same stop latency, far fewer idle wakeups.
 *
 * Shutdown safety: stop() takes the SAME mutex the waiter evaluates its
 * predicate under, sets stop=1, then broadcasts - so a stop requested BEFORE
 * the waiter blocks is seen by the pre-wait predicate check (no wait entered),
 * and a stop requested WHILE blocked is delivered by the broadcast. There is
 * no window in which a stop can be missed, so a worker can never sleep past a
 * shutdown request. Same clock as fanqueue/events so an NTP wall-clock step
 * cannot stretch the wait. */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    int             stop;
} ms_stopgate;

/* Initialise (stop cleared). Safe to call again on restart to re-arm. */
void ms_stopgate_init(ms_stopgate *g);
/* Block up to ms milliseconds, returning early the moment stop is requested.
 * Returns 1 if stop has been requested (caller should exit its loop), else 0
 * (the interval elapsed). A already-stopped gate returns 1 without waiting. */
int  ms_stopgate_wait(ms_stopgate *g, int ms);
/* Request stop and wake the waiter immediately. Idempotent. */
void ms_stopgate_stop(ms_stopgate *g);
/* Non-blocking predicate read (1 if stop requested). */
int  ms_stopgate_stopped(ms_stopgate *g);

/* Escape a string for embedding between JSON double quotes: escapes " and \\,
 * folds control characters to spaces, and replaces invalid UTF-8 so strict
 * parsers do not reject the document. THE one escaper - GET /control, the POST
 * reply's "applied" echo and the SSE config stream all go through it, because a
 * second copy is a second thing to get wrong. Bounds-checked; truncates rather
 * than overflowing out[cap]. */
void ms_json_esc(const char *s, char *out, size_t cap);

/* ---- shared media-tree filesystem helpers (record.c + timelapse.c) --------
 * One copy on purpose: the '..' check below is a SECURITY check and used to
 * exist as word-identical twins in record.c and timelapse.c - a hardening had
 * to land twice, and whoever found only one copy thought the job done. */

/* Path-traversal check, COMPONENT semantics: flags ".." only as a whole path
 * component ("..", "../x", "x/../y", "x/.."), never as a substring of a longer
 * name. This meaning won over the older strstr(s,"..") substring test (which
 * record_clip() and control.c's sound_path() used) for two reasons:
 *   - L-2: legitimate strftime name patterns may contain ".." inside a
 *     component (e.g. "cam..front-%Y") and must not be rejected;
 *   - it is NOT weaker: pathname resolution only ever walks upward on an
 *     exact ".." component - ".." inside a longer component names a literal
 *     file. Symlink escapes are a separate concern, handled at the call sites
 *     that need it (lstat in the pruners, O_NOFOLLOW in record_clip()).
 * Every '..' check in the daemon goes through this one function, so all paths
 * provably share one meaning. */
int ms_has_dotdot(const char *s);

/* dir/name here are runtime-mutable via /control by an authenticated caller;
 * a ".." component (or an absolute name spliced into the path) would let a
 * writer/pruner escape its media tree (L10). name==NULL checks dir only. */
int ms_path_unsafe(const char *dir, const char *name);

/* free space on the filesystem holding dir, in MB; -1 on error */
long long ms_free_mb(const char *dir);

/* create every parent directory of a file path (mkdir -p on dirname) */
void ms_mkdirs(const char *path);

/* F4: gethostname() may fail (fall back to a safe default) and on an overlong
 * hostname is not guaranteed to NUL-terminate - force both. */
void ms_hostname(char *out, size_t cap);

/* Build <dir>/<hostname>/<sub>/<strftime(name)><ext> into out - the shared
 * "where does this segment/shot go" builder for record.c and timelapse.c.
 * Caller must have vetted dir/name with ms_path_unsafe() first; this only
 * builds. strftime() returning 0 (overflow OR a legitimately empty expansion)
 * falls back to the epoch seconds so the file still gets a usable name.
 * Returns the wall time the name was built from (timelapse keeps it as the
 * last-shot timestamp). */
time_t ms_media_path(char *out, size_t cap, const char *dir, const char *sub,
                     const char *name, const char *ext);

#endif
