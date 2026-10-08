/* test_dn_proven.c - host-only test for the proven-night gate on a silent
 * probe's day verdict (src/dn_proven.h). Build/run: make test-dn-proven */
#include <stdio.h>
#include "dn_proven.h"

#define M 0.90f

static int failures, checks;

#define CK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    printf("  FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void t_field(void)
{
    CK(dn_proven_dark(1, 401.0f, 401.0f, M), "Pan: lit 401 at its reference");
    CK(dn_proven_dark(1, 401.0f, 420.0f, M), "darker than the reference");
    CK(dn_proven_dark(1, 1000.0f, 1000.0f, M), "T10: flat scene");
}

static void t_brighter(void)
{
    CK(!dn_proven_dark(1, 401.0f, 100.0f, M), "lamp on: ratio decides");
    CK(!dn_proven_dark(1, 1000.0f, 899.0f, M), "just past the margin");
    CK(dn_proven_dark(1, 1000.0f, 900.0f, M), "at the margin: not brighter");
    CK(!dn_proven_dark(1, 3587.0f, 401.0f, M), "day-pipeline reference: gate open");
}

static void t_unproven(void)
{
    CK(!dn_proven_dark(0, 401.0f, 401.0f, M), "adopted/persisted night");
    CK(!dn_proven_dark(1, -1.0f, 401.0f, M), "no reference");
    CK(!dn_proven_dark(1, 0.0f, 401.0f, M), "zero reference");
    CK(!dn_proven_dark(1, 401.0f, -1.0f, M), "no lit level");
}

int main(void)
{
    t_field();
    t_brighter();
    t_unproven();
    printf("test_dn_proven: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
