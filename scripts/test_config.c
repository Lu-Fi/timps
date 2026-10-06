/* test_config.c - host-only unit test for config.c's line parser.
 *
 * Built and run by `make test-config`. It links the real src/config.c (plus
 * log.c/util.c, its only .c dependencies), writes small config files to a
 * temp path, loads them through the real config_load() and compares what
 * config_get_kv() reads back - i.e. exactly what the daemon would act on.
 *
 * The cases below are the ones that have actually been wrong at some point;
 * add to them rather than starting a second harness elsewhere.
 */
#include "config.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int g_fail;
static char g_path[256];

static void write_conf(const char *body)
{
    FILE *f = fopen(g_path, "w");
    if (!f){ fprintf(stderr, "cannot write %s\n", g_path); exit(2); }
    fputs(body, f);
    fclose(f);
}

/* load `body` and assert that `key` reads back exactly as `want` */
static void check(const char *what, const char *body,
                  const char *key, const char *want)
{
    static ms_config c;
    char got[256] = "";
    write_conf(body);
    config_load(&c, g_path);
    if (!config_get_kv(&c, key, got, sizeof got)){
        printf("FAIL %-46s %s is not readable at all\n", what, key);
        g_fail++;
        return;
    }
    if (strcmp(got, want)){
        printf("FAIL %-46s %s = [%s], expected [%s]\n", what, key, got, want);
        g_fail++;
        return;
    }
    printf("ok   %-46s %s = [%s]\n", what, key, got);
}

/* load `body` and assert an int the loader ends up with (the channel-wiring
 * keys are F_NOGET, so config_get_kv() cannot read them back) */
static void check_int(const char *what, const char *body, const int *field,
                      int want, ms_config *c)
{
    write_conf(body);
    config_load(c, g_path);
    if (*field != want){
        printf("FAIL %-46s = %d, expected %d\n", what, *field, want);
        g_fail++;
        return;
    }
    printf("ok   %-46s = %d\n", what, *field);
}

/* load + sensor finalize (no /proc/jz/sensor on the host, so the sensor size
 * comes from sensor.width/height in `body`) and assert videoN's dims */
static const ms_config *check_dims(const char *what, const char *body,
                                   int stream, int want_w, int want_h)
{
    static ms_config c;
    write_conf(body);
    config_load(&c, g_path);
    config_sensor_finalize(&c);
    const ms_vstream_cfg *v = &c.video[stream];
    if (v->width != want_w || v->height != want_h){
        printf("FAIL %-46s video%d = %dx%d, expected %dx%d\n", what, stream,
               v->width, v->height, want_w, want_h);
        g_fail++;
        return &c;
    }
    printf("ok   %-46s video%d = %dx%d\n", what, stream, v->width, v->height);
    return &c;
}

