#include "aio.h"
#include "sched.h"
#include "heap.h"
#include "console.h"
#include "serial.h"
#include "vfs.h"
#include "fd.h"
#include "spinlock.h"
#include "semaphore.h"

// Max concurrent AIO requests
#define MAX_AIO_REQUESTS 64

typedef struct aio_job {
    aio_request_t req;
    aio_result_t res;
    int status;           // 0=FREE, 1=PENDING, 2=DONE
    uint32_t pid;         // Process that submitted it
    struct aio_job *next;
} aio_job_t;

static aio_job_t jobs[MAX_AIO_REQUESTS];
static spinlock_t aio_lock;
static uint64_t next_aio_id = 1;

// Worker thread queue
static aio_job_t *work_queue_head = NULL;
static aio_job_t *work_queue_tail = NULL;
static semaphore_t work_sem;

// Worker thread function
static void aio_worker(void) {
    while (1) {
        // Wait for work
        sem_wait(&work_sem);
        
        spinlock_acquire(&aio_lock);
        aio_job_t *job = work_queue_head;
        if (job) {
            work_queue_head = job->next;
            if (!work_queue_head) work_queue_tail = NULL;
        }
        spinlock_release(&aio_lock);
        
        if (job) {
            // Execute I/O
            // Note: We need to use the FD table of the *submitting* process
            // But we are in a kernel thread.
            // Ideally we should switch address space or look up the node directly.
            // For simplicity, we'll look up the task and its FD table here.
            
            task_t *task = task_get_by_id(job->pid);
            if (task && task->fd_table) {
                fd_entry_t *entry = fd_get(task->fd_table, job->req.fd);
                int64_t ret = -1;
                
                if (entry) {
                    if (entry->type == FD_FILE && entry->node) {
                        if (job->req.opcode == AIO_OP_READ) {
                            ret = vfs_read(entry->node, job->req.offset, job->req.count, (uint8_t*)job->req.buf);
                        } else if (job->req.opcode == AIO_OP_WRITE) {
                            ret = vfs_write(entry->node, job->req.offset, job->req.count, (uint8_t*)job->req.buf);
                        }
                    } else if (entry->type == FD_DEVICE && entry->dev) {
                         if (job->req.opcode == AIO_OP_READ && entry->dev->read) {
                            ret = entry->dev->read(entry->dev, (uint8_t*)job->req.buf, job->req.count);
                        } else if (job->req.opcode == AIO_OP_WRITE && entry->dev->write) {
                            ret = entry->dev->write(entry->dev, (const uint8_t*)job->req.buf, job->req.count);
                        }
                    }
                }
                
                job->res.result = ret;
            } else {
                job->res.result = -1;
            }
            
            // Mark complete
            spinlock_acquire(&aio_lock);
            job->status = 2; // DONE
            spinlock_release(&aio_lock);
            
            // Notify waiting process? 
            // sys_aio_wait will check status.
        }
    }
}

void aio_init(void) {
    spinlock_init(&aio_lock);
    sem_init(&work_sem, 0);
    
    for (int i = 0; i < MAX_AIO_REQUESTS; i++) {
        jobs[i].status = 0;
    }
    
    // Create worker thread
    task_create("aio_worker", aio_worker);
    kprintf("[AIO] Initialized async I/O subsystem\n");
}

uint64_t sys_aio_submit(aio_request_t *req) {
    if (!req) return (uint64_t)-1;
    
    spinlock_acquire(&aio_lock);
    
    // Find free job slot
    int slot = -1;
    for (int i = 0; i < MAX_AIO_REQUESTS; i++) {
        if (jobs[i].status == 0) {
            slot = i;
            break;
        }
    }
    
    if (slot == -1) {
        spinlock_release(&aio_lock);
        return (uint64_t)-1; // Queue full
    }
    
    aio_job_t *job = &jobs[slot];
    job->req = *req;
    job->pid = task_current_id();
    job->status = 1; // PENDING
    job->res.aio_id = next_aio_id++;
    job->res.result = 0;
    job->next = NULL;
    
    // Add to work queue
    if (work_queue_tail) {
        work_queue_tail->next = job;
        work_queue_tail = job;
    } else {
        work_queue_head = job;
        work_queue_tail = job;
    }
    
    spinlock_release(&aio_lock);
    
    // Wake worker
    sem_post(&work_sem);
    
    return job->res.aio_id;
}

uint64_t sys_aio_wait(uint64_t aio_id, aio_result_t *res) {
    while (1) {
        spinlock_acquire(&aio_lock);
        
        aio_job_t *found = NULL;
        for (int i = 0; i < MAX_AIO_REQUESTS; i++) {
            if (jobs[i].status == 2 && jobs[i].res.aio_id == aio_id) { // DONE
                found = &jobs[i];
                break;
            }
        }
        
        if (found) {
            if (res) *res = found->res;
            found->status = 0; // Free slot
            spinlock_release(&aio_lock);
            return 0;
        }
        
        spinlock_release(&aio_lock);
        task_yield(); // Busy wait for now, better to sleep on a condvar
    }
}
