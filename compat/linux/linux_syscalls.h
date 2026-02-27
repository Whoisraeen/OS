#ifndef LINUX_SYSCALLS_H
#define LINUX_SYSCALLS_H

/* Linux x86_64 Syscall Numbers — matches kernel ABI exactly */

/* I/O */
#define LINUX_SYS_READ           0
#define LINUX_SYS_WRITE          1
#define LINUX_SYS_OPEN           2
#define LINUX_SYS_CLOSE          3
#define LINUX_SYS_STAT           4
#define LINUX_SYS_FSTAT          5
#define LINUX_SYS_LSTAT          6
#define LINUX_SYS_POLL           7
#define LINUX_SYS_LSEEK          8
#define LINUX_SYS_MMAP           9
#define LINUX_SYS_MPROTECT       10
#define LINUX_SYS_MUNMAP         11
#define LINUX_SYS_BRK            12
#define LINUX_SYS_RT_SIGACTION   13
#define LINUX_SYS_RT_SIGPROCMASK 14
#define LINUX_SYS_RT_SIGRETURN   15
#define LINUX_SYS_IOCTL          16
#define LINUX_SYS_PREAD64        17
#define LINUX_SYS_PWRITE64       18
#define LINUX_SYS_READV          19
#define LINUX_SYS_WRITEV         20
#define LINUX_SYS_ACCESS         21
#define LINUX_SYS_PIPE           22
#define LINUX_SYS_SELECT         23
#define LINUX_SYS_SCHED_YIELD    24
#define LINUX_SYS_MREMAP         25
#define LINUX_SYS_MSYNC          26
#define LINUX_SYS_MINCORE        27
#define LINUX_SYS_MADVISE        28
#define LINUX_SYS_DUP            32
#define LINUX_SYS_DUP2           33
#define LINUX_SYS_PAUSE          34
#define LINUX_SYS_NANOSLEEP      35
#define LINUX_SYS_GETPID         39

/* Networking (stubs) */
#define LINUX_SYS_SOCKET         41
#define LINUX_SYS_CONNECT        42
#define LINUX_SYS_ACCEPT         43
#define LINUX_SYS_SENDTO         44
#define LINUX_SYS_RECVFROM       45
#define LINUX_SYS_SENDMSG        46
#define LINUX_SYS_RECVMSG        47
#define LINUX_SYS_SHUTDOWN       48
#define LINUX_SYS_BIND           49
#define LINUX_SYS_LISTEN         50
#define LINUX_SYS_GETSOCKNAME    51
#define LINUX_SYS_GETPEERNAME    52
#define LINUX_SYS_SOCKETPAIR     53
#define LINUX_SYS_SETSOCKOPT     54
#define LINUX_SYS_GETSOCKOPT     55

/* Process */
#define LINUX_SYS_CLONE          56
#define LINUX_SYS_FORK           57
#define LINUX_SYS_VFORK          58
#define LINUX_SYS_EXECVE         59
#define LINUX_SYS_EXIT           60
#define LINUX_SYS_WAIT4          61
#define LINUX_SYS_KILL           62

