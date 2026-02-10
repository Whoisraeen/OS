#include "security.h"
#include "serial.h"
#include <stddef.h>

// Maximum number of processes we track
#define MAX_PROCESSES 256

// Global security contexts array
static security_context_t security_contexts[MAX_PROCESSES];
static bool security_initialized = false;

// ========================================================================
// Initialization
// ========================================================================

void security_init(void) {
    // Initialize all contexts as unused
    for (int i = 0; i < MAX_PROCESSES; i++) {
        security_contexts[i].pid = 0;  // 0 means unused
        security_contexts[i].uid = 0;
        security_contexts[i].gid = 0;
        security_contexts[i].capabilities = 0;
        security_contexts[i].inheritable = 0;
        security_contexts[i].sandboxed = false;
        security_contexts[i].sandbox_root[0] = '/';
        security_contexts[i].sandbox_root[1] = '\0';
        security_contexts[i].max_memory_kb = 0;
        security_contexts[i].max_open_files = 0;
        security_contexts[i].allow_network = true;
    }

    // Create security context for kernel (PID 0)
    security_contexts[0].pid = 0;
    security_contexts[0].uid = 0;  // Root
    security_contexts[0].gid = 0;
    security_contexts[0].capabilities = CAPSET_KERNEL;  // All capabilities
    security_contexts[0].inheritable = CAPSET_KERNEL;
    security_contexts[0].sandboxed = false;

    security_initialized = true;
    kprintf("[SECURITY] Initialized with capability-based model\n");
    kprintf("[SECURITY] Kernel context (PID 0) has full privileges\n");
}

// ========================================================================
// Context Management
// ========================================================================

security_context_t *security_create_context(uint32_t pid, uint32_t parent_pid) {
    if (!security_initialized) {
        kprintf("[SECURITY] Error: Not initialized\n");
        return NULL;
    }

    if (pid == 0 || pid >= MAX_PROCESSES) {
        kprintf("[SECURITY] Error: Invalid PID %d\n", pid);
        return NULL;
    }

    // Get parent's context for capability inheritance
    security_context_t *parent_ctx = NULL;
    if (parent_pid < MAX_PROCESSES && security_contexts[parent_pid].pid == parent_pid) {
        parent_ctx = &security_contexts[parent_pid];
    }

    // Initialize new context
    security_context_t *ctx = &security_contexts[pid];
    ctx->pid = pid;

    if (parent_ctx) {
        // Inherit from parent
        ctx->uid = parent_ctx->uid;
        ctx->gid = parent_ctx->gid;
        ctx->capabilities = parent_ctx->inheritable;  // Only inheritable caps
        ctx->inheritable = parent_ctx->inheritable;
        ctx->sandboxed = parent_ctx->sandboxed;

        // Copy sandbox settings
        if (parent_ctx->sandboxed) {
            for (int i = 0; i < 256 && parent_ctx->sandbox_root[i]; i++) {
                ctx->sandbox_root[i] = parent_ctx->sandbox_root[i];
            }
            ctx->max_memory_kb = parent_ctx->max_memory_kb;
            ctx->max_open_files = parent_ctx->max_open_files;
            ctx->allow_network = parent_ctx->allow_network;
        }

        kprintf("[SECURITY] Created context for PID %d (parent=%d, caps=0x%lx)\n",
                pid, parent_pid, ctx->capabilities);
    } else {
        // No parent or kernel is parent - start with minimal privileges
        ctx->uid = 1000;  // Regular user
        ctx->gid = 1000;
        ctx->capabilities = CAPSET_USER;
        ctx->inheritable = CAPSET_USER;
        ctx->sandboxed = false;

        kprintf("[SECURITY] Created context for PID %d (no parent, default user caps)\n", pid);
    }

    return ctx;
}

security_context_t *security_get_context(uint32_t pid) {
    if (pid >= MAX_PROCESSES || security_contexts[pid].pid != pid) {
        return NULL;
    }
    return &security_contexts[pid];
}

