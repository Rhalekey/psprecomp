/* psprecomp -- the host thread the GE runs on, kept apart from ge.c because
 * windows.h defines macros that collide with ordinary names in a rasterizer. */
#ifndef PSPRECOMP_GE_THREAD_H
#define PSPRECOMP_GE_THREAD_H

void ge_lock(void);
void ge_unlock(void);
void ge_wait(void);    /* with the lock held, until ge_wake */
void ge_wake(void);

/* Returns nonzero if `fn` is now running on a thread of its own. Without real
 * threads there is none, and the GE keeps running inside the enqueue. */
int ge_thread_start(void (*fn)(void));

#endif
