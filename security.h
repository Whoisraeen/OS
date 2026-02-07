#ifndef SECURITY_H
#define SECURITY_H

#include <stdint.h>
#include <stdbool.h>

// Capability Bits (fine-grained permissions)
typedef enum {
    // File System Capabilities
    CAP_FILE_READ          = (1ULL << 0),   // Read files
    CAP_FILE_WRITE         = (1ULL << 1),   // Write/modify files
    CAP_FILE_EXECUTE       = (1ULL << 2),   // Execute programs
    CAP_FILE_CREATE        = (1ULL << 3),   // Create new files
    CAP_FILE_DELETE        = (1ULL << 4),   // Delete files
    CAP_FILE_CHOWN         = (1ULL << 5),   // Change file ownership

    // Network Capabilities
    CAP_NET_BIND           = (1ULL << 10),  // Bind to network ports
    CAP_NET_CONNECT        = (1ULL << 11),  // Connect to remote hosts
    CAP_NET_RAW            = (1ULL << 12),  // Raw socket access
    CAP_NET_PACKET         = (1ULL << 13),  // Packet-level network access

    // Hardware Capabilities
    CAP_HW_DISK            = (1ULL << 20),  // Direct disk access
    CAP_HW_USB             = (1ULL << 21),  // USB device access
    CAP_HW_AUDIO           = (1ULL << 22),  // Audio hardware access
    CAP_HW_VIDEO           = (1ULL << 23),  // Video/framebuffer access
    CAP_HW_INPUT           = (1ULL << 24),  // Input devices (keyboard, mouse)
    CAP_HW_SERIAL          = (1ULL << 25),  // Serial port access

    // IPC Capabilities
    CAP_IPC_CREATE         = (1ULL << 30),  // Create IPC ports
    CAP_IPC_SEND           = (1ULL << 31),  // Send messages
    CAP_IPC_RECV           = (1ULL << 32),  // Receive messages
    CAP_IPC_SHMEM          = (1ULL << 33),  // Shared memory access

    // Process Management Capabilities
    CAP_PROC_FORK          = (1ULL << 40),  // Fork processes
    CAP_PROC_EXEC          = (1ULL << 41),  // Execute programs
    CAP_PROC_KILL          = (1ULL << 42),  // Kill other processes
    CAP_PROC_SETPRIORITY   = (1ULL << 43),  // Change process priority
    CAP_PROC_PTRACE        = (1ULL << 44),  // Debug/trace processes

    // System Capabilities
    CAP_SYS_ADMIN          = (1ULL << 50),  // System administration
    CAP_SYS_MODULE         = (1ULL << 51),  // Load kernel modules/drivers
    CAP_SYS_REBOOT         = (1ULL << 52),  // Reboot system
    CAP_SYS_TIME           = (1ULL << 53),  // Set system time
    CAP_SYS_CHROOT         = (1ULL << 54),  // Change root directory
    CAP_SYS_RESOURCE       = (1ULL << 55),  // Override resource limits

    // Security Capabilities
    CAP_SEC_SETUID         = (1ULL << 60),  // Change user ID
    CAP_SEC_SETGID         = (1ULL << 61),  // Change group ID
    CAP_SEC_CAP_GRANT      = (1ULL << 62),  // Grant capabilities to others
} capability_t;

// Capability Set (64-bit mask)
typedef uint64_t capset_t;

// Process Security Context
typedef struct {
    uint32_t pid;               // Process ID
    uint32_t uid;               // User ID (0 = root)
    uint32_t gid;               // Group ID
    capset_t capabilities;      // Active capabilities
    capset_t inheritable;       // Capabilities inherited by child processes
    bool sandboxed;             // Is process in sandbox?

    // Sandboxing restrictions (if sandboxed = true)
    char sandbox_root[256];     // Chroot path for filesystem access
    uint32_t max_memory_kb;     // Maximum memory allocation
    uint32_t max_open_files;    // Maximum open file descriptors
    bool allow_network;         // Can access network
} security_context_t;

// Pre-defined Capability Sets

// Full privileges (for kernel/init)
#define CAPSET_KERNEL    (~0ULL)

// Administrator privileges
#define CAPSET_ADMIN     (CAP_FILE_READ | CAP_FILE_WRITE | CAP_FILE_EXECUTE | \
                          CAP_FILE_CREATE | CAP_FILE_DELETE | \
                          CAP_NET_BIND | CAP_NET_CONNECT | \
                          CAP_IPC_CREATE | CAP_IPC_SEND | CAP_IPC_RECV | \
                          CAP_PROC_FORK | CAP_PROC_EXEC | CAP_PROC_KILL | \
                          CAP_SYS_ADMIN | CAP_SYS_REBOOT | \
                          CAP_SEC_CAP_GRANT)

// System service privileges (for drivers, network stack, etc.)
#define CAPSET_SERVICE   (CAP_FILE_READ | CAP_FILE_WRITE | \
                          CAP_IPC_CREATE | CAP_IPC_SEND | CAP_IPC_RECV | CAP_IPC_SHMEM | \
                          CAP_PROC_FORK | CAP_SYS_MODULE)

// Regular user application
#define CAPSET_USER      (CAP_FILE_READ | CAP_FILE_EXECUTE | \
                          CAP_IPC_SEND | CAP_IPC_RECV | \
                          CAP_PROC_FORK | CAP_PROC_EXEC)

// Sandboxed application (minimal privileges)
#define CAPSET_SANDBOX   (CAP_FILE_READ | CAP_IPC_SEND | CAP_IPC_RECV)

// === Security API ===

// Initialize security subsystem
void security_init(void);

// Create security context for new process
security_context_t *security_create_context(uint32_t pid, uint32_t parent_pid);

// Get security context for process
security_context_t *security_get_context(uint32_t pid);

// Check if process has capability
bool security_has_capability(uint32_t pid, capability_t cap);

// Grant capability to process (requires CAP_SEC_CAP_GRANT)
int security_grant_capability(uint32_t granter_pid, uint32_t target_pid, capability_t cap);

// Revoke capability from process
int security_revoke_capability(uint32_t revoker_pid, uint32_t target_pid, capability_t cap);

// Enable sandbox mode for process
int security_enable_sandbox(uint32_t pid, const char *root_path);

// Check if operation is allowed
bool security_check_file_access(uint32_t pid, const char *path, capability_t required_cap);
bool security_check_ipc_access(uint32_t pid, uint32_t dest_port);
bool security_check_hardware_access(uint32_t pid, capability_t hardware_cap);

// Destroy security context (when process exits)
void security_destroy_context(uint32_t pid);

// === Debugging ===

// Print security context for process
void security_debug_print_context(uint32_t pid);

// Print all processes with specific capability
void security_debug_list_cap(capability_t cap);

#endif // SECURITY_H