void security_destroy_context(uint32_t pid) {
    if (pid >= MAX_PROCESSES) {
        return;
    }

    security_contexts[pid].pid = 0;  // Mark as unused
    security_contexts[pid].capabilities = 0;
    kprintf("[SECURITY] Destroyed context for PID %d\n", pid);
}

// ========================================================================
// Capability Checks
// ========================================================================

bool security_has_capability(uint32_t pid, capability_t cap) {
    security_context_t *ctx = security_get_context(pid);
    if (!ctx) {
        // Unknown process - deny by default
        return false;
    }

    // Kernel (PID 0) always has all capabilities
    if (pid == 0) {
        return true;
    }
    
    // Service Manager (PID 1) also gets full privileges (or at least Admin)
    // This allows it to grant capabilities to other services
    if (pid == 1) {
        return true;
    }

    // Check if capability bit is set
    return (ctx->capabilities & cap) != 0;
}

int security_grant_capability(uint32_t granter_pid, uint32_t target_pid, capability_t cap) {
    // Check if granter has permission to grant capabilities
    if (!security_has_capability(granter_pid, CAP_SEC_CAP_GRANT)) {
        kprintf("[SECURITY] PID %d denied: Cannot grant capabilities (missing CAP_SEC_CAP_GRANT)\n",
                granter_pid);
        return -1;  // Permission denied
    }

    security_context_t *target_ctx = security_get_context(target_pid);
    if (!target_ctx) {
        return -2;  // Target process not found
    }

    // Grant the capability
    target_ctx->capabilities |= cap;
    target_ctx->inheritable |= cap;  // Also make it inheritable

    kprintf("[SECURITY] PID %d granted capability 0x%lx to PID %d\n",
            granter_pid, (uint64_t)cap, target_pid);
    return 0;
}

int security_revoke_capability(uint32_t revoker_pid, uint32_t target_pid, capability_t cap) {
    // Only kernel or capability granter can revoke
    if (revoker_pid != 0 && !security_has_capability(revoker_pid, CAP_SEC_CAP_GRANT)) {
        kprintf("[SECURITY] PID %d denied: Cannot revoke capabilities\n", revoker_pid);
        return -1;
    }

    security_context_t *target_ctx = security_get_context(target_pid);
    if (!target_ctx) {
        return -2;
    }

    // Revoke the capability
    target_ctx->capabilities &= ~cap;
    target_ctx->inheritable &= ~cap;

    kprintf("[SECURITY] PID %d revoked capability 0x%lx from PID %d\n",
            revoker_pid, (uint64_t)cap, target_pid);
    return 0;
}

// ========================================================================
// Sandboxing
// ========================================================================

int security_enable_sandbox(uint32_t pid, const char *root_path) {
    security_context_t *ctx = security_get_context(pid);
    if (!ctx) {
        return -1;
    }

    ctx->sandboxed = true;

    // Set sandbox root path
    int i;
    for (i = 0; i < 255 && root_path[i] != '\0'; i++) {
        ctx->sandbox_root[i] = root_path[i];
    }
    ctx->sandbox_root[i] = '\0';

    // Restrict capabilities in sandbox
    ctx->capabilities &= CAPSET_SANDBOX;
    ctx->inheritable &= CAPSET_SANDBOX;

    // Set default limits
    ctx->max_memory_kb = 16384;  // 16 MB max
    ctx->max_open_files = 16;
    ctx->allow_network = false;  // No network by default in sandbox

    kprintf("[SECURITY] PID %d entered sandbox (root=%s)\n", pid, root_path);
    return 0;
}

// ========================================================================
// Access Control Checks
// ========================================================================

