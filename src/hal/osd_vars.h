/* osd_vars.h - expand OSD text placeholders.
 *
 * Two stages:
 *   1) {name} tokens are replaced. Built-ins: {hostname} {ip} {mac} {fps}
 *      {uptime}. Any other {name} is looked up in a key=value file (vars_file,
 *      e.g. /tmp/timps_osd.vars) so scripts can inject arbitrary values.
 *   2) The result is passed through strftime(), so %Y %m %d %H %M %S %F %T ...
 *      render the current time.
 *
 * Portable (no SDK) and unit-testable on the host. */
#ifndef MS_OSD_VARS_H
#define MS_OSD_VARS_H

void osd_vars_set_fps(double fps);
int  osd_expand(const char *tmpl, const char *vars_file, char *out, int outsz);

#endif
