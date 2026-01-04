#pragma once

/* Minimal x86_64 Linux syscall wrappers (no libc). */

typedef unsigned long ulong;
typedef unsigned int uint;
typedef unsigned short ushort;
typedef unsigned char uchar;

typedef unsigned long size_t;
typedef long ssize_t;
typedef long off_t;
typedef long intptr_t;
typedef unsigned long uintptr_t;

typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;
typedef unsigned long long uint64_t;

typedef int int32_t;
typedef long long int64_t;

enum {
	SYS_poll = 7,
	SYS_read = 0,
	SYS_write = 1,
	SYS_open = 2,
	SYS_close = 3,
	SYS_wait4 = 61,
	SYS_kill = 62,
	SYS_fork = 57,
	SYS_socket = 41,
	SYS_connect = 42,
	SYS_sendto = 44,
	SYS_recvfrom = 45,
	SYS_setsockopt = 54,
	SYS_getsockopt = 55,
	SYS_getrandom = 318,
	SYS_mmap = 9,
	SYS_munmap = 11,
	SYS_nanosleep = 35,
	SYS_ioctl = 16,
	SYS_ftruncate = 77,
	SYS_openat = 257,
	SYS_clock_gettime = 228,
	SYS_exit = 60,
};

enum {
	AT_FDCWD = -100,
};

enum {
	SIGKILL = 9,
	WNOHANG = 1,
};

enum {
	O_RDONLY = 00,
	O_WRONLY = 01,
	O_RDWR = 02,
	O_CREAT = 0100,
	O_TRUNC = 01000,
	O_NONBLOCK = 04000,
};

enum {
	/* From asm-generic/ioctls.h */
	FIONBIO = 0x5421,
};

enum {
	/* Minimal errno values we need (Linux). */
	EINPROGRESS = 115,
};

enum {
	PROT_READ = 0x1,
	PROT_WRITE = 0x2,
};

enum {
	MAP_SHARED = 0x01,
	MAP_PRIVATE = 0x02,
	MAP_ANONYMOUS = 0x20,
};

#define MAP_FAILED ((void *)-1)

struct timespec {
	int64_t tv_sec;
	int64_t tv_nsec;
};

static inline long sys_call0(long n)
{
	long r;
	__asm__ volatile("syscall" : "=a"(r) : "a"(n) : "rcx", "r11", "memory");
	return r;
}

static inline long sys_call1(long n, long a1)
{
	long r;
	__asm__ volatile("syscall"
			 : "=a"(r)
			 : "a"(n), "D"(a1)
			 : "rcx", "r11", "memory");
	return r;
}

static inline long sys_call2(long n, long a1, long a2)
{
	long r;
	__asm__ volatile("syscall"
			 : "=a"(r)
			 : "a"(n), "D"(a1), "S"(a2)
			 : "rcx", "r11", "memory");
	return r;
}

static inline long sys_call3(long n, long a1, long a2, long a3)
{
	long r;
	__asm__ volatile("syscall"
			 : "=a"(r)
			 : "a"(n), "D"(a1), "S"(a2), "d"(a3)
			 : "rcx", "r11", "memory");
	return r;
}

static inline long sys_call4(long n, long a1, long a2, long a3, long a4)
{
	long r;
	register long r10 __asm__("r10") = a4;
	__asm__ volatile("syscall"
			 : "=a"(r)
			 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
			 : "rcx", "r11", "memory");
	return r;
}

static inline long sys_call5(long n, long a1, long a2, long a3, long a4, long a5)
{
	long r;
	register long r10 __asm__("r10") = a4;
	register long r8 __asm__("r8") = a5;
	__asm__ volatile("syscall"
			 : "=a"(r)
			 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
			 : "rcx", "r11", "memory");
	return r;
}

static inline long sys_call6(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
	long r;
	register long r10 __asm__("r10") = a4;
	register long r8 __asm__("r8") = a5;
	register long r9 __asm__("r9") = a6;
	__asm__ volatile("syscall"
			 : "=a"(r)
			 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
			 : "rcx", "r11", "memory");
	return r;
}

static inline ssize_t sys_write(int fd, const void *buf, size_t count)
{
	return (ssize_t)sys_call3(SYS_write, (long)fd, (long)buf, (long)count);
}

