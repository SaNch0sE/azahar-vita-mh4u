// Waits that sleep (taken from TuizaVita 0.3B, 0.4.3.8).
//
// In the VITASDK's pthreads-embedded, every wait that is a cancellation point
// (pthread_cond_wait and pthread_cond_timedwait, i.e. std::condition_variable; also sem_wait
// and pthread_join) ends in pte_osSemaphoreCancellablePend, which does not sleep: it loops on
// sceKernelPollEventFlag (has the thread been cancelled?) plus sceKernelWaitSema capped at
// 500 us until the semaphore comes. An idle thread waiting for work wakes 2,000 times a second.
// The port's hot waits already use a kernel event flag (common/wake_flag.h); this covers the
// rest. Nothing here cancels threads, so the check can never fire: the wait becomes the
// library's own non-cancellable pte_osSemaphorePend, one sceKernelWaitSema with the same
// timeout and the same return codes. ux0:data/mh4u/waitpoll restores the polling wait.

#include <psp2/io/stat.h>

int pte_osSemaphorePend(int handle, unsigned int* timeout_ms);
int __real_pte_osSemaphoreCancellablePend(int handle, unsigned int* timeout_ms);

static int g_mode; // 0 unknown, 1 sleep, 2 poll (kill switch)

int __wrap_pte_osSemaphoreCancellablePend(int handle, unsigned int* timeout_ms) {
    if (g_mode == 0) {
        SceIoStat st;
        g_mode = sceIoGetstat("ux0:data/mh4u/waitpoll", &st) >= 0 ? 2 : 1;
    }
    if (g_mode == 2) {
        return __real_pte_osSemaphoreCancellablePend(handle, timeout_ms);
    }
    return pte_osSemaphorePend(handle, timeout_ms);
}
