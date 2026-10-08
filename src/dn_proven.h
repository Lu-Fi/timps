#ifndef MS_DN_PROVEN_H
#define MS_DN_PROVEN_H
/* The proven-night gate on a silent probe's "the room supplies the light".
 * r <= 2 only says the illuminator is not what lights the NIGHT pipeline;
 * light the IR-cut filter removes (another camera's illuminator in the same
 * cellar) reads the same, while the day pipeline stays dark. Wyze Pan v1,
 * 2026-10-07: day pipeline 3587 (night), 28 s later lit 401 / dark 483,
 * r 1.20, day. So a night that a measurement proved stays night while the lit
 * level is not measurably brighter than its reference. Pure, so
 * scripts/test_dn_proven.c runs the code the thread runs. */

static inline int dn_proven_dark(int proven, float ref, float lit, float margin)
{
    return proven && ref > 0.0f && lit > 0.0f && !(lit < ref * margin);
}
#endif
