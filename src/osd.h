#ifndef TIMPS_OSD_H
#define TIMPS_OSD_H

/*
 * osd.h — On-Screen Display using IMP_OSD.
 *
 * Creates a text OSD region on both enabled streams.
 * The label template supports strftime(3) specifiers plus:
 *   %{fps}      current encode FPS
 *   %{bitrate}  current bitrate (kbps)
 *   %{viewers}  active viewer count
 *   %{uptime}   camera uptime h:mm:ss
 *   %{host}     hostname
 */

/* Initialise OSD (call after imp_init, before encoder_start). */
int  osd_init(void);

/* Update OSD text (call periodically, e.g. every second). */
void osd_update(void);

/* Destroy OSD resources. */
void osd_exit(void);

#endif /* TIMPS_OSD_H */
