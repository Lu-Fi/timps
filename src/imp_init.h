#ifndef TIMPS_IMP_INIT_H
#define TIMPS_IMP_INIT_H

/*
 * imp_init.h — Ingenic libimp system, ISP and FrameSource initialisation.
 *
 * Wraps IMP_System, IMP_ISP and IMP_FrameSource APIs.
 * Must be called once before any encoder or OSD is created.
 */

/* Initialise IMP system, ISP, sensor and frame-source channels.
 * Returns 0 on success, negative on error. */
int imp_init(void);

/* Tear down everything created by imp_init(). */
void imp_exit(void);

#endif /* TIMPS_IMP_INIT_H */