int main(void)
{
    /* config_load() sets the log level from the file it just read, so the
     * parser's own lines cannot be suppressed here - just keep them in order
     * with ours. */
    setvbuf(stdout, NULL, _IONBF, 0);
    snprintf(g_path, sizeof g_path, "/tmp/timps-test-config-%d.conf", (int)getpid());

    /* the bug this harness was written for: a quoted value followed by an
     * inline comment kept BOTH the quotes and the comment */
    check("quoted value + inline comment",
          "record.dir = \"/mnt/sd\" # a note\n", "record.dir", "/mnt/sd");
    check("quoted value, '#' inside + trailing comment",
          "record.name = \"cam # 2\" # trailing comment\n",
          "record.name", "cam # 2");

    /* the plain cases, so the fix above cannot quietly break them */
    check("quoted value, nothing after it",
          "record.dir = \"/mnt/sd\"\n", "record.dir", "/mnt/sd");
    check("single-quoted value + inline comment",
          "record.dir = '/mnt/sd' # a note\n", "record.dir", "/mnt/sd");
    check("bare value + inline comment",
          "record.dir = /mnt/sd # a note\n", "record.dir", "/mnt/sd");
    check("bare value, no comment",
          "record.dir = /mnt/sd\n", "record.dir", "/mnt/sd");
    check("quoted value keeps its inner spaces",
          "record.name = \"  pad  \"\n", "record.name", "  pad  ");
    check("quoted empty value",
          "record.name = \"\"\n", "record.name", "");

    /* write_kv_line() does NOT escape a quote inside a value and relies on the
     * loader stripping one leading + one trailing quote - so this round trip
     * must keep working (see the comment above write_kv_line in config.c) */
    check("value containing quotes round-trips",
          "record.name = \"say \"hi\"\"\n", "record.name", "say \"hi\"");
    check("value ending in a quote round-trips",
          "record.name = \"a\"\"\n", "record.name", "a\"");
    check("comment characters inside a quoted value survive",
          "record.name = \"a\" # b\"\n", "record.name", "a\" # b");

    /* unbalanced quote: kept verbatim (and warned about), not truncated */
    check("unterminated quote kept verbatim",
          "record.name = \"oops\n", "record.name", "\"oops");

    /* F_RESTART: what caps.restart and the POST reply's "deferred" list say */
    static const struct { const char *key; int want; } rk[] = {
        { "audio.codec", 1 }, { "audio.codec2", 1 }, { "audio.agc", 1 },
        { "audio.backchannel_rate", 1 }, { "audio.volume", 0 },
        { "audio.mute", 0 }, { "audio.talk_ws", 0 },
        { "osd.enabled", 1 }, { "osd.font_path", 1 }, { "osd.supersample", 1 },
        { "osd.hinting", 1 }, { "osd.monitor_stream", 0 },
        { "osd0.1.enabled", 0 }, { "video0.bitrate", 0 }, { "nosuch.key", 0 },
    };
    for (size_t i = 0; i < sizeof rk / sizeof rk[0]; i++) {
        int got = config_key_restart(rk[i].key);
        if (got != rk[i].want) {
            printf("FAIL %-46s config_key_restart(%s) = %d, expected %d\n",
                   "restart flag", rk[i].key, got, rk[i].want);
            g_fail++;
        } else
            printf("ok   %-46s config_key_restart(%s) = %d\n",
                   "restart flag", rk[i].key, got);
    }

    unlink(g_path);
    /* channel wiring: videoN publishes into hub slot videoN.imp_chn, so a
     * mismatch silently fed another stream's (or the audio) slot, and a
     * duplicate JPEG encoder channel failed start() into its reboot path */
    {
        static ms_config cc;
        check_int("video1.imp_chn forced to the stream index",
                  "video1.enabled = 1\nvideo1.imp_chn = 2\n", &cc.video[1].imp_chn, 1, &cc);
        check_int("jpeg.imp_chn colliding with video0 moved",
                  "jpeg.enabled = 1\njpeg.imp_chn = 0\n", &cc.jpeg.imp_chn, 2, &cc);
        check_int("video1.jpeg_chn duplicating video0's moved",
                  "video1.enabled = 1\nvideo0.jpeg_chn = 3\nvideo1.jpeg_chn = 3\n",
                  &cc.video[1].jpeg_chn, 2, &cc);
        check_int("defaults stay untouched",
                  "video1.enabled = 1\njpeg.enabled = 1\n", &cc.video[1].jpeg_chn, 4, &cc);
    }

    /* general.fs_keepalive: -1 auto by default, clamped to -1..1, words parsed */
    {
        static ms_config kc;
        check_int("fs_keepalive default is auto (-1)", "", &kc.fs_keepalive, -1, &kc);
        check_int("fs_keepalive = 1", "general.fs_keepalive = 1\n", &kc.fs_keepalive, 1, &kc);
        check_int("fs_keepalive = 0", "general.fs_keepalive = 0\n", &kc.fs_keepalive, 0, &kc);
        check_int("fs_keepalive = 5 clamped", "general.fs_keepalive = 5\n", &kc.fs_keepalive, 1, &kc);
        check_int("fs_keepalive = -9 clamped", "general.fs_keepalive = -9\n", &kc.fs_keepalive, -1, &kc);
        check_int("fs_keepalive = auto", "general.fs_keepalive = auto\n", &kc.fs_keepalive, -1, &kc);
        check_int("fs_keepalive = on", "general.fs_keepalive = on\n", &kc.fs_keepalive, 1, &kc);
        check_int("fs_keepalive = off", "general.fs_keepalive = off\n", &kc.fs_keepalive, 0, &kc);
        check_int("fs_keepalive = true", "general.fs_keepalive = true\n", &kc.fs_keepalive, 1, &kc);
        check_int("fs_keepalive = false", "general.fs_keepalive = false\n", &kc.fs_keepalive, 0, &kc);
        check_int("fs_keepalive = junk keeps auto", "general.fs_keepalive = always\n", &kc.fs_keepalive, -1, &kc);
    }

    /* daynight.ir_idle_off / ir_idle_linger_s: off and 60 s by default, clamped */
    {
        static ms_config ic;
        check_int("ir_idle_off default 0", "", &ic.daynight.ir_idle_off, 0, &ic);
        check_int("ir_idle_linger_s default 60", "", &ic.daynight.ir_idle_linger_s, 60, &ic);
        check_int("ir_idle_off = 1", "daynight.ir_idle_off = 1\n", &ic.daynight.ir_idle_off, 1, &ic);
        check_int("ir_idle_off = 7 clamped", "daynight.ir_idle_off = 7\n", &ic.daynight.ir_idle_off, 1, &ic);
        check_int("ir_idle_linger_s = 0", "daynight.ir_idle_linger_s = 0\n", &ic.daynight.ir_idle_linger_s, 0, &ic);
        check_int("ir_idle_linger_s = 9999 clamped", "daynight.ir_idle_linger_s = 9999\n",
                  &ic.daynight.ir_idle_linger_s, 3600, &ic);
        check_int("ir_idle_linger_s = -5 clamped", "daynight.ir_idle_linger_s = -5\n",
                  &ic.daynight.ir_idle_linger_s, 0, &ic);
        check("ir_idle_off readable", "daynight.ir_idle_off = 1\n", "daynight.ir_idle_off", "1");
        check("ir_idle_linger_s readable", "daynight.ir_idle_linger_s = 120\n",
              "daynight.ir_idle_linger_s", "120");
    }

    /* default stream size follows a smaller sensor, never upscales */
    {
        const char *s720 = "sensor.width = 1280\nsensor.height = 720\n";
        char b[256];
        check_dims("720p sensor, dims unset", s720, 0, 1280, 720);
        check_dims("720p sensor, sub stream unchanged", s720, 1, 640, 360);
        snprintf(b, sizeof b, "%svideo0.width = 1920\nvideo0.height = 1080\n", s720);
        check_dims("720p sensor, explicit 1920x1080 wins", b, 0, 1920, 1080);
        snprintf(b, sizeof b, "%svideo0.width = 1920\n", s720);
        check_dims("720p sensor, only width explicit", b, 0, 1920, 1080);
        check_dims("1440p sensor keeps the 1080p default",
                   "sensor.width = 2560\nsensor.height = 1440\n", 0, 1920, 1080);
        const ms_config *u = check_dims("unknown sensor keeps the 1080p default",
                                        "", 0, 1920, 1080);
        if (u->sensor.width != 1920 || u->sensor.height != 1080){
            printf("FAIL unknown sensor fallback = %dx%d, expected 1920x1080\n",
                   u->sensor.width, u->sensor.height);
            g_fail++;
        } else printf("ok   %-46s sensor = 1920x1080\n", "unknown sensor fallback");
        check_dims("sensor smaller than the sub stream",
                   "sensor.width = 320\nsensor.height = 240\n", 1, 320, 240);
    }

    /* an embedded NUL used to truncate the value silently (strlen) instead
     * of being reported; the line is now dropped as a whole */
    {
        static const char body[] = "record.dir = /mnt/a\0/b\nrecord.name = ok\n";
        FILE *f = fopen(g_path, "w");
        fwrite(body, 1, sizeof body - 1, f);
        fclose(f);
        static ms_config c; char got[256] = "";
        config_load(&c, g_path);
        config_get_kv(&c, "record.dir", got, sizeof got);
        if (!strcmp(got, "/mnt/a")) { printf("FAIL embedded NUL truncated the value to [%s]\n", got); g_fail++; }
        else printf("ok   %-46s record.dir = [%s]\n", "line with embedded NUL dropped", got);
        config_get_kv(&c, "record.name", got, sizeof got);
        if (strcmp(got, "ok")) { printf("FAIL line after the NUL line lost: [%s]\n", got); g_fail++; }
        else printf("ok   %-46s record.name = [%s]\n", "next line still parsed", got);
    }

    /* the rewrite must keep a hand-applied chmod 600 on a file of secrets */
    {
        write_conf("record.dir = /mnt/a\n");
        chmod(g_path, 0600);
        const char *k[] = { "record.dir" }, *v[] = { "/mnt/b" };
        config_write_keys(g_path, k, v, 1);
        struct stat st;
        if (stat(g_path, &st) != 0 || (st.st_mode & 0777) != 0600) {
            printf("FAIL config rewrite changed mode to %o\n", (unsigned)(st.st_mode & 0777));
            g_fail++;
        } else printf("ok   %-46s mode = 600\n", "rewrite keeps the file mode");
    }

    if (g_fail){ printf("\n%d config parser test(s) FAILED\n", g_fail); return 1; }
    printf("\nall config parser tests passed\n");
    return 0;
}
