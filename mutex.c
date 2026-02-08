#include "mutex.h"
#include "cpu.h"
#include "serial.h"

void mutex_init(mutex_t *m) {
    spinlock_init(&m->lock);
    m->held = 0;
    m->owner_pid = 0;
    m->wait_head = NULL;
    m->wait_tail = NULL;
}

void mutex_acquire(mutex_t *m) {
    for (;;) {
        spinlock_acquire(&m->lock);

        if (!m->held) {
            // Mutex is free — acquire it
            m->held = 1;
            m->owner_pid = task_current_id();
            spinlock_release(&m->lock);
            return;
        }

        // Mutex is held — add current task to wait queue and block
        cpu_t *cpu = get_cpu();
        if (!cpu || !cpu->current_task) {
            // Kernel context without a task — spin instead
            spinlock_release(&m->lock);
            for (volatile int i = 0; i < 1000; i++);
            continue;
        }

        task_t *current = (task_t *)cpu->current_task;
        current->next = NULL;

        if (m->wait_tail) {
            m->wait_tail->next = current;
            m->wait_tail = current;
        } else {
            m->wait_head = current;
            m->wait_tail = current;
        }

        // Set state to BLOCKED before releasing spinlock
        // task_block() will yield to the scheduler
        current->state = TASK_BLOCKED;
        spinlock_release(&m->lock);

        // Yield — we won't be re-enqueued because state is BLOCKED
        task_yield();

        // When we wake up (via mutex_release -> task_unblock),
        // loop back and try to acquire again
    }
}

int mutex_try_acquire(mutex_t *m) {
    spinlock_acquire(&m->lock);

    if (!m->held) {
        m->held = 1;
        m->owner_pid = task_current_id();
        spinlock_release(&m->lock);
        return 1;
    }

    spinlock_release(&m->lock);
    return 0;
}

void mutex_release(mutex_t *m) {
    spinlock_acquire(&m->lock);

    m->held = 0;
    m->owner_pid = 0;

    // Wake up the first waiter
    task_t *waiter = m->wait_head;
    if (waiter) {
        m->wait_head = waiter->next;
        if (!m->wait_head) {
            m->wait_tail = NULL;
        }
        waiter->next = NULL;
    }

    spinlock_release(&m->lock);

    // Unblock outside the spinlock to avoid nested lock issues
    if (waiter) {
        task_unblock(waiter);
    }
}
