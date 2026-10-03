/* test_image_caps.c - host-only test for GET /control caps.image and its
 * run-time adjustment by OpenIMP's IMP_ISP_QueryCaps (config.c
 * cfg_image_caps_adjust, hal_ingenic.c isp_query_caps).
 *
 * Built and run by `make test-image-caps`, once per PLATFORM (config.c is
 * compiled with -DPLATFORM_<SoC>, exactly like the cross build, so the F_CAP /
 * F_NOHW baseline is that SoC's). The list is built with the same walk as
 * control.c's control_get_json() (cfg_field_capped in table order).
 *
 * Regression boundary: a vendor libimp has no IMP_ISP_QueryCaps, the HAL then
 * never calls cfg_image_caps_adjust, and caps.image must be byte-identical to
 * the pre-QueryCaps rule "(flags & (F_CTRL|F_CAP)) == (F_CTRL|F_CAP)". Each
 * scenario runs in a forked child because the adjustment is once-per-process.
 */
#include "config.h"
#include "isp_caps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define ALL ((1ULL << 23) - 1ULL)

static const char *plat = "?";
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "[%s] FAIL %s:%d: ", plat, \
    __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fails++; } } while (0)

/* the rule control.c used before the run-time query existed */
static void list_baseline(char *buf, size_t cap)
{
    int n; const cfg_field *t = cfg_fields_image(&n);
    size_t off = 0; buf[0] = 0;
    for (int i = 0; i < n; i++)
        if ((t[i].flags & (F_CTRL|F_CAP)) == (F_CTRL|F_CAP))
            off += (size_t)snprintf(buf + off, cap - off, "%s\"%s\"", off ? "," : "", t[i].name);
}

/* the rule control.c uses now */
static void list_now(char *buf, size_t cap)
{
    int n; const cfg_field *t = cfg_fields_image(&n);
    size_t off = 0; buf[0] = 0;
    for (int i = 0; i < n; i++)
        if (cfg_field_capped(&t[i]))
            off += (size_t)snprintf(buf + off, cap - off, "%s\"%s\"", off ? "," : "", t[i].name);
}

static int has(const char *list, const char *key)
{
    char q[64]; snprintf(q, sizeof q, "\"%s\"", key);
    return strstr(list, q) != NULL;
}

static int is_nohw_baseline(const char *key)
{
    int n; const cfg_field *t = cfg_fields_image(&n);
    for (int i = 0; i < n; i++)
        if (!strcmp(t[i].name, key)) return (t[i].flags & F_NOHW) != 0;
    return -1;
}

/* run fn in a child; the child's own CHECK count is its exit code */
static void scenario(const char *name, void (*fn)(void))
{
    pid_t p = fork();
    if (p == 0) { fails = 0; fn(); _exit(fails > 100 ? 100 : fails); }
    int st = 0; waitpid(p, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st)) {
        fprintf(stderr, "[%s] scenario '%s' failed\n", plat, name);
        fails++;
    }
}

static char base[2048];

static void sc_vendor(void)          /* no symbol: adjust is never called */
{
    char now[2048]; list_now(now, sizeof now);
    CHECK(!strcmp(now, base), "vendor caps differ:\n  base %s\n  now  %s", base, now);
}

static void sc_no_statement(void)    /* symbol, but the library knows nothing */
{
    char now[2048];
    CHECK(cfg_image_caps_adjust(0, 0, ALL, NULL, 0, NULL, 0) == 0, "changed keys");
    list_now(now, sizeof now);
    CHECK(!strcmp(now, base), "no-statement caps differ: %s", now);
}

static void sc_no_symbols(void)      /* all applied, but no setter exported */
{
    char now[2048];
    cfg_image_caps_adjust(ALL, ALL, 0, NULL, 0, NULL, 0);
    list_now(now, sizeof now);
    CHECK(!strcmp(now, base), "all-applied without callable setters differs: %s", now);
}

static const char *const ext_keys[] = { "hue", "ae_compensation", "sinter_strength",
    "temper_strength", "dpc_strength", "defog_strength", "drc_strength",
    "backlight_compensation", "colorfx", "scene" };
static const char *const never_ext[] = { "core_wb_mode", "wb_rgain", "wb_bgain",
    "ae_it_max_us", "hflip", "vflip", "max_again", "max_dgain", "highlight_depress" };

static void sc_extend_all(void)      /* driver applies everything, all callable */
{
    char now[2048], es[512];
    cfg_image_caps_adjust(ALL, ALL, ALL, NULL, 0, es, sizeof es);
    list_now(now, sizeof now);
#if !defined(ISP_CAN_EXTEND)
    /* T40/T41 (other tuning API) and the host sim: never extended */
    CHECK(!strcmp(now, base) && !es[0], "extended without ISP_CAN_EXTEND: %s", es);
    return;
#endif
    for (size_t i = 0; i < sizeof ext_keys / sizeof *ext_keys; i++)
        CHECK(has(now, ext_keys[i]), "extendable %s missing: %s", ext_keys[i], now);
    for (size_t i = 0; i < sizeof never_ext / sizeof *never_ext; i++)
        if (is_nohw_baseline(never_ext[i]) == 1)
            CHECK(!has(now, never_ext[i]), "%s must never be extended", never_ext[i]);
    /* extended keys are no longer "no hardware": POST accepts and persists them */
    int n; const cfg_field *t = cfg_fields_image(&n);
    for (int i = 0; i < n; i++)
        if (has(now, t[i].name)) CHECK(!cfg_field_nohw(&t[i]), "%s listed but nohw", t[i].name);
    /* baseline keys stay, in table order (base is a subsequence of now) */
    const char *p = now;
    for (const char *q = base; *q == '"'; ) {
        const char *e = strchr(q + 1, '"'); char k[64];
        snprintf(k, sizeof k, "%.*s", (int)(e - q + 1), q);
        p = strstr(p, k);
        CHECK(p != NULL, "baseline %s lost or reordered", k);
        if (!p) break;
        q = e + 1; if (*q == ',') q++;
    }
    /* a second call (start retry) changes nothing */
    CHECK(cfg_image_caps_adjust(ALL, 0, 0, NULL, 0, NULL, 0) == 0, "second call not a no-op");
}

static void sc_restrict_all(void)    /* driver applies nothing it knows about */
{
    char now[2048];
    cfg_image_caps_adjust(ALL, 0, ALL, NULL, 0, NULL, 0);
    list_now(now, sizeof now);
    CHECK(now[0] == 0, "restrict-all left %s", now);
    CHECK(cfg_image_key_nohw("brightness") == 1, "brightness not restricted");
}

int main(int argc, char **argv)
{
    plat = argc > 1 ? argv[1] : "host";
    list_baseline(base, sizeof base);
    CHECK(base[0] == '"', "empty baseline");
    scenario("vendor libimp (no IMP_ISP_QueryCaps)", sc_vendor);
    scenario("QueryCaps without a statement", sc_no_statement);
    scenario("applied but setters not exported", sc_no_symbols);
    scenario("extend all", sc_extend_all);
    scenario("restrict all", sc_restrict_all);
    printf("[%s] caps.image %s (%d failure%s)\n", plat, fails ? "FAIL" : "ok",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
