# RaeenOS Hybrid Kernel Architecture

## Overview

RaeenOS uses a **hybrid kernel architecture** combining the performance benefits of monolithic kernels with the stability and security benefits of microkernels. This design is inspired by Windows NT and macOS (XNU) but optimized for modern hardware.

## Architecture Layers

```
┌─────────────────────────────────────────────────────────────┐
│                    USER SPACE (Ring 3)                       │
├─────────────────────────────────────────────────────────────┤
│  Applications        │  System Services  │   User Drivers   │
│  - File Explorer     │  - Network Stack  │   - USB Stack    │
│  - Text Editor       │  - Audio Server   │   - Network      │
│  - Calculator        │  - Window Server  │   - Graphics     │
│  - Settings          │  - File System    │   - Storage      │
└─────────────────────────────────────────────────────────────┘
                          ↕ IPC (Messages, Shared Memory)
┌─────────────────────────────────────────────────────────────┐
│                    KERNEL SPACE (Ring 0)                     │
├─────────────────────────────────────────────────────────────┤
│  Core Kernel Services (Always in Kernel Space):             │
│  ┌──────────────┬──────────────┬──────────────┬───────────┐ │
│  │ Scheduler    │ IPC Manager  │ Security     │ VMM/PMM   │ │
│  │ (Preemptive) │ (Messages)   │ (Caps/Perms) │ (Paging)  │ │
│  └──────────────┴──────────────┴──────────────┴───────────┘ │
│                                                               │
│  Performance-Critical Drivers (Kernel Space):                │
│  ┌──────────────┬──────────────┬──────────────────────────┐ │
│  │ Timer (PIT)  │ Framebuffer  │ PS/2 Keyboard/Mouse      │ │
│  │ (IRQ0)       │ (Direct)     │ (IRQ1/12 - bootstrap)    │ │
│  └──────────────┴──────────────┴──────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
                          ↕ Hardware Abstraction
┌─────────────────────────────────────────────────────────────┐
│                         HARDWARE                             │
│  CPU │ Memory │ Disk │ Network │ USB │ Audio │ Display     │
└─────────────────────────────────────────────────────────────┘
```

## Core Kernel Components (Ring 0)

### 1. Memory Manager (vmm.c, pmm.c, heap.c)
- **Physical Memory Manager**: Page frame allocation (bitmap-based)
- **Virtual Memory Manager**: 4-level paging, per-process address spaces
- **Kernel Heap**: Dynamic memory allocation with guards
- **Features**:
  - Copy-on-write (COW) for fork
  - Demand paging with page fault handler
  - Memory-mapped files
  - Shared memory regions for IPC

### 2. Process Scheduler (sched.c)
- **Preemptive Multitasking**: Time-slice based (quantum: 10ms)
- **Priority-based Scheduling**: Real-time, high, normal, low, idle
- **CPU Affinity**: SMP support for multi-core systems
- **Features**:
  - O(1) scheduler with per-CPU run queues
  - Load balancing across cores
  - Thread support (lightweight processes)
  - Sleep/wake mechanisms

### 3. IPC Manager (ipc.c) **[NEW]**
- **Message Passing**: Synchronous and asynchronous messages
- **Ports**: Named endpoints for communication
- **Shared Memory**: Zero-copy data transfer for large payloads
- **Features**:
  - Port capabilities for security
  - Message queues with priorities
  - Timeout support
  - Notification system

### 4. Security Manager (security.c) **[NEW]**
- **Capability System**: Fine-grained permissions (macOS-style)
- **Sandboxing**: Process isolation with restricted capabilities
- **Access Control**: File, network, hardware access permissions
- **Features**:
  - Capability inheritance and delegation
  - Secure IPC with capability passing
  - Code signing verification
  - Exploit mitigations (ASLR, DEP, stack canaries)

