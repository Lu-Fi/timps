/* test_dn_idle.c - host-only test for the Idle-Dark decision (src/dn_idle.h)
 * and the stop gate's kick (src/util.c), which wakes the day/night thread
 * when a consumer arrives. Time is virtual: every step passes now_ms.
 * Build/run: make test-dn-idle */
#include <stdio.h>
#include <string.h>
#include "dn_idle.h"
#include "util.h"

static int failures, checks;

#define CK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    printf("  FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define TICK 2000   /* daynight.interval_ms default */

/* booted night camera, feature on, everything available */
static dni_in base(void)
{
    dni_in in;
    memset(&in, 0, sizeof in);
    in.cfg = 1; in.linger_s = 60; in.enabled = 1; in.auto_mode = 1; in.keepalive = 1;
    in.hook = DNI_HOOK_OK; in.booted = 1; in.night = 1;
    in.mode_since_ms = -100000;
    in.day_gain = 768.0f; in.night_gain = 4096.0f;
    in.s = 20000.0f; in.stable = 1; in.fresh = 1;
    return in;
}

static int step(dni_state *st, dni_in *in, int64_t t, int *why)
{
    int w;
    in->now_ms = t;
    int a = dni_step(st, in, why ? why : &w);
    in->kicked = 0;
    return a;
}

/* run ticks over [from, to), consumers from cons(t); count LED actions */
typedef int (*cons_fn)(int64_t t);
static void run(dni_state *st, dni_in *in, int64_t from, int64_t to,
                cons_fn cons, int *offs, int *ons)
{
    for (int64_t t = from; t < to; t += TICK) {
        in->consumers = cons ? cons(t) : 0;
        int a = step(st, in, t, NULL);
        if (a == DNI_LED_OFF && offs) (*offs)++;
        if ((a == DNI_LED_ON || a == DNI_WAKE_DAY) && ons) (*ons)++;
    }
}

static int until100(int64_t t) { return t < 100000; }

static void t_linger_expiry(void)
{
    dni_state st = {0}; dni_in in = base(); int why;
    run(&st, &in, 0, 100000, until100, NULL, NULL);
    CK(!st.dark && st.watched_until == 98000 + 60000, "watched_until %lld", (long long)st.watched_until);
    in.consumers = 0;
    CK(step(&st, &in, 100000, &why) == DNI_NONE, "linger running");
    CK(dni_linger_left_s(&st, &in) == 58, "linger left %d", dni_linger_left_s(&st, &in));
    CK(step(&st, &in, 157998, &why) == DNI_NONE, "1 ms before expiry");
    CK(step(&st, &in, 158000, &why) == DNI_LED_OFF && why == DNI_R_IDLE, "LED off at expiry");
    CK(st.dark && st.dark_at == 158000, "dark");
    CK(step(&st, &in, 160000, &why) == DNI_NONE, "no repeat");
}

static void t_linger_zero(void)
{
    dni_state st = {0}; dni_in in = base(); int why;
    in.linger_s = 0; in.consumers = 1;
    CK(step(&st, &in, 0, &why) == DNI_NONE, "watched");
    in.consumers = 0;
    CK(step(&st, &in, TICK, &why) == DNI_LED_OFF, "linger 0: off on the next tick");
}

static int poll10(int64_t t)  { return (t % 10000) < 2000; }
static int poll120(int64_t t) { return (t % 120000) < 2000; }

static void t_flapping(void)
{
    dni_state st = {0}; dni_in in = base();
    int offs = 0, ons = 0;
    run(&st, &in, 0, 3600000, poll10, &offs, &ons);
    CK(offs == 0 && ons == 0 && !st.dark, "10 s poll keeps the LED on (%d off, %d on)", offs, ons);

    dni_state s2 = {0}; dni_in i2 = base();
    offs = ons = 0;
    run(&s2, &i2, 0, 3600000, poll120, &offs, &ons);
    /* 30 polls; after each one the linger runs out, idle (1 off), then the
     * fast re-asserts (2 more offs) land before the next poll wakes it */
    CK(ons == 29, "one wake per poll after the first: %d", ons);
    CK(offs == 30 * (1 + DN_IR_IDLE_REFAST_N), "offs %d", offs);
}

