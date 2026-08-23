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

    unlink(g_path);
    if (g_fail){ printf("\n%d config parser test(s) FAILED\n", g_fail); return 1; }
    printf("\nall config parser tests passed\n");
    return 0;
}
