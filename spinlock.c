#include "spinlock.h"

void spinlock_init(spinlock_t *lock) {
    lock->locked = 0;
}

void spinlock_acquire(spinlock_t *lock) {
    // GCC builtin for atomic test-and-set
    // __sync_lock_test_and_set returns the previous value.
    // We want to loop while the previous value was 1 (locked).
    while (__sync_lock_test_and_set(&lock->locked, 1)) {
        // Spin hint (pause instruction) to be nice to the CPU pipeline
        __asm__ volatile ("pause");
    }
}

void spinlock_release(spinlock_t *lock) {
    // Atomic release
    __sync_lock_release(&lock->locked);
}
