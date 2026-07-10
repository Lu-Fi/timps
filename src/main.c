/*
 * main.c — timps entry point.
 *
 * Usage: timps [-c config] [-d] [-v] [-h]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <pthread.h>

#include "config.h"
#include "log.h"
#include "imp_init.h"
#include "audio.h"
#include "osd.h"
#include "motion.h"
#include "rtsp.h"
#include "http.h"

int g_log_level = LOG_LEVEL_INFO;

static volatile int g_quit = 0;

static void sig_handler(int sig)
{
    (void)sig;
    g_quit = 1;
}

static void daemonize(void)
{
    pid_t pid;

    pid = fork();
    if (pid < 0) { perror("fork"); exit(EXIT_FAILURE); }
    if (pid > 0) exit(EXIT_SUCCESS); /* parent exits */

    if (setsid() < 0) exit(EXIT_FAILURE);

    /* Redirect standard file descriptors */
    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > 2) close(fd);
    }
}

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  -c <file>   Configuration file (default: /etc/timps.conf)\n"
        "  -d          Daemonize (run in background)\n"
        "  -v          Increase log verbosity (may be repeated)\n"
        "  -h          Show this help\n"
        "\n"
        "RTSP URL:    rtsp://<host>:554/stream0   (main)\n"
        "             rtsp://<host>:554/stream1   (sub)\n"
        "HTTP stream: http://<host>:8080/stream\n"
        "MJPEG:       http://<host>:8080/mjpeg\n"
        "Snapshot:    http://<host>:8080/snapshot.jpg\n",
        prog);
}

/* OSD/motion poll thread */
static void *osd_thread(void *arg)
{
    (void)arg;
    while (!g_quit) {
        osd_update();
        if (motion_triggered())
            log_info("motion detected");
        sleep(1);
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    const char *cfg_path = "/etc/timps.conf";
    int         daemonize_flag = 0;
    int         verbosity = 0;
    int         opt;

    while ((opt = getopt(argc, argv, "c:dvh")) != -1) {
        switch (opt) {
        case 'c': cfg_path        = optarg;  break;
        case 'd': daemonize_flag  = 1;       break;
        case 'v': verbosity++;               break;
        case 'h': print_usage(argv[0]);      return 0;
        default:  print_usage(argv[0]);      return 1;
        }
    }

    /* Load configuration */
    config_defaults();
    if (access(cfg_path, R_OK) == 0) {
        if (config_load(cfg_path) != 0) {
            fprintf(stderr, "Failed to load config: %s\n", cfg_path);
            return 1;
        }
    } else {
        fprintf(stderr, "Config not found: %s — using defaults\n", cfg_path);
    }

    /* Override log level from CLI */
    g_cfg.log_level += verbosity;
    if (daemonize_flag)
        g_cfg.daemonize = 1;

    g_log_level = g_cfg.log_level;

    if (g_cfg.daemonize)
        daemonize();

    /* Seed PRNG for nonce generation */
    srand((unsigned)time(NULL) ^ (unsigned)getpid());

    log_info("timps starting up");
    config_dump();

    /* Signal handling */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* Initialise IMP (ISP + FrameSource) */
    if (imp_init() != 0) {
        log_error("IMP initialisation failed");
        return 1;
    }

    /* Initialise audio */
    if (g_cfg.audio_codec != AUDIO_NONE) {
        if (audio_init() != 0)
            log_warn("Audio init failed — continuing without audio");
    }

    /* OSD */
    if (g_cfg.osd_enabled)
        osd_init();

    /* Motion detection */
    if (g_cfg.motion_enabled)
        motion_init();

    /* Servers */
    if (rtsp_init() != 0) {
        log_error("RTSP server failed to start");
        goto shutdown;
    }

    if (http_init() != 0) {
        log_error("HTTP server failed to start");
        goto shutdown;
    }

    /* OSD update thread */
    pthread_t osd_tid;
    pthread_create(&osd_tid, NULL, osd_thread, NULL);

    log_info("timps ready — RTSP port %d  HTTP port %d",
             g_cfg.rtsp_port, g_cfg.http_port);

    /* Main loop — wait for signal */
    while (!g_quit)
        sleep(1);

    log_info("shutting down...");
    g_quit = 1;
    pthread_join(osd_tid, NULL);

shutdown:
    http_exit();
    rtsp_exit();
    motion_exit();
    osd_exit();
    audio_exit();
    imp_exit();

    log_info("timps stopped");
    return 0;
}
