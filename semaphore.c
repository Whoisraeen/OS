#include "semaphore.h"
#include "cpu.h"

void sem_init(semaphore_t *s, int initial_count) {
    spinlock_init(&s->lock);
    s->count = initial_count;
    s->wait_head = NULL;
    s->wait_tail = NULL;
}

void sem_wait(semaphore_t *s) {
    for (;;) {
        spinlock_acquire(&s->lock);

        if (s->count > 0) {
            s->count--;
            spinlock_release(&s->lock);
            return;
        }

        // Count is 0 — block
        cpu_t *cpu = get_cpu();
        if (!cpu || !cpu->current_task) {
            // No task context — busy-wait
            spinlock_release(&s->lock);
            for (volatile int i = 0; i < 1000; i++);
            continue;
        }

        task_t *current = (task_t *)cpu->current_task;
        current->next = NULL;

        if (s->wait_tail) {
            s->wait_tail->next = current;
            s->wait_tail = current;
        } else {
            s->wait_head = current;
            s->wait_tail = current;
        }

        current->state = TASK_BLOCKED;
        spinlock_release(&s->lock);

        task_yield();
        // Woken up — loop back and try again
    }
}

int sem_try_wait(semaphore_t *s) {
    spinlock_acquire(&s->lock);

    if (s->count > 0) {
        s->count--;
        spinlock_release(&s->lock);
        return 1;
    }

    spinlock_release(&s->lock);
    return 0;
}

void sem_post(semaphore_t *s) {
    spinlock_acquire(&s->lock);

    s->count++;

    task_t *waiter = s->wait_head;
    if (waiter) {
        s->wait_head = waiter->next;
        if (!s->wait_head) {
            s->wait_tail = NULL;
        }
        waiter->next = NULL;
        // Decrement count for the waiter we're about to wake
        s->count--;
    }

    spinlock_release(&s->lock);

    if (waiter) {
        task_unblock(waiter);
    }
}
