#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>

typedef struct {
    volatile uint32_t locked;
    // We could add debug info here later (e.g., holder CPU/Task ID)
} spinlock_t;

// Initialize a spinlock
void spinlock_init(spinlock_t *lock);

// Acquire the lock (spins until acquired)
void spinlock_acquire(spinlock_t *lock);

// Release the lock
void spinlock_release(spinlock_t *lock);

#endif
