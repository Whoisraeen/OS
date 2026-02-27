#ifndef PROCFS_H
#define PROCFS_H

// Initialize /proc virtual filesystem and mount it at /proc.
// Call after vfs_root is set up.
void procfs_init(void);

#endif // PROCFS_H
