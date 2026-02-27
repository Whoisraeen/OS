#ifndef FUTEX_H
#define FUTEX_H

#include <stdint.h>

// Futex operations
#define FUTEX_WAIT  0
#define FUTEX_WAKE  1

/* Private futex flag — process-local, same semantics for our in-kernel impl */
#define FUTEX_PRIVATE_FLAG  0x80
#define FUTEX_WAIT_PRIVATE  (FUTEX_WAIT | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_PRIVATE  (FUTEX_WAKE | FUTEX_PRIVATE_FLAG)

// Futex syscall interface
// FUTEX_WAIT: if *addr == val, block. Returns 0 on wakeup, -1 if *addr != val.
// FUTEX_WAKE: wake up to 'val' waiters on addr. Returns number woken.
int futex_op(uint64_t *addr, int op, uint32_t val);

#endif
