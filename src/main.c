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
#include <setjmp.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/file.h>
#include <sys/reboot.h>
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

static void hard_exit(int s)
{
    (void)s;
    /* async-signal-safe only: no LOGx here (stdio and syslog both take locks
     * and may allocate). Without this line the guillotine and a clean exit are
     * indistinguishable in the log - both end after "shutting down" with
     * whatever subsystem logged last. */
    static const char m[] = "timpsd: shutdown alarm fired - hard exit\n";
    ssize_t ignored = write(STDERR_FILENO, m, sizeof m - 1);
    (void)ignored;
    _exit(0);
}
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

/* ---- bring-up teardown deadline (B1) -----------------------------------
 * The shutdown path re-arms the guillotine right before its g_hal->stop()
 * because the vendor IMP teardown "is precisely what cannot be trusted to
 * return" (see above). The bring-up retry loop calls exactly the same
 * g_hal->stop() to unwind a failed start() before retrying, and had no such
 * protection: a wedged IMP_System_Exit() on, say, the 3rd of 10 attempts
 * hung main() forever - the retry cap is never reached, the reboot
 * escalation never fires, and the camera sits dark behind a process that
 * looks perfectly alive. That is the failure the cap and the escalation
 * exist to rule out, reached one level lower down.
 *
 * hard_exit() is the wrong response here: _exit(0) in the middle of the
 * bring-up loop would end the process on attempt 3 with nothing to respawn
 * it, throwing away the remaining retries AND the reboot escalation. So the
 * bring-up guillotine does not kill the process; it abandons the wedged
 * call and returns control to the loop via siglongjmp, which is safe
 * precisely because the alternative is a permanent hang in the same thread.
 *
 * Deliberately NOT MS_SHUTDOWN_ALARM_S (3 s): that number is chosen for
 * shutdown latency (S95timps' wait_stop budget), and nothing waits on us
 * during bring-up. Being wrong here is expensive - a stop() that is merely
 * slow would be declared wedged and cost the incident its one-shot reboot -
 * so the bring-up deadline is generous enough that expiring it means
 * "wedged", not "slow". A shutdown requested while we are in here still
 * gets the short budget.
 *
 * Caveat, unavoidable at this level: if the vendor call is stuck in an
 * uninterruptible kernel ioctl, SIGALRM is not delivered until it returns
 * and no user-space guillotine (this one or hard_exit) can do anything. */
#ifndef MS_STARTUP_STOP_ALARM_S
#define MS_STARTUP_STOP_ALARM_S 20
#endif
static sigjmp_buf g_stop_jb;
static volatile sig_atomic_t g_stop_armed;      /* inside a bounded stop()? */
static volatile sig_atomic_t g_stop_expired;    /* it hit the deadline */
static void startup_alarm(int s)
{
    if (g_stop_armed){
        g_stop_armed  = 0;
        g_stop_expired = 1;
        /* async-signal-safe only, like hard_exit() */
        static const char m[] =
            "timpsd: HAL teardown did not return - abandoning it\n";
        ssize_t ignored = write(STDERR_FILENO, m, sizeof m - 1);
        (void)ignored;
        siglongjmp(g_stop_jb, 1);
    }
    hard_exit(s);            /* not in a bounded stop(): shutdown guillotine */
}
/* g_hal->stop() with the deadline above. 0 = it returned, -1 = it was
 * abandoned and the vendor library is now in an unknown state. */
static int hal_stop_bounded(void)
{
    g_stop_expired = 0;
    if (sigsetjmp(g_stop_jb, 1) == 0){
        g_stop_armed = 1;
        signal(SIGALRM, startup_alarm);
        /* a stop requested by the operator keeps the short shutdown budget */
        alarm(g_run ? MS_STARTUP_STOP_ALARM_S : MS_SHUTDOWN_ALARM_S);
        g_hal->stop();
    }
    /* Order matters: cancel the alarm BEFORE clearing the armed flag. The
     * other order leaves a window where an alarm that fires just as stop()
     * returns finds armed==0 and takes hard_exit() - killing the process at
     * the exact moment things went RIGHT. This way that race lands in the
     * "abandoned" branch instead, which merely costs one wasted decision. */
    alarm(0);
    g_stop_armed = 0;
    return g_stop_expired ? -1 : 0;
}

/* Marker file for the one-shot recovery reboot, on the persistent (non-tmpfs)
 * /etc partition so it survives the reboot the way nothing in /run does. */
/* both -D overridable so the host sim can exercise these paths without a
 * writable /etc and without a 10-attempt wait */
#ifndef MS_STARTUP_MAX_START_FAILS
#define MS_STARTUP_MAX_START_FAILS 10
#endif
#ifndef MS_STARTUP_REBOOT_MARKER
#define MS_STARTUP_REBOOT_MARKER "/etc/timps-startup-reboot.flag"
#endif