static void t_kick_between_ticks(void)
{
    dni_state st = {0}; dni_in in = base(); int why;
    CK(step(&st, &in, 0, &why) == DNI_LED_OFF, "idle at once (never watched)");
    in.kicked = 1; in.consumers = 0;    /* a snapshot that already left */
    CK(step(&st, &in, 30000, &why) == DNI_LED_ON, "a kick alone wakes");
    CK(st.watched_until == 90000, "and starts the linger");
    CK(step(&st, &in, 32000, &why) == DNI_NONE, "lingers");
    CK(step(&st, &in, 90000, &why) == DNI_LED_OFF, "off after the linger");
}

static int always(int64_t t) { (void)t; return 1; }

static void t_pinned(void)
{
    dni_state st = {0}; dni_in in = base();
    int offs = 0, ons = 0;
    run(&st, &in, 0, 86400000, always, &offs, &ons);
    CK(offs == 0 && ons == 0, "motion pin / recorder: LED never touched");
}

static void t_gates(void)
{
    struct { const char *n; void (*f)(dni_in *); int want; } g[] = {
        { "cfg off",        NULL, DNI_U_DISABLED },
        { "dn disabled",    NULL, DNI_U_DISABLED },
        { "no keepalive",   NULL, DNI_U_KEEPALIVE },
        { "no illuminator", NULL, DNI_U_NO_ILLUM },
        { "old hook",       NULL, DNI_U_HOOK },
        { "white light",    NULL, DNI_U_WHITE },
        { "schedule mode",  NULL, DNI_U_SCHEDULE },
    };
    for (unsigned i = 0; i < sizeof g / sizeof g[0]; i++) {
        dni_state st = {0}; dni_in in = base();
        switch (i) {
        case 0: in.cfg = 0; break;
        case 1: in.enabled = 0; break;
        case 2: in.keepalive = 0; break;
        case 3: in.hook = DNI_HOOK_NONE; break;
        case 4: in.hook = DNI_HOOK_OLD; break;
        case 5: in.hook = DNI_HOOK_WHITE; break;
        case 6: in.auto_mode = 0; break;
        }
        CK(dni_gate(&in) == g[i].want, "%s: gate %d", g[i].n, dni_gate(&in));
        int offs = 0, ons = 0;
        run(&st, &in, 0, 7200000, NULL, &offs, &ons);
        CK(offs == 0 && ons == 0 && !st.dark, "%s: never idle", g[i].n);
    }
    dni_in in = base();
    CK(dni_gate(&in) == DNI_OK && !strcmp(dni_unavail_str(DNI_OK), ""), "available");
    CK(!strcmp(dni_unavail_str(DNI_U_KEEPALIVE), "keepalive"), "reason string");
    CK(!strcmp(dni_unavail_str(DNI_U_SCHEDULE), "schedule"), "schedule reason");
}

static void t_not_while(void)
{
    int why;
    dni_state st = {0}; dni_in in = base();
    in.night = 0;
    CK(step(&st, &in, 0, &why) == DNI_NONE, "day: LED is off anyway");
    in = base(); in.booted = 0;
    CK(step(&st, &in, 0, &why) == DNI_NONE, "not before boot");
    in = base(); in.busy = 1;
    CK(step(&st, &in, 0, &why) == DNI_NONE, "not mid-probe");
    in = base(); in.no_entry = 1;
    CK(step(&st, &in, 0, &why) == DNI_NONE, "not on a kick wake / while suspended");
    in = base(); in.mode_since_ms = -(DN_TRANSITION_S * 1000 - 1);
    CK(step(&st, &in, 0, &why) == DNI_NONE, "not inside the dwell");
    in.mode_since_ms = -(DN_TRANSITION_S * 1000);
    CK(step(&st, &in, 0, &why) == DNI_LED_OFF, "after the dwell");
}

static void t_handback(void)
{
    int why;
    dni_state st = {0}; dni_in in = base();
    CK(step(&st, &in, 0, &why) == DNI_LED_OFF, "dark");
    in.cfg = 0;     /* POST daynight.ir_idle_off=0 */
    CK(step(&st, &in, TICK, &why) == DNI_LED_ON && why == DNI_R_HANDBACK, "cfg off hands back");
    CK(step(&st, &in, 2 * TICK, &why) == DNI_NONE, "once");

    in = base();
    CK(step(&st, &in, 0, &why) == DNI_LED_OFF, "dark again");
    in.enabled = 0; in.night = 0;   /* manual mode resets the decided mode */
    CK(step(&st, &in, TICK, &why) == DNI_LED_ON, "disable hands back");

    in = base();
    CK(step(&st, &in, 0, &why) == DNI_LED_OFF, "dark again");
    CK(dni_handback(&st) == DNI_LED_ON && !st.dark, "stop hands back");
    CK(dni_handback(&st) == DNI_NONE, "nothing left to hand back");

    in = base();
    CK(step(&st, &in, 0, &why) == DNI_LED_OFF, "dark again");
    in.night = 0;   /* the noir day rule switched to day */
    CK(step(&st, &in, TICK, &why) == DNI_NONE && !st.dark, "day: no relight");
}

