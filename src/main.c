/* main.c - timps (Tiny IMP Streamer): minimal-dependency RTSP + fMP4 streamer for Ingenic SoCs */
#include "config.h"
#include "log.h"
#include "hub.h"
#include "hal/hal.h"
#include "rtsp/rtsp.h"
#include "mp4/httpd.h"
#include "record.h"
#include "timelapse.h"
#include "srt.h"
#ifdef USE_DAYNIGHT
#include "daynight.h"
#endif
#ifdef USE_CONTROL
#include "auth.h"
#include <fcntl.h>
#include <sys/stat.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>

#define MOD "MAIN"
#ifndef MS_VERSION
#define MS_VERSION "0.1.0"
#endif

static volatile int g_run = 1;
static const hal_backend *g_hal;

static void hard_exit(int s){ (void)s; _exit(0); }
static void on_signal(int s)
{
    (void)s;
    g_run = 0;
    /* guarantee the process actually stops even if vendor teardown stalls:
     * a second Ctrl-C, or 3 s without a clean exit, forces termination. */
    signal(SIGINT,  hard_exit);
    signal(SIGTERM, hard_exit);
    signal(SIGALRM, hard_exit);
    alarm(3);
}
static void idr_trampoline(int src){ if (g_hal && g_hal->request_idr) g_hal->request_idr(src); }
static void act_trampoline(int src, int on){ if (g_hal && g_hal->set_active) g_hal->set_active(src, on); }

#ifdef USE_CONTROL
/* Publish the per-boot /control token for local privileged readers (the
 * thingino WebUI reads it server-side and hands it only to authenticated
 * sessions). Rewritten on every start; fchmod pins the mode to 0640
 * regardless of the process umask. Only the random per-boot token goes
 * here - a configured http.token secret is NEVER written to disk. */
static void write_token_file(const ms_config *cfg)
{
    if (!cfg->http_token_file[0]) return;            /* "" = disabled */
    int fd = open(cfg->http_token_file, O_CREAT|O_WRONLY|O_TRUNC, 0640);
    if (fd < 0){ LOGW(MOD,"cannot write token file %s", cfg->http_token_file); return; }
    fchmod(fd, 0640);
    size_t l = strlen(g_ctl_token);
    if (write(fd, g_ctl_token, l) != (ssize_t)l || write(fd, "\n", 1) != 1)
        LOGW(MOD,"short write on token file %s", cfg->http_token_file);
    else
        LOGI(MOD,"/control token published to %s", cfg->http_token_file);
    close(fd);
}
#endif

int main(int argc, char **argv)
{
    const char *cfgpath = "/etc/timps.conf";
    for (int i=1;i<argc;i++){
        if (!strcmp(argv[i],"-c") && i+1<argc) cfgpath=argv[++i];
        else if (!strcmp(argv[i],"-v")) log_set_level(LOG_DEBUG);
        else if (!strcmp(argv[i],"-h")){
            printf("timps %s\nusage: %s [-c config] [-v]\n",MS_VERSION,argv[0]);
            return 0;
        }
    }

    config_load(&g_cfg, cfgpath);
    config_sensor_finalize(&g_cfg);   /* auto-detect sensor from /proc/jz/sensor */
    srand((unsigned)(time(NULL) ^ getpid()));   /* rtsp.c session IDs / UDP port picks */
    LOGI(MOD,"timps %s starting (backend=%s)", MS_VERSION, hal_get()->name);

#ifdef USE_CONTROL
    /* per-boot /control token: valid alongside Basic auth (httpd.c) */
    auth_gen_token(g_ctl_token);
    if (g_cfg.http_enabled) write_token_file(&g_cfg);
#endif

    hub_init();
    hub_set_idr_cb(idr_trampoline);
    hub_set_activity_cb(act_trampoline);

    g_hal = hal_get();
    if (g_hal->init(&g_cfg)!=0){ LOGE(MOD,"HAL init failed"); return 1; }
    if (g_hal->start(&g_cfg)!=0){ LOGE(MOD,"HAL start failed"); return 1; }

    rtsp_server *rtsp = NULL;
    httpd       *http = NULL;
    if (g_cfg.rtsp_enabled) rtsp = rtsp_start(&g_cfg);
    if (g_cfg.http_enabled) http = httpd_start(&g_cfg);
#ifdef USE_DAYNIGHT
    daynight_start();
#endif
    record_start(&g_cfg);
    timelapse_start(&g_cfg);
    srt_start(&g_cfg);

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    LOGI(MOD,"running. rtsp://<ip>:%d%s  http://<ip>:%d/",
         g_cfg.rtsp_port, g_cfg.video[0].rtsp_path, g_cfg.http_port);

    while (g_run) sleep(1);

    LOGI(MOD,"shutting down");
    srt_stop();
    timelapse_stop();
    record_stop();
#ifdef USE_DAYNIGHT
    daynight_stop();
#endif
    if (rtsp) rtsp_stop(rtsp);
    if (http) httpd_stop(http);
    g_hal->stop();
    return 0;
}
