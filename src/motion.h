#ifndef TIMPS_MOTION_H
#define TIMPS_MOTION_H

/*
 * motion.h — motion detection via IMP_IVS.
 *
 * When motion is detected the optional user script is executed (if configured).
 */

/* Initialise and start motion detection. */
int  motion_init(void);

/* Stop and destroy motion detection. */
void motion_exit(void);

/* Returns 1 if motion was detected since the last call. */
int  motion_triggered(void);

#endif /* TIMPS_MOTION_H */
