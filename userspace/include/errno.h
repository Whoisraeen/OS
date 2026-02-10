#ifndef _ERRNO_H
#define _ERRNO_H
extern int errno;
#define EINTR 4
#define EAGAIN 11
#define EWOULDBLOCK EAGAIN
#endif