### 5. System Call Interface (syscall.c)
- **Fast Path**: MSR-based SYSCALL/SYSRET (x86-64)
- **Security Checks**: Capability verification before operations
- **System Calls**:
  ```c
  // Process Management
  SYS_FORK, SYS_EXEC, SYS_EXIT, SYS_WAIT, SYS_YIELD

  // Memory
  SYS_MMAP, SYS_MUNMAP, SYS_MPROTECT, SYS_BRK

  // IPC
  SYS_PORT_CREATE, SYS_SEND_MESSAGE, SYS_RECV_MESSAGE
  SYS_SHMEM_CREATE, SYS_SHMEM_MAP

  // File I/O
  SYS_OPEN, SYS_READ, SYS_WRITE, SYS_CLOSE, SYS_SEEK

  // Capability
  SYS_CAP_GET, SYS_CAP_SET, SYS_CAP_CHECK
  ```

## User-Space Services (Ring 3)

### 1. Service Manager (services/manager.c)
- **Role**: Spawns, monitors, and restarts system services
- **Features**:
  - Service dependencies (start order)
  - Health monitoring (watchdog)
  - Automatic restart on crash
  - Capability assignment to services

### 2. User-Space Drivers
All moved to user space with IPC to kernel:

#### Storage Driver (services/drivers/ahci.c)
- **Communicates with**: Kernel via IPC for DMA setup
- **Provides**: Block device interface to file systems

#### Network Driver (services/drivers/e1000.c)
- **Communicates with**: Network stack via shared memory
- **Provides**: Packet send/receive interface

#### USB Driver (services/drivers/usb.c)
- **Communicates with**: Kernel for interrupts, DMA
- **Provides**: USB device enumeration and I/O

#### Audio Driver (services/drivers/hda.c)
- **Communicates with**: Audio server via shared memory
- **Provides**: PCM audio playback/recording

### 3. Network Stack (services/network/tcpip.c)
- **User-space TCP/IP implementation**
- **Features**: TCP, UDP, ICMP, ARP, DHCP, DNS
- **Socket API**: BSD sockets for applications

### 4. File Systems (services/fs/)
- **FAT32, ext2, RaeenFS**: Implemented as user-space services
- **VFS Proxy**: Kernel VFS forwards requests via IPC
- **Features**: Caching, journaling (RaeenFS)

### 5. Window Server (services/gui/compositor.c)
- **Compositing Window Manager**: Runs in user space
- **Communicates with**: Kernel framebuffer via shared memory
- **Features**: Window management, event dispatch, rendering

## Fault Isolation & Recovery

### Driver Crashes
- **Detection**: Service manager monitors via heartbeat
- **Action**: Kill crashed driver, restart with clean state
- **Impact**: System remains stable, only affected service disrupted

### Kernel Panic
- **Critical Errors**: Memory corruption, unexpected exceptions
- **Action**: Save crash dump, reboot (if ACPI available)
- **Prevention**: Kernel code is minimal and well-tested

### Security Boundaries
- **Ring 0 ↔ Ring 3**: Syscall interface with capability checks
- **Process ↔ Process**: IPC with port capabilities
- **Sandboxing**: Applications have restricted capabilities by default

## Performance Optimizations

### Fast Paths
1. **IPC Fastpath**: Kernel directly copies small messages (<128 bytes)
2. **Shared Memory**: Large data transfers avoid copying
3. **CPU Pinning**: Latency-sensitive services pinned to cores
4. **Zero-Copy I/O**: DMA directly to user buffers where safe

### Caching
- **Page Cache**: File system data cached in kernel
- **TLB Management**: Minimize context switch overhead
- **Message Queue**: Batch small messages for efficiency

## Comparison with Other Kernels

| Feature                  | Linux (Monolithic) | Windows NT (Hybrid) | macOS XNU (Hybrid) | RaeenOS (Hybrid) |
|--------------------------|--------------------|--------------------|---------------------|------------------|
| Core in Kernel Space     | Everything         | Core + HAL         | Mach + BSD         | Core + Critical  |
| Drivers Location         | Kernel space       | Mixed              | Mixed               | Mostly user space|
| IPC Mechanism            | Signals, pipes     | ALPC               | Mach ports          | Ports + shmem    |
| Security Model           | DAC + LSM          | ACLs + Tokens      | Capabilities        | Capabilities     |
| Scheduler                | CFS                | Priority-based     | Mach scheduler      | O(1) preemptive  |
| Fault Isolation          | Limited            | Good               | Good                | Excellent        |
| Performance              | Excellent          | Very Good          | Very Good           | Good (improving) |

