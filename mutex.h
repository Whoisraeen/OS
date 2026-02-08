#ifndef MUTEX_H
#define MUTEX_H

#include "spinlock.h"
#include "sched.h"

typedef struct {
    spinlock_t lock;            // Protects mutex state
    volatile int held;          // 1 = locked, 0 = unlocked
    uint32_t owner_pid;         // PID of holder (for debugging)
    task_t *wait_head;          // Linked list of blocked tasks
    task_t *wait_tail;
} mutex_t;

void mutex_init(mutex_t *m);
void mutex_acquire(mutex_t *m);
void mutex_release(mutex_t *m);
int mutex_try_acquire(mutex_t *m);  // Returns 1 if acquired, 0 if contended

#endif
