/* osd_vars.h - expand OSD text placeholders.
 *
 * Two stages:
 *   1) {name} tokens are replaced. Built-ins: {hostname} {ip} {mac} {fps}
 *      {fpsN} {bitrate} {bitrateN} {uptime}. {fps}/{bitrate} are the measured
 *      output fps/kbit/s of osd.monitor_stream; {fpsN}/{bitrateN} (e.g.
 *      {fps0}, {bitrate1}) are those of video stream N specifically - the
 *      per-layer form, since every stream has its own OSD text but the
 *      no-number pair reads one configured channel for all of them.
 *      Any other {name} is looked up in a key=value file at a fixed path
 *      (OSD_VARS_FILE in osd_vars.c, /tmp/timps_osd.vars) so scripts can
 *      inject arbitrary values. The path is not configurable - it used to be
 *      (osd.vars_file), but a POSTable or even file-only-but-settable path
 *      let it be pointed at timps.conf itself, rendering credentials onto
 *      the video through a {placeholder}.
 *   2) The result is passed through strftime(), so %Y %m %d %H %M %S %F %T ...
 *      render the current time.
 *
 * Portable (no SDK) and unit-testable on the host. */
#ifndef MS_OSD_VARS_H
#define MS_OSD_VARS_H

void osd_vars_set_fps(double fps);
void osd_vars_set_bitrate(double kbps);   /* live stream bitrate, kbit/s */
int  osd_expand(const char *tmpl, char *out, int outsz);

#endif
