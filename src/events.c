/* events.c - tiny notify hub for the /events SSE push stream. See events.h.
 * Compiled to a no-op stub without -DUSE_CONTROL (the /events endpoint lives
 * under USE_CONTROL like /control, but the producers call events_notify()
 * unconditionally, so the stub keeps every build permutation linking). */
#include "events.h"

#ifdef USE_CONTROL
#include <pthread.h>
#include <time.h>

static pthread_mutex_t g_mu   = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t  g_once = PTHREAD_ONCE_INIT;
static pthread_cond_t  g_cv;
static unsigned        g_gen;

/* condvar on CLOCK_MONOTONIC so a wall-clock jump (NTP sync on boot) cannot
 * stall the wait - it doubles as the SSE keepalive/stats timer */
static void cv_init(void)
{
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(&g_cv, &a);
    pthread_condattr_destroy(&a);
}

void events_notify(void)
{
    pthread_once(&g_once, cv_init);
    pthread_mutex_lock(&g_mu);
    g_gen++;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

unsigned events_generation(void)
{
    pthread_mutex_lock(&g_mu);
    unsigned g = g_gen;
    pthread_mutex_unlock(&g_mu);
    return g;
}

unsigned events_wait(unsigned last_gen, int timeout_ms)
{
    pthread_once(&g_once, cv_init);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec  += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L){ ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&g_mu);
    while (g_gen == last_gen &&
           pthread_cond_timedwait(&g_cv, &g_mu, &ts) == 0)
        ;
    unsigned g = g_gen;
    pthread_mutex_unlock(&g_mu);
    return g;
}

#else /* !USE_CONTROL: producers still link, notification goes nowhere */

void events_notify(void) {}

#endif
