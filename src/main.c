/* main.c - timps (Tiny IMP Streamer): minimal-dependency RTSP + fMP4 streamer for Ingenic SoCs */
#include "config.h"
#include "log.h"
#include "hub.h"
#include "hal/hal.h"
#include "rtsp/rtsp.h"
#include "rtsp/backchannel.h"
#include "rtsp/speaker.h"
#include "mp4/httpd.h"
#include "record.h"
#include "timelapse.h"
#include "srt.h"
#ifdef USE_DAYNIGHT
#include "daynight.h"
#endif
#ifdef USE_CONTROL
#include "auth.h"
#include <sys/stat.h>
#endif
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/file.h>

#define MOD "MAIN"
#ifndef MS_VERSION
#define MS_VERSION "0.1.0"
#endif

static volatile int g_run = 1;
static const hal_backend *g_hal;

/* Single-instance guard against concurrent ISP/HAL access (root cause of a
 * real incident: a manually-launched foreground /tmp/timpsd test build was
 * still running when `/etc/init.d/S95timps restart` fired in another shell.
 * busybox's start-stop-daemon -S -x /usr/bin/timpsd matches "already running"
 * by executable PATH, so it never saw the /tmp/timpsd process and happily
 * launched a second /usr/bin/timpsd. That second process's IMP_ISP_Open/
 * sensor-init sequence reset the shared ISP kernel driver state out from
 * under the first, still-running process, destroying its FrameSource
 * channels - which the first process then could not recover from (see the
 * video/jpeg watchdog escalation in hal_ingenic.c, added to at least detect
 * and exit that unrecoverable state instead of retrying it forever).
 * flock() closes the actual race instead of just detecting its aftermath:
 * whichever process loses never gets far enough to call IMP_ISP_Open in the
 * first place. Matches the flat-file-in-/run convention already used for
 * http_token_file ("/run/timps.token" in config.c) rather than the /run/
 * timps/ subdirectory speaker.c mkdir()s for the audio FIFO - a lock file
 * needs no subdirectory and must work before any other subsystem has set
 * one up. */
#define MS_LOCK_PATH "/run/timps.lock"
static int g_lockfd = -1;   /* held for the process lifetime; never closed
                             * while running - flock() releases on exit/crash */
static int acquire_singleton_lock(void)
{
    int fd = open(MS_LOCK_PATH, O_CREAT|O_RDWR, 0644);
    if (fd < 0){
        /* Can't even open it (e.g. /run not yet mounted read-write) - warn
         * but don't block startup over the guard itself; this is best-effort
         * hardening, not a hard dependency of every boot path. */
        LOGW(MOD,"cannot open %s (%s) - proceeding without the single-"
             "instance guard", MS_LOCK_PATH, strerror(errno));
        return 0;
    }
    if (flock(fd, LOCK_EX|LOCK_NB) != 0){
        LOGE(MOD,"another timpsd instance already holds %s - refusing to "
             "start (a second instance re-initializing the ISP would "
             "corrupt the running instance's video pipeline)", MS_LOCK_PATH);
        close(fd);
        return -1;
    }
    g_lockfd = fd;
    return 0;
}

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
/* rand() seed for the remaining non-secret rand() users (UDP port picks
 * etc.). Seeding from time^pid made every rand()-derived value guessable
 * from the approximate boot time; pull the seed from /dev/urandom instead
 * (same source auth_gen_token uses). The weak mix stays only as a last
 * resort when /dev/urandom is unavailable. */
static unsigned rand_seed(void)
{
    unsigned s = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t r = read(fd, &s, sizeof s);
        close(fd);
        if (r == (ssize_t)sizeof s) return s;
    }
    return (unsigned)(time(NULL) ^ getpid());
}
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
    /* Freeze the boot view of the config BEFORE the HAL/servers (and thus any
     * /control thread) run. Restart-only videoN.* fields (codec/enabled/dims/
     * fps/bitrate/rotation) are read from g_cfg_boot by the streaming consumers
     * so a live /control edit persists for next boot without desyncing the
     * running encoder from new RTSP/fMP4/SRT/record sessions. See config.h. */
    config_snapshot_boot();
    srand(rand_seed());               /* non-secret rand() users (UDP port picks) */
    g_hal = hal_get();
    if (!g_hal){ LOGE(MOD,"no HAL backend available"); return 1; }   /* F-08 */
    LOGI(MOD,"timps %s starting (backend=%s)", MS_VERSION, g_hal->name);

    /* Must run BEFORE any ISP/HAL init below - see acquire_singleton_lock's
     * comment for why. A lost race means an already-live instance owns the
     * hardware; touching it further (even just to log more) risks nothing
     * extra, but there's also nothing useful left to do, so exit immediately. */
    if (acquire_singleton_lock() != 0) return 1;

#ifdef USE_CONTROL
    /* per-boot /control token: valid alongside Basic auth (httpd.c) */
    auth_gen_token(g_ctl_token);
    if (g_cfg.http_enabled) write_token_file(&g_cfg);
#endif

    /* Install the shutdown handlers BEFORE the HAL/IMP bring-up: a SIGINT/
     * SIGTERM during the (potentially slow) init used to abort the process
     * with no HAL teardown at all. With the handlers in place, an interrupt
     * during init just clears g_run - init/start complete, the main loop is
     * skipped and the normal orderly teardown below runs (the handler's 3 s
     * alarm still force-exits if a vendor call wedges). */
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    hub_init();
    hub_set_idr_cb(idr_trampoline);
    hub_set_activity_cb(act_trampoline);

    if (g_hal->init(&g_cfg)!=0){ LOGE(MOD,"HAL init failed"); return 1; }
    if (g_hal->start(&g_cfg)!=0){ LOGE(MOD,"HAL start failed"); return 1; }

#if defined(USE_BACKCHANNEL) || defined(USE_PLAY)
    {
        int spk_rate = 16000;   /* default AO rate for a play-only build */
#ifdef USE_BACKCHANNEL
        if (g_cfg.audio.backchannel){
            bc_configure(g_cfg.audio.backchannel_codec, g_cfg.audio.backchannel_rate);
            spk_rate = g_cfg.audio.backchannel_rate;
            LOGI(MOD,"audio backchannel enabled (codec=%d rate=%d, native IMP_AO)",
                 g_cfg.audio.backchannel_codec, g_cfg.audio.backchannel_rate);
        }
#endif
        speaker_configure(spk_rate);
#ifdef USE_PLAY
        speaker_start();   /* /run/timps/audio_out play-FIFO thread */
#endif
    }
#endif
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
#ifdef USE_PLAY
    speaker_stop();
#endif
    g_hal->stop();
    return 0;
}
