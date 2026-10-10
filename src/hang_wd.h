#ifndef MS_HANG_WD_H
#define MS_HANG_WD_H
/* Request threads stamp a slot while inside code that reaches libimp; the
 * main loop's 1 s tick (control_tick) escalates when one is held too long. A
 * wedged IMP call cannot be interrupted, only the process restarted. Pure;
 * scripts/test_hang_wd.c. A stamp is seconds + 1, so 0 means free. */

#define HWD_SLOTS 20                /* HTTP_MAX_CLIENTS + daynight + spare */

typedef struct { volatile int t0[HWD_SLOTS]; } hwd;

/* slot, or -1 when all are taken (then that request is not watched) */
static inline int hwd_begin(hwd *w, int now_s)
{
    for (int i = 0; i < HWD_SLOTS; i++)
        if (__sync_bool_compare_and_swap(&w->t0[i], 0, now_s + 1)) return i;
    return -1;
}

static inline void hwd_end(hwd *w, int slot)
{
    if (slot >= 0 && slot < HWD_SLOTS) __sync_lock_release(&w->t0[slot]);
}

/* how long the oldest stamp has been held, in seconds */
static inline int hwd_oldest(const hwd *w, int now_s)
{
    int age = 0;
    for (int i = 0; i < HWD_SLOTS; i++) {
        int t = w->t0[i];
        if (t && now_s + 1 - t > age) age = now_s + 1 - t;
    }
    return age;
}

#endif
