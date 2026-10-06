/* libimp_check.h - does the libimp this process runs on fit the build?
 * timpsd --check-libs and the daemon start (before any IMP call) both use it. */
#ifndef MS_LIBIMP_CHECK_H
#define MS_LIBIMP_CHECK_H

/* exit codes, also matched by thingino's S95timps preflight */
#define TIMPS_EXIT_LIBIMP_FLAVOUR 78   /* OpenIMP build on a vendor libimp, or vice versa */
#define TIMPS_EXIT_LIBIMP_SYMBOL  79   /* libimp lacks a function every start calls */

/* Called on every start by every build. Each name must be exported by every
 * supported libimp, vendor and OpenIMP, or the check refuses a working camera:
 * scripts/check-libimp-required.sh verifies this table against libraries and
 * headers. */
#define LIBIMP_REQUIRED(X) \
    X(IMP_System_Init) X(IMP_System_Exit) X(IMP_System_Bind) X(IMP_System_UnBind) \
    X(IMP_System_GetTimeStamp) \
    X(IMP_ISP_Open) X(IMP_ISP_Close) X(IMP_ISP_AddSensor) X(IMP_ISP_DelSensor) \
    X(IMP_ISP_EnableSensor) X(IMP_ISP_DisableSensor) X(IMP_ISP_EnableTuning) \
    X(IMP_FrameSource_CreateChn) X(IMP_FrameSource_DestroyChn) \
    X(IMP_FrameSource_SetChnAttr) X(IMP_FrameSource_EnableChn) \
    X(IMP_FrameSource_DisableChn) \
    X(IMP_Encoder_CreateGroup) X(IMP_Encoder_CreateChn) X(IMP_Encoder_RegisterChn) \
    X(IMP_Encoder_StartRecvPic) X(IMP_Encoder_PollingStream) \
    X(IMP_Encoder_GetStream) X(IMP_Encoder_ReleaseStream)

/* OpenIMP exports all three on every SoC (its libimp.map); no vendor libimp
 * exports any OpenIMP_* symbol. Any one present = OpenIMP. */
#define LIBIMP_OPENIMP_MARKERS(X) \
    X(OpenIMP_P0_GetState) X(OpenIMP_AVC_Create) X(OpenIMP_Tuning_Create)

enum { LIBIMP_OK = 0, LIBIMP_SKIPPED, LIBIMP_FLAVOUR, LIBIMP_MISSING };

typedef void *(*libimp_lookup_fn)(void *ctx, const char *name);
typedef void (*libimp_report_fn)(void *ctx, int error, const char *msg);

/* Pure decision: built_openimp = this binary's flavour, lookup = dlsym on the
 * libimp handle. One report() per problem (error = 0 only for SKIPPED, i.e.
 * no required name resolves at all); returns a LIBIMP_* verdict. */
int libimp_evaluate(int built_openimp, const char *libname,
                    libimp_lookup_fn lookup, void *lctx,
                    libimp_report_fn report, void *rctx);
int libimp_exit_code(int verdict);

/* check_only = 1: --check-libs (stderr/stdout only). 0: daemon start (LOGE).
 * Returns 0 (also when the check cannot run) or a TIMPS_EXIT_LIBIMP_* code. */
int libimp_preflight(int check_only);

#endif
