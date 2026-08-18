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
#include <ucontext.h>

#define MOD "MAIN"
#ifndef MS_VERSION
#define MS_VERSION "0.1.0"
#endif

static volatile int g_run = 1;
static const hal_backend *g_hal;

/* ---- fatal-signal handler (SIGSEGV/SIGBUS/SIGFPE/SIGABRT) --------------
 * timpsd links closed-source Ingenic vendor libraries (libimp.so for video/
 * ISP/encoder, libaudioProcess.so for audio) that have documented crash
 * modes elsewhere in this file's own hardening comments: a UAF in
 * libaudioProcess.so on channel-teardown races (hal_ingenic.c ~line 2645), a
 * div-by-zero SIGFPE from a bad encoder QP (~line 2030), and others. When one
 * of those fires it takes the whole process down with no supervisor and no
 * core dumps practical on this embedded target - the only diagnostic we get
 * is whatever we capture ourselves, right here, before the process dies.
 *
 * Hard constraints: this runs inside a signal handler for a signal that may
 * mean the stack/heap are already corrupt, so it must be async-signal-safe -
 * no malloc, no LOGx (log.c's log_printf() does vsnprintf + a pthread mutex +
 * syslog(), none of which are safe here) and no libc string functions whose
 * signal-safety isn't guaranteed on this uClibc target. Everything below is
 * hand-rolled on top of open/read/write/close/signal/raise/sigprocmask only -
 * all POSIX async-signal-safe. sigaltstack() below means this still runs even
 * on a stack-overflow SIGSEGV. */
#define MS_CRASH_LOG_PATH "/run/timps.crash"
#define MS_ALTSTACK_SIZE  (64*1024)
static unsigned char g_altstack[MS_ALTSTACK_SIZE];
static char g_maps_buf[16384];   /* static: no stack/heap use in the handler */

static size_t crash_strlen(const char *s){ size_t n=0; while(s[n]) n++; return n; }
static void   crash_write(int fd, const char *s){ if (fd>=0){ ssize_t r = write(fd, s, crash_strlen(s)); (void)r; } }

static void crash_write_hex(int fd, unsigned long v)
{
    static const char hexd[] = "0123456789abcdef";
    char buf[2+sizeof(unsigned long)*2+1];
    buf[0]='0'; buf[1]='x';
    int ndig = (int)sizeof(unsigned long)*2;
    for (int i=0;i<ndig;i++)
        buf[2+i] = hexd[(v >> ((ndig-1-i)*4)) & 0xf];
    buf[2+ndig]=0;
    crash_write(fd, buf);
}

/* pure/stateless substring search - no libc dependency, safe to call here */
static int crash_mem_contains(const char *hay, size_t haylen, const char *needle)
{
    size_t nlen = crash_strlen(needle);
    if (nlen==0 || nlen>haylen) return 0;
    for (size_t i=0;i+nlen<=haylen;i++){
        size_t j=0;
        while (j<nlen && hay[i+j]==needle[j]) j++;
        if (j==nlen) return 1;
    }
    return 0;
}

/* Reads /proc/self/maps once, writes the raw content to crashfd (best-effort
 * post-mortem detail) and returns a static string classifying which mapping
 * `addr` falls in - the single most useful fact for triage: did this die in
 * a closed-source vendor blob (libimp.so/libaudioProcess.so) or in timps's
 * own code. */
static const char *crash_classify_addr(int crashfd, unsigned long addr)
{
    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd < 0) return "(/proc/self/maps unavailable)";
    ssize_t n = read(fd, g_maps_buf, sizeof g_maps_buf - 1);
    close(fd);
    if (n <= 0) return "(/proc/self/maps empty/unreadable)";
    g_maps_buf[n] = 0;

    crash_write(crashfd, "--- /proc/self/maps ---\n");
    crash_write(crashfd, g_maps_buf);
    crash_write(crashfd, "--- end maps ---\n");

    const char *p = g_maps_buf, *end = g_maps_buf + n;
    while (p < end){
        const char *eol = p;
        while (eol < end && *eol != '\n') eol++;
        unsigned long start=0, stop=0;
        const char *q = p;
        while (q < eol && *q != '-'){
            int c=*q, d = (c>='0'&&c<='9')?c-'0':(c>='a'&&c<='f')?c-'a'+10:-1;
            if (d<0) break;
            start = (start<<4)|(unsigned)d; q++;
        }
        if (q < eol && *q=='-'){
            q++;
            while (q < eol && *q != ' '){
                int c=*q, d = (c>='0'&&c<='9')?c-'0':(c>='a'&&c<='f')?c-'a'+10:-1;
                if (d<0) break;
                stop = (stop<<4)|(unsigned)d; q++;
            }
        }
        if (addr >= start && addr < stop){
            size_t linelen = (size_t)(eol - p);
            if (crash_mem_contains(p, linelen, "libimp.so"))
                return "libimp.so (Ingenic video/ISP/encoder vendor library)";
            if (crash_mem_contains(p, linelen, "libaudioProcess.so"))
                return "libaudioProcess.so (Ingenic audio vendor library)";
            if (crash_mem_contains(p, linelen, "timpsd"))
                return "timps's own code (timpsd binary)";
            return "some other mapped region (see maps dump)";
        }
        p = (eol < end) ? eol+1 : end;
    }
    return "not inside any mapped region (wild/corrupted pointer)";
}

