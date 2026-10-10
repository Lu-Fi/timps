/* test_hang_wd.c - host-only test for the request-thread hang stamps
 * (src/hang_wd.h), virtual clock plus a real blocked thread.
 * Build/run: make test-hang-wd */
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include "hang_wd.h"

static int failures, checks;

#define CK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    printf("  FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void t_basic(void)
{
    hwd w = {{0}};
    CK(hwd_oldest(&w, 100) == 0, "empty");
    int a = hwd_begin(&w, 100);
    int b = hwd_begin(&w, 110);
    CK(a >= 0 && b >= 0 && a != b, "two slots");
    CK(hwd_oldest(&w, 100) == 0, "just stamped: age 0");
    CK(hwd_oldest(&w, 130) == 30, "oldest wins: %d", hwd_oldest(&w, 130));
    hwd_end(&w, a);
    CK(hwd_oldest(&w, 130) == 20, "ended slot no longer counts");
    hwd_end(&w, b);
    CK(hwd_oldest(&w, 1000) == 0, "all ended");
    hwd_end(&w, -1);
    hwd_end(&w, HWD_SLOTS);
    CK(hwd_oldest(&w, 1000) == 0, "bad slots ignored");
    CK(hwd_begin(&w, 0) >= 0 && hwd_oldest(&w, 5) == 5, "a stamp at t=0 is not 'free'");
}

static void t_full(void)
{
    hwd w = {{0}};
    for (int i = 0; i < HWD_SLOTS; i++) hwd_begin(&w, 10);
    CK(hwd_begin(&w, 20) == -1, "full -> -1, not watched");
    hwd_end(&w, 3);
    CK(hwd_begin(&w, 20) == 3, "freed slot reused");
}

static hwd g_w;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static void *stuck(void *arg)
{
    (void)arg;
    int s = hwd_begin(&g_w, 50);
    pthread_mutex_lock(&g_mu);                  /* the "IMP call" that hangs */
    pthread_mutex_unlock(&g_mu);
    hwd_end(&g_w, s);
    return NULL;
}

static void t_thread(void)
{
    pthread_t th[8];
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < 8; i++) pthread_create(&th[i], NULL, stuck, NULL);
    usleep(100000);
    CK(hwd_oldest(&g_w, 80) == 30, "blocked threads seen by the tick: %d", hwd_oldest(&g_w, 80));
    pthread_mutex_unlock(&g_mu);
    for (int i = 0; i < 8; i++) pthread_join(th[i], NULL);
    CK(hwd_oldest(&g_w, 80) == 0, "released");
}

int main(void)
{
    t_basic(); t_full(); t_thread();
    printf("hang wd: %d/%d passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
