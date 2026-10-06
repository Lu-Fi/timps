#ifndef MS_DN_IDLE_H
#define MS_DN_IDLE_H
/* Idle-Dark (daynight.ir_idle_off): at night, while nobody needs a picture,
 * the IR illuminator goes off (IR-cut and ISP stay in night) and the AE keeps
 * measuring ambient light under the kept-alive FrameSource; the first
 * consumer is answered from that measurement instead of a probe. Pure (clock
 * and inputs injected, the caller runs the hooks and logs) so
 * scripts/test_dn_idle.c runs the code the day/night thread runs. */
#include <stdint.h>
#include "daynight.h"   /* DN_PROBE_SETTLE_S, DN_TRANSITION_S */

#define DN_IR_WAKE_SETTLE_MS   3500   /* AE after "IR on"; the snapshot hold */
#define DN_IR_IDLE_REASSERT_S   600   /* a WebUI/cron "daynight night" relights behind our back */
#define DN_IR_IDLE_REFAST_MS   8000   /* the board script lights in the background, possibly late */
#define DN_IR_IDLE_REFAST_N       2
#define DN_IR_WAKE_HOOK_MS     1000   /* budget for the "on" hook before ready_at is known */

enum { DNI_NONE = 0, DNI_LED_OFF, DNI_LED_ON, DNI_WAKE_DAY };
enum { DNI_R_NONE = 0, DNI_R_IDLE, DNI_R_REASSERT, DNI_R_WAKE,
       DNI_R_WAKE_UNSETTLED, DNI_R_HANDBACK };
enum { DNI_OK = 0, DNI_U_DISABLED, DNI_U_KEEPALIVE, DNI_U_NO_ILLUM,
       DNI_U_WHITE, DNI_U_HOOK, DNI_U_SCHEDULE };
/* what the illuminator hook can do */
enum { DNI_HOOK_OK = 0, DNI_HOOK_NONE, DNI_HOOK_OLD, DNI_HOOK_WHITE };

typedef struct {
    int64_t now_ms;
    int     cfg;            /* daynight.ir_idle_off */
    int     linger_s;       /* daynight.ir_idle_linger_s */
    int     enabled;        /* daynight.enabled */
    int     auto_mode;      /* daynight.mode = auto */
    int     keepalive;      /* hal_fs_keepalive() */
    int     hook;           /* DNI_HOOK_* */
    int     booted;
    int     night;          /* the decided mode is night */
    int64_t mode_since_ms;
    int     busy;           /* probe, verify or enforce cycle in flight */
    int     no_entry;       /* no fresh lit reading (a kick wake) or entry suspended */
    int     day_rule;       /* a wake may be answered with day from noir */
    int     consumers;      /* hub_image_subs() + hal_image_pinned() */
    int     kicked;         /* a consumer arrived since the last step */
    float   s;              /* smoothed exposure index */
    int     stable;         /* stable_n reached */
    int     fresh;          /* the AE moved since the LED went off */
    float   day_gain, night_gain;
    float   noir_cost;      /* learnt day-optics/noir factor, <= 0 unknown */
} dni_in;

typedef struct {
    int     dark;           /* the LED is off because of us */
    int64_t dark_at;
    int64_t watched_until;  /* end of the linger */
    int64_t reassert_at;
    int     refast_left;
    int64_t ready_at;       /* first usable picture after a wake */
} dni_state;

static inline int dni_gate(const dni_in *in)
{
    if (!in->cfg || !in->enabled) return DNI_U_DISABLED;
    if (!in->auto_mode) return DNI_U_SCHEDULE;
    if (!in->keepalive) return DNI_U_KEEPALIVE;
    switch (in->hook) {
    case DNI_HOOK_OK:    return DNI_OK;
    case DNI_HOOK_WHITE: return DNI_U_WHITE;
    case DNI_HOOK_OLD:   return DNI_U_HOOK;
    default:             return DNI_U_NO_ILLUM;
    }
}

/* a kick counts: a snapshot can come and go between two ticks */
static inline int dni_watch(dni_state *st, const dni_in *in)
{
    int w = in->consumers > 0 || in->kicked;
    if (w) st->watched_until = in->now_ms + (int64_t)in->linger_s * 1000;
    return w;
}