/* The handler itself. SA_SIGINFO gives us siginfo_t (si_addr = faulting
 * address) and the ucontext_t (uc_mcontext holds the faulting PC, field name
 * is architecture-specific - see below). Logs to both stderr (visible when
 * run in the foreground / make sim) and MS_CRASH_LOG_PATH (a flat file in
 * /run, same convention as the singleton lock and the /control token file -
 * survives the process death so a respawn supervisor can pick it up and feed
 * it to logread, since the init script normally backgrounds timpsd and its
 * stderr is otherwise discarded, see log.c). Never attempts to continue past
 * the fault: restores the signal's default disposition and re-raises it. */
static void fatal_signal_handler(int sig, siginfo_t *info, void *ucontext_v)
{
    int crashfd = open(MS_CRASH_LOG_PATH, O_CREAT|O_WRONLY|O_TRUNC|O_CLOEXEC, 0644);

    const char *signame = sig==SIGSEGV ? "SIGSEGV" : sig==SIGBUS ? "SIGBUS" :
                           sig==SIGFPE  ? "SIGFPE"  : sig==SIGABRT? "SIGABRT" : "SIG?";
    unsigned long addr = (unsigned long)(info ? info->si_addr : NULL);

    unsigned long pc = 0;
    if (ucontext_v){
        ucontext_t *uc = (ucontext_t*)ucontext_v;
#if defined(__mips__)
        /* uClibc/glibc MIPS o32 sys/ucontext.h: mcontext_t has a direct
         * `greg_t pc;` member (verified against this target's actual
         * toolchain header, not assumed - see report). */
        pc = (unsigned long)uc->uc_mcontext.pc;
#elif defined(__aarch64__)
        pc = (unsigned long)uc->uc_mcontext.pc;
#elif defined(__x86_64__)
        pc = (unsigned long)uc->uc_mcontext.gregs[REG_RIP];   /* host `make sim` testing only */
#endif
    }

    int fds[2] = { 2, crashfd };
    for (int i=0;i<2;i++){
        int fd = fds[i];
        if (fd < 0) continue;
        crash_write(fd, "\n*** timpsd FATAL: ");
        crash_write(fd, signame);
        crash_write(fd, " si_addr=");   crash_write_hex(fd, addr);
        crash_write(fd, " pc=");        crash_write_hex(fd, pc);
        crash_write(fd, "\n");
    }

    const char *where = crash_classify_addr(crashfd, addr);
    crash_write(2,       "fault address is in: "); crash_write(2,       where); crash_write(2,       "\n");
    crash_write(crashfd, "fault address is in: "); crash_write(crashfd, where); crash_write(crashfd, "\n");
    if (crashfd >= 0) close(crashfd);

    /* Do NOT resume - restore default disposition, unblock this signal (it's
     * currently blocked, see sa_mask in install_fatal_handlers below), and
     * re-raise so the kernel does its normal thing. The _exit() is a
     * last-resort fallback that should never actually execute. */
    signal(sig, SIG_DFL);
    sigset_t set; sigemptyset(&set); sigaddset(&set, sig);
    sigprocmask(SIG_UNBLOCK, &set, NULL);
    raise(sig);
    _exit(128 + sig);
}

