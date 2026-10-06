/* libimp_check.c - see libimp_check.h */
#include "libimp_check.h"
#include "../log.h"
#include <stdio.h>
#include <string.h>

#define MOD "LIBIMP"

#define LIBIMP_NAME_STR(f) #f,
static const char *const k_required[] = { LIBIMP_REQUIRED(LIBIMP_NAME_STR) };
static const char *const k_markers[]  = { LIBIMP_OPENIMP_MARKERS(LIBIMP_NAME_STR) };
#define NELEM(a) (sizeof(a) / sizeof((a)[0]))

int libimp_evaluate(int built_openimp, const char *libname,
                    libimp_lookup_fn lookup, void *lctx,
                    libimp_report_fn report, void *rctx)
{
    char m[320];
    unsigned char miss[NELEM(k_required)];
    size_t nmiss = 0;
    for (size_t i = 0; i < NELEM(k_required); i++) {
        miss[i] = lookup(lctx, k_required[i]) == NULL;
        nmiss += miss[i];
    }
    /* a libimp without a single one of these is no libimp at all: the lookup
     * itself is what failed, and that must not stop a camera */
    if (nmiss == NELEM(k_required)) {
        snprintf(m, sizeof m, "no IMP function resolves in %s - libimp check "
                 "skipped", libname);
        report(rctx, 0, m);
        return LIBIMP_SKIPPED;
    }
    int lib_openimp = 0;
    for (size_t i = 0; i < NELEM(k_markers); i++)
        if (lookup(lctx, k_markers[i])) { lib_openimp = 1; break; }

    int v = LIBIMP_OK;
    if (built_openimp && !lib_openimp) {
        snprintf(m, sizeof m, "libimp mismatch: this timpsd is built for OpenIMP "
                 "(USE_OPENIMP=1) but %s is the Ingenic vendor libimp - install "
                 "the vendor-flavour timpsd (USE_OPENIMP=0) or the OpenIMP libimp",
                 libname);
        report(rctx, 1, m);
        v = LIBIMP_FLAVOUR;
    } else if (!built_openimp && lib_openimp) {
        snprintf(m, sizeof m, "libimp mismatch: this timpsd is built for the "
                 "Ingenic vendor libimp (USE_OPENIMP=0) but %s is OpenIMP - "
                 "install the OpenIMP-flavour timpsd (USE_OPENIMP=1) or the "
                 "vendor libimp", libname);
        report(rctx, 1, m);
        v = LIBIMP_FLAVOUR;
    }
    for (size_t i = 0; i < NELEM(k_required); i++) {
        if (!miss[i]) continue;
        snprintf(m, sizeof m, "libimp lacks %s - %s does not fit this timpsd",
                 k_required[i], libname);
        report(rctx, 1, m);
        if (v == LIBIMP_OK) v = LIBIMP_MISSING;
    }
    return v;
}

int libimp_exit_code(int verdict)
{
    switch (verdict) {
    case LIBIMP_FLAVOUR: return TIMPS_EXIT_LIBIMP_FLAVOUR;
    case LIBIMP_MISSING: return TIMPS_EXIT_LIBIMP_SYMBOL;
    default:             return 0;
    }
}

#ifdef HAL_INGENIC
#include <dlfcn.h>
#include <imp/imp_system.h>
#include <imp/imp_isp.h>
#include <imp/imp_framesource.h>
#include <imp/imp_encoder.h>

#ifndef LIBIMP_SONAME
#define LIBIMP_SONAME "libimp.so"
#endif

static void report_stderr(void *ctx, int error, const char *msg)
{
    (void)ctx;
    fprintf(stderr, "timpsd: %s%s\n", error ? "" : "warning: ", msg);
}

static void report_log(void *ctx, int error, const char *msg)
{
    (void)ctx;
    if (error) LOGE(MOD, "%s", msg);
    else LOGW(MOD, "%s", msg);
}

/* Through the libimp handle only, never RTLD_DEFAULT: that scope starts at
 * the executable, whose MIPS lazy stubs give every imported IMP_* a non-zero
 * value whether or not any library defines it. */
static void *lookup_dl(void *ctx, const char *name)
{
    return ctx ? dlsym(ctx, name) : NULL;
}

int libimp_preflight(int check_only)
{
    /* compile error here = a table name the SoC's IMP headers do not declare */
#define LIBIMP_DECLARED(f) (void)sizeof(&f);
    LIBIMP_REQUIRED(LIBIMP_DECLARED)
#ifdef USE_OPENIMP
    const int built_openimp = 1;
#else
    const int built_openimp = 0;
#endif
    /* libimp is NEEDED, so this returns the loaded copy and is never closed.
     * RTLD_NOLOAD in the daemon: a second copy would rerun its constructors. */
    dlerror();
    void *h = dlopen(LIBIMP_SONAME, check_only ? RTLD_NOW : RTLD_NOW | RTLD_NOLOAD);
    if (!h) {
        const char *e = dlerror();
        if (check_only) {
            fprintf(stderr, "timpsd: warning: dlopen(%s): %s - libimp check "
                    "skipped\n", LIBIMP_SONAME, e ? e : "failed");
            return TIMPS_EXIT_LIBIMP_UNCHECKED;
        }
        LOGW(MOD, "dlopen(%s): %s - libimp check skipped", LIBIMP_SONAME,
             e ? e : "failed");
        return 0;
    }
    char libname[160];
    Dl_info di;
    void *a = dlsym(h, "IMP_System_Init");
    if (a && dladdr(a, &di) && di.dli_fname && di.dli_fname[0])
        snprintf(libname, sizeof libname, "%s", di.dli_fname);
    else
        snprintf(libname, sizeof libname, "%s", LIBIMP_SONAME);

    int v = libimp_evaluate(built_openimp, libname, lookup_dl, h,
                            check_only ? report_stderr : report_log, NULL);
    if (check_only && v == LIBIMP_OK)
        printf("timpsd: libimp OK (%s build, %s)\n",
               built_openimp ? "OpenIMP" : "vendor", libname);
    if (check_only && v == LIBIMP_SKIPPED) return TIMPS_EXIT_LIBIMP_UNCHECKED;
    int rc = libimp_exit_code(v);
    if (rc && !check_only)
        LOGE(MOD, "not starting: the libimp does not fit this build (exit %d)", rc);
    return rc;
}
#else
int libimp_preflight(int check_only)
{
    if (check_only)
        printf("timpsd: simulation build, no libimp to check\n");
    return 0;
}
#endif