static inline int dni_linger_left_s(const dni_state *st, const dni_in *in)
{
    int64_t l = st->watched_until - in->now_ms;
    return l > 0 ? (int)((l + 999) / 1000) : 0;
}

static inline int dni_noir_valid(const dni_state *st, const dni_in *in)
{
    return st->dark && in->now_ms - st->dark_at >= (int64_t)DN_PROBE_SETTLE_S * 1000 &&
           in->stable && in->fresh && in->s > 0.0f;
}

/* noir is optimistic (IR-cut open): clearly day only below day_gain and not
 * vetoed by what the day optics cost in this scene before */
static inline int dni_noir_day(const dni_state *st, const dni_in *in)
{
    return dni_noir_valid(st, in) && in->s < in->day_gain &&
           !(in->noir_cost > 0.0f && in->s * in->noir_cost > in->night_gain);
}

static inline void dni_arm_reassert(dni_state *st, int64_t now)
{
    if (st->refast_left > 0) { st->refast_left--; st->reassert_at = now + DN_IR_IDLE_REFAST_MS; }
    else st->reassert_at = now + (int64_t)DN_IR_IDLE_REASSERT_S * 1000;
}

/* the LED went off for idle at dark_at */
static inline void dni_enter(dni_state *st, int64_t dark_at, int64_t now)
{
    st->dark = 1;
    st->dark_at = dark_at;
    st->refast_left = DN_IR_IDLE_REFAST_N;
    dni_arm_reassert(st, now);
}

/* One decision per tick or kick. DNI_WAKE_DAY leaves the LED off: the caller
 * confirms the LED state and switches to day, or lights it itself. */
static inline int dni_step(dni_state *st, const dni_in *in, int *why)
{
    int64_t now = in->now_ms;
    int watched = dni_watch(st, in);
    *why = DNI_R_NONE;
    if (dni_gate(in) != DNI_OK) {
        if (!st->dark) return DNI_NONE;
        st->dark = 0;
        *why = DNI_R_HANDBACK;
        return DNI_LED_ON;
    }
    if (!in->night) {           /* a day switch has turned it off for good */
        st->dark = 0;
        return DNI_NONE;
    }
    if (st->dark) {
        if (watched) {
            int valid = dni_noir_valid(st, in);
            int day = in->day_rule && dni_noir_day(st, in);
            st->dark = 0;
            st->ready_at = now + DN_IR_WAKE_SETTLE_MS;
            *why = valid ? DNI_R_WAKE : DNI_R_WAKE_UNSETTLED;
            return day ? DNI_WAKE_DAY : DNI_LED_ON;
        }
        if (now >= st->reassert_at) {
            dni_arm_reassert(st, now);
            *why = DNI_R_REASSERT;
            return DNI_LED_OFF;
        }
        return DNI_NONE;
    }
    if (watched || now < st->watched_until || !in->booted || in->busy ||
        in->no_entry || now - in->mode_since_ms < (int64_t)DN_TRANSITION_S * 1000)
        return DNI_NONE;
    dni_enter(st, now, now);
    *why = DNI_R_IDLE;
    return DNI_LED_OFF;
}

/* every switch_cmd: the board script drives the LED itself */
static inline void dni_note_switch(dni_state *st) { st->dark = 0; }

/* stop or a forced reset: give back what we took */
static inline int dni_handback(dni_state *st)
{
    int was = st->dark;
    st->dark = 0;
    return was ? DNI_LED_ON : DNI_NONE;
}

static inline const char *dni_unavail_str(int r)
{
    switch (r) {
    case DNI_U_DISABLED:  return "disabled";
    case DNI_U_KEEPALIVE: return "keepalive";
    case DNI_U_NO_ILLUM:  return "no_illuminator";
    case DNI_U_WHITE:     return "white_light";
    case DNI_U_HOOK:      return "hook";
    case DNI_U_SCHEDULE:  return "schedule";
    default:              return "";
    }
}

static inline const char *dni_why_str(int w)
{
    switch (w) {
    case DNI_R_IDLE:           return "idle";
    case DNI_R_REASSERT:       return "reassert";
    case DNI_R_WAKE:           return "wake";
    case DNI_R_WAKE_UNSETTLED: return "wake (noir unsettled)";
    case DNI_R_HANDBACK:       return "handback";
    default:                   return "";
    }
}

#endif
