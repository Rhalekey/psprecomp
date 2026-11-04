/* psprecomp -- the host thread the GE runs on. See ge_thread.h. */

#include "ge_thread.h"

#if PSPRECOMP_REAL_THREADS

#include "host_thread.h"

#define GE_THREAD_STACK (1024u * 1024u)

static host_mutex g_lock = HOST_MUTEX_INIT;
static host_cond  g_wake = HOST_COND_INIT;

void ge_lock(void)   { host_mutex_lock(&g_lock); }
void ge_unlock(void) { host_mutex_unlock(&g_lock); }
void ge_wait(void)   { host_cond_wait(&g_wake, &g_lock); }
void ge_wake(void)   { host_cond_broadcast(&g_wake); }

static void *thread_main(void *arg) {
    ((void (*)(void))arg)();
    return NULL;
}

int ge_thread_start(void (*fn)(void)) {
    host_thread t;
    return host_thread_create(&t, GE_THREAD_STACK, thread_main, (void *)fn) == 0;
}

#else

void ge_lock(void)   { }
void ge_unlock(void) { }
void ge_wait(void)   { }
void ge_wake(void)   { }
int  ge_thread_start(void (*fn)(void)) { (void)fn; return 0; }

#endif
