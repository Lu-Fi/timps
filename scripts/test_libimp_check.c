/* test_libimp_check.c - host test for the libimp fit decision
 * (src/hal/libimp_check.c) with an injected symbol lookup. make test-libimp-check */
#include <stdio.h>
#include <string.h>
#include "hal/libimp_check.h"

static int failures, checks;

/* fake libimp: every required name and, for OpenIMP, the markers */
typedef struct {
    int openimp;
    const char *drop;      /* one name the fake lacks, or NULL */
    int empty;             /* nothing resolves */
} fake_lib;

#define STR(f) #f,
static const char *const req[] = { LIBIMP_REQUIRED(STR) };
static const char *const mark[] = { LIBIMP_OPENIMP_MARKERS(STR) };
#define N(a) (sizeof(a) / sizeof((a)[0]))

static void *lookup(void *ctx, const char *name)
{
    const fake_lib *f = ctx;
    static char sym;
    if (f->empty) return NULL;
    if (f->drop && !strcmp(f->drop, name)) return NULL;
    for (size_t i = 0; i < N(req); i++)
        if (!strcmp(req[i], name)) return &sym;
    if (f->openimp)
        for (size_t i = 0; i < N(mark); i++)
            if (!strcmp(mark[i], name)) return &sym;
    return NULL;
}

static char lines[8][320];
static int nlines, nerr;
static void report(void *ctx, int error, const char *msg)
{
    (void)ctx;
    if (nlines < 8) snprintf(lines[nlines], sizeof lines[0], "%s", msg);
    nlines++;
    nerr += error;
}

static void ck(const char *what, int built, fake_lib f, int want_v,
               int want_lines, const char *want_sub)
{
    nlines = nerr = 0;
    int v = libimp_evaluate(built, "/usr/lib/libimp.so", lookup, &f, report, NULL);
    checks++;
    int ok = v == want_v && nlines == want_lines;
    if (ok && want_sub && (nlines == 0 || !strstr(lines[0], want_sub))) ok = 0;
    if (!ok) {
        failures++;
        printf("  FAIL %s: verdict %d (want %d), %d lines (want %d): %s\n", what,
               v, want_v, nlines, want_lines, nlines ? lines[0] : "-");
    }
}

int main(void)
{
    /* exit-code mapping */
    checks += 4;
    if (libimp_exit_code(LIBIMP_OK) != 0) { failures++; puts("  FAIL exit OK"); }
    if (libimp_exit_code(LIBIMP_SKIPPED) != 0) { failures++; puts("  FAIL exit SKIPPED"); }
    if (libimp_exit_code(LIBIMP_FLAVOUR) != 78) { failures++; puts("  FAIL exit FLAVOUR"); }
    if (libimp_exit_code(LIBIMP_MISSING) != 79) { failures++; puts("  FAIL exit MISSING"); }

    ck("vendor build, vendor lib", 0, (fake_lib){0, NULL, 0}, LIBIMP_OK, 0, NULL);
    ck("openimp build, openimp lib", 1, (fake_lib){1, NULL, 0}, LIBIMP_OK, 0, NULL);
    ck("openimp build, vendor lib", 1, (fake_lib){0, NULL, 0}, LIBIMP_FLAVOUR, 1,
       "built for OpenIMP (USE_OPENIMP=1) but /usr/lib/libimp.so is the Ingenic vendor libimp");
    ck("vendor build, openimp lib", 0, (fake_lib){1, NULL, 0}, LIBIMP_FLAVOUR, 1,
       "built for the Ingenic vendor libimp (USE_OPENIMP=0) but /usr/lib/libimp.so is OpenIMP");
    ck("vendor lib lacks GetStream", 0, (fake_lib){0, "IMP_Encoder_GetStream", 0},
       LIBIMP_MISSING, 1, "libimp lacks IMP_Encoder_GetStream");
    ck("first required missing", 1, (fake_lib){1, "IMP_System_Init", 0},
       LIBIMP_MISSING, 1, "libimp lacks IMP_System_Init");
    /* mismatch outranks a missing name, both are reported */
    ck("mismatch + missing", 1, (fake_lib){0, "IMP_ISP_Open", 0}, LIBIMP_FLAVOUR, 2,
       "libimp mismatch");
    /* one marker suffices */
    ck("marker P0 only missing", 1, (fake_lib){1, "OpenIMP_P0_GetState", 0},
       LIBIMP_OK, 0, NULL);
    /* lookup broken: no verdict, a warning only */
    ck("nothing resolves", 1, (fake_lib){0, NULL, 1}, LIBIMP_SKIPPED, 1, "skipped");
    checks++;
    if (nerr != 0) { failures++; puts("  FAIL skip reported as error"); }

    printf("libimp check: %d/%d passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