static void install_fatal_handlers(void)
{
    stack_t ss = { .ss_sp = g_altstack, .ss_size = sizeof g_altstack, .ss_flags = 0 };
    if (sigaltstack(&ss, NULL) != 0)
        LOGW(MOD, "sigaltstack failed (%s) - a stack-overflow fault won't be caught", strerror(errno));

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fatal_signal_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigfillset(&sa.sa_mask);  /* block other signals (incl. the other 3 fatal
                               * ones) for the duration of the handler */
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
    sigaction(SIGFPE,  &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

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
    /* O_CLOEXEC: imp_motion.c's on_motion hook double-forks a detached
     * grandchild that execlp()s a long-lived script (clip capture + upload,
     * several seconds). Without CLOEXEC that grandchild inherits this fd and
     * keeps the flock held after timpsd itself exits (restart/crash/this
     * watchdog's own escalation) - the next instance then loses the race
     * against an orphan holding the lock and refuses to start, with nothing
     * left to respawn it. Matches the CLOEXEC convention already used for
     * other long-lived fds (net.c, speaker.c). */
    int fd = open(MS_LOCK_PATH, O_CREAT|O_RDWR|O_CLOEXEC, 0644);
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
/* L-4: hard-exit deadline for the whole shutdown path. It is a guillotine, not
 * a timeout - it _exit()s wherever teardown happens to be, so anything still
 * unfinished (a recording segment whose moov has not been written) is lost.
 * Kept at 3 s deliberately: the network servers no longer NEED it (their stop
 * paths now END their client threads instead of outwaiting them, see
 * ms_creg_wake_all() in util.h), so what remains inside the window is
 * record_stop()/timelapse_stop() finalising their files plus the vendor IMP
 * teardown - and the vendor side is precisely what cannot be trusted to
 * return, which is the reason this exists.
 * Lengthening it to give the recorder more room would be the wrong trade in
 * both directions. The recorder does not actually compete for the tail of this
 * window: main() below stops it BEFORE rtsp_stop()/httpd_stop(), so it gets the
 * front of the budget, and what used to eat the rest (stream threads that never
 * returned - measured at >20 s, i.e. the alarm was firing every single time a
 * client was connected) is now gone: the same measurement is 40 ms after the
 * change. Meanwhile a longer alarm lengthens EVERY wedged-vendor shutdown on a
 * device whose recovery path is a watchdog restart. So: unchanged at 3 s, but
 * for a different reason than before - it is a backstop again rather than the
 * routine exit path. -D overridable for the shutdown-latency measurements,
 * which must outlive the deadline to be able to report what happened. */
#ifndef MS_SHUTDOWN_ALARM_S
#define MS_SHUTDOWN_ALARM_S 3
#endif
static void on_signal(int s)
{
    (void)s;
    g_run = 0;
    /* guarantee the process actually stops even if vendor teardown stalls:
     * a second Ctrl-C, or MS_SHUTDOWN_ALARM_S without a clean exit, forces
     * termination. */
    signal(SIGINT,  hard_exit);
    signal(SIGTERM, hard_exit);
    signal(SIGALRM, hard_exit);
    alarm(MS_SHUTDOWN_ALARM_S);
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
    /* Installed before anything else: covers config parsing, HAL/IMP bring-
     * up and the whole run loop. Depends only on the static altstack buffer
     * above, nothing else needs to be initialized first. */
    install_fatal_handlers();

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
#ifdef MS_CLOCK_SCALE
    /* Virtual clock active (sim builds only, see ms_now_us() in util.h). This
     * line is the replay harness's HANDSHAKE, not decoration: a binary built
     * WITHOUT the -DMS_CLOCK_SCALE hook silently ignores the flag and runs in
     * real time, while the harness keeps driving the scenario 60x faster than
     * the machine experiences it. Every deadline in the machine then becomes
     * unreachable and the scenario's incident cascade simply never happens -
     * which reads as a clean pass, i.e. a FALSE NEGATIVE that looks like
     * evidence the scenario is toothless. (Seen for real: a 12000 s dawn
     * scenario run against a pre-hook binary, where the tell was the first
     * switch landing at "t=451s" instead of t=8s - 7.5 REAL seconds of boot
     * settle multiplied by the scale the binary never applied.) So the sim
     * announces its scale and scripts/dn-replay.py refuses to run any binary
     * that does not announce the scale the scenario asked for. */
    LOGI(MOD,"virtual clock: MS_CLOCK_SCALE=%d (sim replay build)",
         (int)(MS_CLOCK_SCALE));
#endif

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