static void t_first_consumer(void)
{
    int why;
    dni_state st = {0}; dni_in in = base();
    CK(step(&st, &in, 0, &why) == DNI_LED_OFF, "dark");
    in.consumers = 1;
    CK(step(&st, &in, 60000, &why) == DNI_LED_ON && why == DNI_R_WAKE, "consumer lights at once");
    CK(st.ready_at == 60000 + DN_IR_WAKE_SETTLE_MS, "ready_at %lld", (long long)st.ready_at);
    CK(step(&st, &in, 62000, &why) == DNI_NONE, "stays on while watched");
}

static void t_wake_verdicts(void)
{
    int why;
    dni_state st = {0}; dni_in in = base();
    in.day_rule = 1;
    step(&st, &in, 0, &why);
    in.consumers = 1;
    CK(step(&st, &in, 3000, &why) == DNI_LED_ON && why == DNI_R_WAKE_UNSETTLED,
       "3 s after LED off: unsettled, night, LED on");

    st = (dni_state){0}; in = base(); in.day_rule = 1; in.s = 300.0f;
    step(&st, &in, 0, &why);
    in.consumers = 1;
    CK(step(&st, &in, 3000, &why) == DNI_LED_ON, "bright but unsettled: no day verdict");

    st = (dni_state){0}; in = base(); in.day_rule = 1; in.s = 300.0f;
    step(&st, &in, 0, &why);
    in.consumers = 1;
    CK(step(&st, &in, 20000, &why) == DNI_WAKE_DAY && !st.dark, "settled bright noir: day");

    st = (dni_state){0}; in = base(); in.day_rule = 0; in.s = 300.0f;
    step(&st, &in, 0, &why);
    in.consumers = 1;
    CK(step(&st, &in, 20000, &why) == DNI_LED_ON, "day rule off (schedule / before C5): LED on");

    st = (dni_state){0}; in = base(); in.day_rule = 1; in.s = 300.0f; in.noir_cost = 20.0f;
    step(&st, &in, 0, &why);
    in.consumers = 1;
    CK(step(&st, &in, 20000, &why) == DNI_LED_ON, "veto: 300 x 20 > night_gain");

    st = (dni_state){0}; in = base(); in.day_rule = 1; in.s = 300.0f; in.fresh = 0;
    step(&st, &in, 0, &why);
    in.consumers = 1;
    CK(step(&st, &in, 20000, &why) == DNI_LED_ON, "frozen AE: no day verdict");

    st = (dni_state){0}; in = base(); in.day_rule = 1; in.s = 1500.0f;
    step(&st, &in, 0, &why);
    in.consumers = 1;
    CK(step(&st, &in, 20000, &why) == DNI_LED_ON, "twilight band: night, LED on");
}

static void t_reassert(void)
{
    int why;
    dni_state st = {0}; dni_in in = base();
    step(&st, &in, 0, &why);
    CK(step(&st, &in, 7999, &why) == DNI_NONE, "nothing before +8 s");
    CK(step(&st, &in, 8000, &why) == DNI_LED_OFF && why == DNI_R_REASSERT, "+8 s");
    CK(step(&st, &in, 16000, &why) == DNI_LED_OFF, "+16 s");
    CK(step(&st, &in, 24000, &why) == DNI_NONE, "fast series done");
    CK(step(&st, &in, 16000 + DN_IR_IDLE_REASSERT_S * 1000 - 1, &why) == DNI_NONE, "slow not yet");
    CK(step(&st, &in, 16000 + DN_IR_IDLE_REASSERT_S * 1000, &why) == DNI_LED_OFF, "slow re-assert");
}