## Boot Sequence

1. **Limine Bootloader** → Load kernel.elf
2. **Kernel Init** (kernel.c:_start)
   - Initialize GDT, IDT, PIC
   - Initialize PMM, VMM, heap
   - Initialize scheduler, IPC, security
   - Load initrd file system
3. **Bootstrap Drivers** (in kernel space temporarily)
   - PS/2 keyboard, mouse
   - Timer (PIT)
   - Framebuffer
4. **Service Manager Start** (first user-space process)
   - Reads /etc/services.conf
   - Spawns system services in order
5. **User-Space Drivers Load**
   - AHCI, USB, Network, Audio
   - Register with kernel via IPC
6. **Window Server Start**
   - Initialize compositor
   - Map framebuffer as shared memory
7. **Login Manager** → User session

## Development Roadmap

### Phase 1: IPC Foundation ✓
- [x] Design architecture
- [ ] Implement message passing IPC
- [ ] Implement shared memory API
- [ ] Port creation and management

### Phase 2: Service Infrastructure
- [ ] Service manager implementation
- [ ] Driver stub/proxy pattern
- [ ] Service health monitoring
- [ ] Automatic restart logic

### Phase 3: Driver Migration
- [ ] Move PS/2 drivers to user space (keep kernel stub)
- [ ] Move storage drivers to user space
- [ ] Move network drivers to user space
- [ ] Move audio drivers to user space

### Phase 4: Security Hardening
- [ ] Capability system implementation
- [ ] Sandboxing enforcement
- [ ] Code signing infrastructure
- [ ] Exploit mitigations (ASLR, etc.)

### Phase 5: Performance Tuning
- [ ] IPC fastpath optimization
- [ ] Zero-copy I/O implementation
- [ ] CPU affinity and pinning
- [ ] Profiling and optimization

## Security Model: Capabilities

### Capability Types
```c
typedef enum {
    CAP_NONE           = 0,
    // File System
    CAP_FILE_READ      = (1 << 0),
    CAP_FILE_WRITE     = (1 << 1),
    CAP_FILE_EXECUTE   = (1 << 2),
    // Network
    CAP_NET_LISTEN     = (1 << 3),
    CAP_NET_CONNECT    = (1 << 4),
    CAP_NET_RAW        = (1 << 5),
    // Hardware
    CAP_HW_DISK        = (1 << 6),
    CAP_HW_USB         = (1 << 7),
    CAP_HW_AUDIO       = (1 << 8),
    CAP_HW_VIDEO       = (1 << 9),
    // IPC
    CAP_IPC_SEND       = (1 << 10),
    CAP_IPC_RECV       = (1 << 11),
    CAP_IPC_CREATE     = (1 << 12),
    // System
    CAP_SYS_ADMIN      = (1 << 13),
    CAP_SYS_REBOOT     = (1 << 14),
    CAP_SYS_CHROOT     = (1 << 15),
} capability_t;
```

### Default Capabilities
- **Applications**: FILE_READ, IPC_SEND, IPC_RECV
- **System Services**: FILE_READ, FILE_WRITE, IPC_* , specific HW_*
- **Drivers**: Specific HW_* for their hardware only
- **Root/Admin**: CAP_SYS_ADMIN (can grant other capabilities)

### Capability Checks
Every system call checks capabilities before execution:
```c
if (!has_capability(current_process, CAP_FILE_WRITE)) {
    return -EPERM;
}
```

## Conclusion

This hybrid kernel architecture provides:
- **Stability**: Driver crashes don't bring down the system
- **Security**: Fine-grained capability-based access control
- **Performance**: Critical paths in kernel, bulk in user space
- **Maintainability**: Services can be updated without kernel changes
- **Scalability**: SMP support with per-CPU data structures

RaeenOS aims to match Windows NT's reliability while adopting macOS's elegant security model, all while being open and understandable.