/* Bring-up has failed for good ("why" says how). Escalate to ONE real reboot
 * - but ONLY one, ever, per incident: cam-kinder-rechts' hardware-verified
 * run (2026-08-22) showed process-level retries never cleared that board's
 * stuck rmem while a real `reboot` cleared it every single time, so the
 * reboot is worth taking; a second failure after it means the reboot did not
 * help either and repeating it would be a silent boot loop instead of a fix.
 * The marker is cleared the moment start() next succeeds, so a genuinely new
 * incident later (even after months of uptime) gets its own fresh one-shot
 * reboot rather than a permanently spent one.
 * Returns the process exit status; does not return at all when it reboots. */
static int startup_give_up(const char *why)
{
    if (access(MS_STARTUP_REBOOT_MARKER, F_OK) == 0){
        LOGE(MOD,"%s - AGAIN, after the one-shot recovery reboot already tried "
                 "for this. A real reboot does not fix this board's problem "
                 "either, so giving up for real (needs manual intervention, "
                 "not another reboot)", why);
        return 1;
    }
    /* The marker is the ONLY thing that makes this reboot one-shot, so it has
     * to exist before the reboot, not after. If it cannot be created - /etc
     * full, or the rootfs remounted read-only after an earlier fault, both
     * realistic on these NOR-flash boards - then the access() check above
     * finds nothing on the NEXT boot either, this path reboots again, and a
     * persistent startup fault turns into a reboot every few minutes forever:
     * precisely the boot loop the marker exists to prevent. So no marker
     * means no reboot. Dark-but-stable degrades correctly and leaves a log to
     * read; a silent reboot loop does neither.
     * close() is checked because a deferred ENOSPC surfaces there on some
     * flash filesystems, and the sync()+access() pair confirms the entry is
     * really on disk to be found after a reboot rather than merely opened. */
    int marker_err = 0;
    int mf = open(MS_STARTUP_REBOOT_MARKER, O_CREAT|O_WRONLY|O_TRUNC, 0644);
    if (mf < 0) {
        marker_err = errno;
    } else {
        if (close(mf) != 0) marker_err = errno;
        sync();
        if (!marker_err && access(MS_STARTUP_REBOOT_MARKER, F_OK) != 0)
            marker_err = errno;
    }
    if (marker_err){
        LOGE(MOD,"%s, but the one-shot reboot marker %s cannot be written "
                 "(%s) - without it every boot would take this same escalation "
                 "path and reboot again, i.e. a silent reboot loop, so giving "
                 "up permanently WITHOUT the escalation reboot. Fix the rootfs "
                 "(/etc full, or mounted read-only?) to re-enable the one-shot "
                 "recovery reboot",
             why, MS_STARTUP_REBOOT_MARKER, strerror(marker_err));
        return 1;
    }
    LOGE(MOD,"%s - escalating to ONE reboot before giving up permanently "
             "(2026-08-22 T31 precedent: retries alone did not clear whatever "
             "the board was waiting on, only a real reboot did)", why);
    sync();
    reboot(RB_AUTOBOOT);
    /* unreachable if the syscall works; fall through to the normal give-up
     * path if it somehow doesn't (no permission, sandboxed init, etc.)
     * rather than hang here forever */
    LOGE(MOD,"reboot() itself failed (%s) - giving up instead", strerror(errno));
    return 1;
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

    /* Nothing supervises this process: S95timps starts it with
     * start-stop-daemon -b, busybox init does not respawn, and there is no
     * watchdog for it. Exiting here therefore means the camera stays dark
     * until someone logs in - measured once already, after a restart where
     * the vendor ISP was still held 4 s after a CLEAN teardown. The init
     * path unwinds fully on failure, so retrying costs nothing but the wait.
     * A genuine misconfiguration retries forever at 60 s, which is log noise
     * rather than harm, and it recovers by itself once the config is fixed. */
    /* start(), unlike init(), is NOT given the same infinite patience: a
     * config problem waiting for a human to fix it is the case init()'s
     * forever-retry is for, but a start() failure here is a resource-drain
     * race (2026-08-22 T31 incident) that either clears within a handful of
     * attempts or, per that same incident's follow-up test, sometimes never
     * clears without a real reboot - at which point retrying forever is the
     * "silent, unbounded hang" the video watchdog's own MS_VIDEO_WATCHDOG_
     * MAX_RECOVERIES precedent (hal_ingenic.c) exists to avoid. Same
     * response here: give up loudly and exit after a bounded number of
     * start() failures, so the camera goes dark in a way that is visible in
     * the log and matches "needs a manual/scheduled restart" rather than an
     * indefinitely spinning process that LOOKS alive but never serves a
     * frame. init() failures do not count against this limit. */
    /* Giving up is not the end of the story: cam-kinder-rechts' own
     * hardware-verification run (2026-08-22) showed 10 retries is not
     * always enough time, but a real `reboot` fixed it every single time
     * that resource-drain incident occurred tonight, on every affected
     * camera - process-level retries never did on their own. So after the
     * retry budget is spent, startup_give_up() above escalates to ONE real
     * reboot before finally giving up. */
    char why[200];
    int start_fails = 0;
    for (int backoff = 5;;) {
        if (g_hal->init(&g_cfg) == 0) {
            if (g_hal->start(&g_cfg) == 0) {
                if (unlink(MS_STARTUP_REBOOT_MARKER) == 0)
                    LOGI(MOD,"cleared the startup-reboot marker - this "
                             "incident is over");
                break;
            }
            /* start() failing is the SAME transient class as init() failing,
             * and exiting here was the actual kill shot in the 2026-08-22 T31
             * fleet incident (5 cameras): after a real restart the previous
             * instance's rmem carve-out (22 MB) is not always released by the
             * time this instance runs. IMP_System_Init's small allocations
             * (init stage, retried by this loop) often squeak through while
             * the encoder's big contiguous alloc in start() still fails
             * ("Codec_Encode_Create failed") - so init retried forever but
             * one start() failure exited the process permanently, leaving the
             * camera dark with nothing to respawn it. start()'s failure path
             * unwinds every partially-created channel (M8), so after a
             * g_hal->stop() to release the init-stage state (System_Exit/ISP
             * close - ing_stop tolerates the already-empty channel lists) a
             * fresh init+start attempt is exactly as safe as the first one. */
            if (++start_fails >= MS_STARTUP_MAX_START_FAILS){
                /* bounded like every other teardown here: a wedge in this one
                 * would strand us one statement short of the escalation that
                 * this whole branch exists to perform */
                hal_stop_bounded();
                snprintf(why, sizeof why, "HAL start failed %d times in a row "
                         "and retries alone did not clear whatever this board "
                         "is waiting on", start_fails);
                return startup_give_up(why);
            }
            LOGE(MOD,"HAL start failed (%d/%d) - unwinding and retrying in %ds",
                 start_fails, MS_STARTUP_MAX_START_FAILS, backoff);
            if (hal_stop_bounded() != 0){
                /* The vendor teardown never returned and we walked away from
                 * it. Do NOT simply retry: init()/start() have no deadline of
                 * their own, and running them on top of a libimp left
                 * mid-teardown (locks held, threads not joined) is how a
                 * "retry" turns into a second, permanent hang - the very
                 * thing this deadline was added to prevent. A teardown that
                 * is still running after MS_STARTUP_STOP_ALARM_S is also
                 * exactly the class of fault the 2026-08-22 incident showed
                 * only a real reboot clears. So spend the retry budget here
                 * and hand this to the same one-shot, marker-gated escalation
                 * the exhausted budget uses: reboot once, and if the marker
                 * says that reboot has already been tried, stay down. */
                if (!g_run) return 1;      /* operator asked us to stop */
                snprintf(why, sizeof why, "HAL teardown after start failure "
                         "%d/%d did not return within %ds and had to be "
                         "abandoned, leaving the vendor library in an unknown "
                         "state", start_fails, MS_STARTUP_MAX_START_FAILS,
                         MS_STARTUP_STOP_ALARM_S);
                int rc = startup_give_up(why);
                /* a vendor thread is still wedged somewhere inside libimp;
                 * running libc's exit handlers on top of that buys nothing */
                _exit(rc);
            }
        } else {
            LOGE(MOD,"HAL init failed - retrying in %ds", backoff);
        }
        for (int i = 0; i < backoff && g_run; i++) sleep(1);
        if (!g_run) return 1;
        if (backoff < 60) backoff *= 2;
        if (backoff > 60) backoff = 60;
    }

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
    /* Re-arm the guillotine with a FULL budget for the vendor teardown: the
     * single alarm armed in on_signal covers everything above too, so a slow
     * recorder finalize or server stop could eat the whole 3 s and leave the
     * IMP teardown to be cut short mid-way - which leaves the rmem carve-out
     * dirty for the NEXT instance (the 2026-08-22 T31 incident: encoder
     * allocs failing after a restart). Worst case is now bounded at
     * ~2*MS_SHUTDOWN_ALARM_S instead of teardown silently getting only the
     * leftovers; S95timps' wait_stop polls long enough to cover that. */
    alarm(MS_SHUTDOWN_ALARM_S);
    g_hal->stop();
    /* the counterpart to hard_exit()'s write(): its absence after "shutting
     * down" is what identifies a shutdown the alarm cut short. */
    LOGI(MOD,"teardown complete - exiting");
    return 0;
}