/* Misc */
#define LINUX_SYS_UNAME          63
#define LINUX_SYS_FCNTL          72
#define LINUX_SYS_FLOCK          73
#define LINUX_SYS_TRUNCATE       76
#define LINUX_SYS_FTRUNCATE      77
#define LINUX_SYS_GETDENTS       78
#define LINUX_SYS_GETCWD         79
#define LINUX_SYS_CHDIR          80
#define LINUX_SYS_FCHDIR         81
#define LINUX_SYS_RENAME         82
#define LINUX_SYS_MKDIR          83
#define LINUX_SYS_RMDIR          84
#define LINUX_SYS_CREAT          85
#define LINUX_SYS_LINK           86
#define LINUX_SYS_UNLINK         87
#define LINUX_SYS_SYMLINK        88
#define LINUX_SYS_READLINK       89
#define LINUX_SYS_CHMOD          90
#define LINUX_SYS_FCHMOD         91
#define LINUX_SYS_CHOWN          92
#define LINUX_SYS_FCHOWN         93
#define LINUX_SYS_UMASK          95
#define LINUX_SYS_GETTIMEOFDAY   96
#define LINUX_SYS_GETRLIMIT      97
#define LINUX_SYS_GETRUSAGE      98
#define LINUX_SYS_SYSINFO        99
#define LINUX_SYS_TIMES          100
#define LINUX_SYS_GETUID         102
#define LINUX_SYS_GETGID         104
#define LINUX_SYS_GETGROUPS      115
#define LINUX_SYS_SETGROUPS      116
#define LINUX_SYS_CAPGET         125
#define LINUX_SYS_CAPSET         126
#define LINUX_SYS_GETEUID        107
#define LINUX_SYS_GETEGID        108
#define LINUX_SYS_GETPPID        110
#define LINUX_SYS_GETPGRP        111
#define LINUX_SYS_SYSLOG         103
#define LINUX_SYS_SETUID         105
#define LINUX_SYS_SETGID         106
#define LINUX_SYS_SETPGID        109
#define LINUX_SYS_SETSID         112
#define LINUX_SYS_SIGRETURN      117
#define LINUX_SYS_MLOCK          149
#define LINUX_SYS_MUNLOCK        150
#define LINUX_SYS_MLOCKALL       151
#define LINUX_SYS_MUNLOCKALL     152
#define LINUX_SYS_SIGALTSTACK    131
#define LINUX_SYS_STATFS         137
#define LINUX_SYS_FSTATFS        138
#define LINUX_SYS_SCHED_SETPARAM          142
#define LINUX_SYS_SCHED_SETSCHEDULER      144
#define LINUX_SYS_SCHED_GETSCHEDULER      145
#define LINUX_SYS_SCHED_GETPARAM          143
#define LINUX_SYS_SCHED_GET_PRIORITY_MAX  146
#define LINUX_SYS_SCHED_GET_PRIORITY_MIN  147
#define LINUX_SYS_PRCTL          157
#define LINUX_SYS_ARCH_PRCTL     158
#define LINUX_SYS_GETTID         186
#define LINUX_SYS_TKILL          200
#define LINUX_SYS_FUTEX          202
#define LINUX_SYS_SET_TID_ADDRESS 218
#define LINUX_SYS_GETDENTS64     217
#define LINUX_SYS_CLOCK_GETTIME  228
#define LINUX_SYS_CLOCK_GETRES   229
#define LINUX_SYS_CLOCK_NANOSLEEP 230
#define LINUX_SYS_EXIT_GROUP     231
#define LINUX_SYS_TGKILL         234
#define LINUX_SYS_OPENAT         257
#define LINUX_SYS_MKDIRAT        258
#define LINUX_SYS_FSTATAT        262
#define LINUX_SYS_UNLINKAT       263
#define LINUX_SYS_RENAMEAT       264
#define LINUX_SYS_SET_ROBUST_LIST 273
#define LINUX_SYS_GET_ROBUST_LIST 274
#define LINUX_SYS_PRLIMIT64      302
#define LINUX_SYS_GETRANDOM      318
#define LINUX_SYS_RSEQ           334
#define LINUX_SYS_ACCEPT4        288
#define LINUX_SYS_DUP3           292
#define LINUX_SYS_PIPE2          293

/* Filesystem mounts */
#define LINUX_SYS_MOUNT          165
#define LINUX_SYS_UMOUNT2        166
#define LINUX_SYS_REBOOT         169

/* *at syscalls */
#define LINUX_SYS_FACCESSAT      269
#define LINUX_SYS_FACCESSAT2     439

/* epoll */
#define LINUX_SYS_EPOLL_CREATE   213
#define LINUX_SYS_EPOLL_CTL      233
#define LINUX_SYS_EPOLL_WAIT     232
#define LINUX_SYS_EPOLL_CREATE1  291
#define LINUX_SYS_EPOLL_PWAIT    281
#define LINUX_SYS_EPOLL_PWAIT2   441

/* eventfd / timerfd */
#define LINUX_SYS_EVENTFD        284
#define LINUX_SYS_EVENTFD2       290
#define LINUX_SYS_TIMERFD_CREATE  283
#define LINUX_SYS_TIMERFD_SETTIME 286
#define LINUX_SYS_TIMERFD_GETTIME 287

/* inotify */
#define LINUX_SYS_INOTIFY_INIT   253
#define LINUX_SYS_INOTIFY_INIT1  294
#define LINUX_SYS_INOTIFY_ADD_WATCH 254
#define LINUX_SYS_INOTIFY_RM_WATCH  255

/* fs sync / advisory */
#define LINUX_SYS_FSYNC          74
#define LINUX_SYS_FDATASYNC      75
#define LINUX_SYS_FADVISE64      221
#define LINUX_SYS_WAITID         247
#define LINUX_SYS_IOPRIO_SET     251
#define LINUX_SYS_IOPRIO_GET     252

/* misc */
#define LINUX_SYS_SENDFILE       40
#define LINUX_SYS_SOCKETPAIR     53
#define LINUX_SYS_READLINKAT     267
#define LINUX_SYS_SPLICE         275
#define LINUX_SYS_TEE            276
#define LINUX_SYS_VMSPLICE       278

#endif