static inline ssize_t sys_read(int fd, void *buf, size_t count)
{
	return (ssize_t)sys_call3(SYS_read, (long)fd, (long)buf, (long)count);
}

static inline int sys_openat(int dirfd, const char *path, int flags, int mode)
{
	return (int)sys_call4(SYS_openat, (long)dirfd, (long)path, (long)flags, (long)mode);
}

static inline int sys_close(int fd)
{
	return (int)sys_call1(SYS_close, (long)fd);
}

static inline int sys_kill(int pid, int sig)
{
	return (int)sys_call2(SYS_kill, (long)pid, (long)sig);
}

static inline int sys_wait4(int pid, int *wstatus, int options, void *rusage)
{
	return (int)sys_call4(SYS_wait4, (long)pid, (long)wstatus, (long)options, (long)rusage);
}

static inline int sys_fork(void)
{
	return (int)sys_call0(SYS_fork);
}

static inline int sys_socket(int domain, int type, int protocol)
{
	return (int)sys_call3(SYS_socket, (long)domain, (long)type, (long)protocol);
}

static inline int sys_connect(int fd, const void *addr, uint32_t addrlen)
{
	return (int)sys_call3(SYS_connect, (long)fd, (long)addr, (long)addrlen);
}

static inline ssize_t sys_sendto(int fd, const void *buf, size_t len, int flags, const void *dest, uint32_t addrlen)
{
	return (ssize_t)sys_call6(SYS_sendto, (long)fd, (long)buf, (long)len, (long)flags, (long)dest, (long)addrlen);
}

static inline ssize_t sys_recvfrom(int fd, void *buf, size_t len, int flags, void *src, uint32_t *addrlen)
{
	return (ssize_t)sys_call6(SYS_recvfrom, (long)fd, (long)buf, (long)len, (long)flags, (long)src, (long)addrlen);
}

static inline long sys_getrandom(void *buf, size_t len, uint32_t flags)
{
	return sys_call3(SYS_getrandom, (long)buf, (long)len, (long)flags);
}

static inline int sys_setsockopt(int fd, int level, int optname, const void *optval, uint32_t optlen)
{
	return (int)sys_call5(SYS_setsockopt, (long)fd, (long)level, (long)optname, (long)optval, (long)optlen);
}

typedef ulong nfds_t;

struct pollfd {
	int fd;
	short events;
	short revents;
};

enum {
	POLLIN = 0x0001,
	POLLOUT = 0x0004,
	POLLERR = 0x0008,
	POLLHUP = 0x0010,
};

static inline int sys_poll(struct pollfd *fds, nfds_t nfds, int timeout_ms)
{
	return (int)sys_call3(SYS_poll, (long)fds, (long)nfds, (long)timeout_ms);
}

static inline int sys_getsockopt(int fd, int level, int optname, void *optval, uint32_t *optlen)
{
	return (int)sys_call5(SYS_getsockopt, (long)fd, (long)level, (long)optname, (long)optval, (long)optlen);
}

static inline int sys_ftruncate(int fd, off_t length)
{
	return (int)sys_call2(SYS_ftruncate, (long)fd, (long)length);
}

static inline void *sys_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	return (void *)sys_call6(SYS_mmap, (long)addr, (long)length, (long)prot, (long)flags, (long)fd, (long)offset);
}

static inline int sys_munmap(void *addr, size_t length)
{
	return (int)sys_call2(SYS_munmap, (long)addr, (long)length);
}

static inline int sys_nanosleep(const struct timespec *req, struct timespec *rem)
{
	return (int)sys_call2(SYS_nanosleep, (long)req, (long)rem);
}

static inline int sys_ioctl(int fd, ulong request, void *argp)
{
	return (int)sys_call3(SYS_ioctl, (long)fd, (long)request, (long)argp);
}

static inline void sys_exit(int code)
{
	sys_call1(SYS_exit, (long)code);
	__builtin_unreachable();
}

static inline size_t c_strlen(const char *s)
{
	size_t n = 0;
	while (s[n] != 0) {
		n++;
	}
	return n;
}

static inline void dbg_write(const char *s)
{
	sys_write(2, s, c_strlen(s));
}