bool security_check_file_access(uint32_t pid, const char *path, capability_t required_cap) {
    security_context_t *ctx = security_get_context(pid);
    if (!ctx) {
        return false;
    }

    // Check capability
    if (!security_has_capability(pid, required_cap)) {
        kprintf("[SECURITY] PID %d denied file access to '%s' (missing cap 0x%lx)\n",
                pid, path, (uint64_t)required_cap);
        return false;
    }

    // If sandboxed, check path restrictions
    if (ctx->sandboxed) {
        // Simple check: path must start with sandbox_root
        int i = 0;
        while (ctx->sandbox_root[i] != '\0' && path[i] != '\0') {
            if (ctx->sandbox_root[i] != path[i]) {
                kprintf("[SECURITY] PID %d denied: Path '%s' outside sandbox '%s'\n",
                        pid, path, ctx->sandbox_root);
                return false;
            }
            i++;
        }
    }

    return true;
}

bool security_check_ipc_access(uint32_t pid, uint32_t dest_port) {
    // For now, allow all IPC if process has CAP_IPC_SEND
    // In full implementation, check port-specific capabilities
    return security_has_capability(pid, CAP_IPC_SEND);
}

bool security_check_hardware_access(uint32_t pid, capability_t hardware_cap) {
    security_context_t *ctx = security_get_context(pid);
    if (!ctx) {
        return false;
    }

    // Sandboxed processes cannot access hardware directly
    if (ctx->sandboxed) {
        kprintf("[SECURITY] PID %d denied hardware access (sandboxed)\n", pid);
        return false;
    }

    return security_has_capability(pid, hardware_cap);
}

// ========================================================================
// Debugging
// ========================================================================

void security_debug_print_context(uint32_t pid) {
    security_context_t *ctx = security_get_context(pid);
    if (!ctx) {
        kprintf("[SECURITY] No context for PID %d\n", pid);
        return;
    }

    kprintf("\n[SECURITY] Context for PID %d:\n", pid);
    kprintf("  UID: %d, GID: %d\n", ctx->uid, ctx->gid);
    kprintf("  Capabilities: 0x%016lx\n", ctx->capabilities);
    kprintf("  Inheritable:  0x%016lx\n", ctx->inheritable);
    kprintf("  Sandboxed: %s\n", ctx->sandboxed ? "YES" : "NO");

    if (ctx->sandboxed) {
        kprintf("  Sandbox root: %s\n", ctx->sandbox_root);
        kprintf("  Max memory: %d KB\n", ctx->max_memory_kb);
        kprintf("  Max files: %d\n", ctx->max_open_files);
        kprintf("  Network: %s\n", ctx->allow_network ? "YES" : "NO");
    }

    // Print individual capabilities
    kprintf("  Active capabilities:\n");
    if (ctx->capabilities & CAP_FILE_READ)    kprintf("    - FILE_READ\n");
    if (ctx->capabilities & CAP_FILE_WRITE)   kprintf("    - FILE_WRITE\n");
    if (ctx->capabilities & CAP_FILE_EXECUTE) kprintf("    - FILE_EXECUTE\n");
    if (ctx->capabilities & CAP_NET_BIND)     kprintf("    - NET_BIND\n");
    if (ctx->capabilities & CAP_NET_CONNECT)  kprintf("    - NET_CONNECT\n");
    if (ctx->capabilities & CAP_IPC_CREATE)   kprintf("    - IPC_CREATE\n");
    if (ctx->capabilities & CAP_IPC_SEND)     kprintf("    - IPC_SEND\n");
    if (ctx->capabilities & CAP_IPC_RECV)     kprintf("    - IPC_RECV\n");
    if (ctx->capabilities & CAP_SYS_ADMIN)    kprintf("    - SYS_ADMIN\n");
}

void security_debug_list_cap(capability_t cap) {
    kprintf("\n[SECURITY] Processes with capability 0x%lx:\n", (uint64_t)cap);

    bool found = false;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (security_contexts[i].pid != 0 && (security_contexts[i].capabilities & cap)) {
            kprintf("  PID %d (UID %d)\n", i, security_contexts[i].uid);
            found = true;
        }
    }

    if (!found) {
        kprintf("  (none)\n");
    }
}