static void t_switch_while_idle(void)
{
    int why;
    dni_state st = {0}; dni_in in = base();
    step(&st, &in, 0, &why);
    dni_note_switch(&st);           /* e.g. a re-assert of night via switch_cmd */
    in.mode_since_ms = 10000;
    CK(step(&st, &in, 12000, &why) == DNI_NONE, "the script lit it; wait the dwell");
    CK(step(&st, &in, 15000, &why) == DNI_LED_OFF && why == DNI_R_IDLE, "dark again after the dwell");
}

static void t_boot_and_restart(void)
{
    int why;
    dni_state st = {0}; dni_in in = base();
    in.booted = 0; in.mode_since_ms = 0;
    int offs = 0, ons = 0;
    run(&st, &in, 0, 20000, NULL, &offs, &ons);
    CK(offs == 0 && ons == 0, "nothing while booting");
    in.booted = 1; in.mode_since_ms = 20000;
    CK(step(&st, &in, 22000, &why) == DNI_NONE, "dwell after the boot assertion");
    CK(step(&st, &in, 25000, &why) == DNI_LED_OFF, "no consumer since boot: idle without a linger");

    /* restart: the LED may still be off, the new process does not know */
    dni_state fresh = {0}; dni_in r = base();
    r.booted = 0; r.consumers = 1;
    offs = ons = 0;
    run(&fresh, &r, 0, 20000, always, &offs, &ons);
    CK(offs == 0 && ons == 0, "no blind LED action after a restart (the boot assertion lights)");
    r.booted = 1; r.mode_since_ms = 20000; r.consumers = 0;
    CK(step(&fresh, &r, 26000, &why) == DNI_NONE, "linger from the watched boot");
    CK(step(&fresh, &r, 18000 + 60000, &why) == DNI_LED_OFF, "re-entry after the linger");
}

/* a kick wake has no fresh sample: it may wake, never enter */
static void t_kick_wake(void)
{
    int why;
    dni_state st = {0}; dni_in in = base();
    CK(step(&st, &in, 0, &why) == DNI_LED_OFF, "dark");
    in.no_entry = 1; in.kicked = 1;
    CK(step(&st, &in, 30500, &why) == DNI_LED_ON && why == DNI_R_WAKE, "kick wake lights");
    in.kicked = 0;
    CK(step(&st, &in, 200000, &why) == DNI_NONE, "kick wake past the linger: no entry");
    in.no_entry = 0;
    CK(step(&st, &in, 200100, &why) == DNI_LED_OFF, "the next tick enters");
}

/* the boot probe that proved night is the entry: dark since it began */
static void t_enter_from_boot(void)
{
    int why;
    dni_state st = {0}; dni_in in = base();
    dni_enter(&st, 2000, 10000);
    CK(st.dark && st.dark_at == 2000 && st.refast_left == DN_IR_IDLE_REFAST_N - 1,
       "entered, fast re-asserts armed");
    CK(st.reassert_at == 10000 + DN_IR_IDLE_REFAST_MS, "first re-assert from the verdict");
    in.consumers = 1;
    CK(step(&st, &in, 12000, &why) == DNI_LED_ON && why == DNI_R_WAKE,
       "noir valid 10 s after the LED went off");
}

static void t_stopgate_kick(void)
{
    ms_stopgate g;
    ms_stopgate_init(&g);
    int64_t t0 = ms_now_us();
    ms_stopgate_kick(&g);
    CK(ms_stopgate_wait(&g, 30) == 0, "plain wait ignores a kick");
    CK(ms_now_us() - t0 >= 25000, "and sleeps its interval");
    t0 = ms_now_us();
    CK(ms_stopgate_wait_kick(&g, 5000) == 2, "a pending kick ends wait_kick");
    CK(ms_now_us() - t0 < 1000000, "at once");
    CK(ms_stopgate_wait_kick(&g, 20) == 0, "the kick was consumed");
    ms_stopgate_kick(&g);
    ms_stopgate_stop(&g);
    CK(ms_stopgate_wait_kick(&g, 5000) == 1, "stop wins over a kick");
    CK(ms_stopgate_wait(&g, 5000) == 1, "stopped gate");
}

int main(void)
{
    t_linger_expiry();
    t_linger_zero();
    t_flapping();
    t_kick_between_ticks();
    t_pinned();
    t_gates();
    t_not_while();
    t_handback();
    t_first_consumer();
    t_wake_verdicts();
    t_reassert();
    t_switch_while_idle();
    t_boot_and_restart();
    t_kick_wake();
    t_enter_from_boot();
    t_stopgate_kick();
    printf("dn_idle: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
