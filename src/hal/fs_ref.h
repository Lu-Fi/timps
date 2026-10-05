#ifndef MS_FS_REF_H
#define MS_FS_REF_H
/* FrameSource enable refcount and the chn0 latch-kick hold. Pure (IMP calls
 * injected, caller holds the lock, caller logs) so the host test
 * (scripts/test_fs_ref.c) runs the code the HAL runs. */

#define FSR_MAXCHN 8

typedef struct {
    int (*enable)(int chn);     /* 0 = success */
    int (*disable)(int chn);
} fsr_ops;

typedef struct {
    int users[FSR_MAXCHN];
    int enabled[FSR_MAXCHN];    /* EnableChn succeeded, independent of users */
    int kick_held;              /* one chn0 reference owned by the latch kick */
    long long kick_until;       /* us; the kick reference is released after */
} fsr_state;

enum { FSR_FAIL = -1, FSR_BUMP = 0, FSR_EDGE = 1 };

/* retry the hardware enable independent of the refcount transition */
static inline int fsr_try_enable(fsr_state *s, const fsr_ops *o, int chn)
{
    if (s->enabled[chn]) return FSR_BUMP;
    if (o->enable(chn) != 0) return FSR_FAIL;
    s->enabled[chn] = 1;
    return FSR_EDGE;
}

static inline int fsr_use(fsr_state *s, const fsr_ops *o, int chn)
{
    if (chn < 0 || chn >= FSR_MAXCHN) return FSR_BUMP;
    s->users[chn]++;
    return fsr_try_enable(s, o, chn);
}

/* 1 = this call disabled the channel */
static inline int fsr_unuse(fsr_state *s, const fsr_ops *o, int chn)
{
    if (chn < 0 || chn >= FSR_MAXCHN) return 0;
    if (s->users[chn] > 0 && --s->users[chn] == 0 && s->enabled[chn]) {
        o->disable(chn);
        s->enabled[chn] = 0;
        return 1;
    }
    return 0;
}

/* caller checks enabled[] first; returns the EnableChn rc */
static inline int fsr_recycle(fsr_state *s, const fsr_ops *o, int chn)
{
    o->disable(chn);
    int rc = o->enable(chn);
    s->enabled[chn] = (rc == 0);
    return rc;
}

/* Latch kick on chn0. FSR_EDGE: chn0 was just enabled, the caller lets it run
 * kick_us and then calls fsr_kick_end(). Otherwise chn0 already runs and the
 * hold only keeps it running another idle_us - no edge, no wait. The hold is
 * dropped by fsr_kick_reap() like an idle encoder, never right after the wait:
 * an immediate disable is what met the next wake within milliseconds. */
static inline int fsr_kick_begin(fsr_state *s, const fsr_ops *o, long long now,
                                 long long kick_us, long long idle_us)
{
    int r;
    if (s->kick_held) r = fsr_try_enable(s, o, 0);
    else { s->kick_held = 1; r = fsr_use(s, o, 0); }
    long long until = now + (r == FSR_EDGE ? kick_us : 0) + idle_us;
    if (until > s->kick_until) s->kick_until = until;
    return r;
}

static inline void fsr_kick_end(fsr_state *s, long long now, long long idle_us)
{
    if (s->kick_held && now + idle_us > s->kick_until)
        s->kick_until = now + idle_us;
}

/* 1 = releasing the hold disabled chn0 */
static inline int fsr_kick_reap(fsr_state *s, const fsr_ops *o, long long now)
{
    if (!s->kick_held || now < s->kick_until) return 0;
    s->kick_held = 0;
    s->kick_until = 0;
    return fsr_unuse(s, o, 0);
}

/* Hard stop at teardown. The kick reference is ours and goes first; *leaked
 * is what any other holder still has. Returns the DisableChn rc, 0 when the
 * channel was already off. */
static inline int fsr_teardown(fsr_state *s, const fsr_ops *o, int chn, int *leaked)
{
    *leaked = 0;
    if (chn < 0 || chn >= FSR_MAXCHN) return 0;
    if (chn == 0 && s->kick_held) {
        s->kick_held = 0; s->kick_until = 0;
        if (s->users[0] > 0) s->users[0]--;
    }
    *leaked = s->users[chn];
    int rc = 0;
    if (s->enabled[chn]) { rc = o->disable(chn); s->enabled[chn] = 0; }
    s->users[chn] = 0;
    return rc;
}

#endif
