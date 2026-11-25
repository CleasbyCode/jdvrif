// Fault injection for jdvrif EINTR/stdout tests. JDVRIF_FAULT selects:
//   fsync-eintr      first fsync fails with EINTR, no signal (must retry)
//   fsync-sigint     first fsync raises SIGINT, then fails with EINTR
//   prefix-sigint    at the directory fsync after commit, fill the stdout pipe
//                    so the KEY-prefix flush blocks; SIGINT arrives 300 ms later
//   after-key-full   once the 29-byte KEY write completes, stdout becomes /dev/full
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static int fired;
static const char* mode(void) { const char* m = getenv("JDVRIF_FAULT"); return m ? m : ""; }
static void* late_sigint(void* arg) {
    (void)arg; struct timespec t = {0, 300000000}; nanosleep(&t, NULL); kill(getpid(), SIGINT); return NULL;
}
int fsync(int fd) {
    static int (*real)(int); if (!real) real = dlsym(RTLD_NEXT, "fsync");
    if (!fired && !strcmp(mode(), "fsync-eintr")) { fired = 1; errno = EINTR; return -1; }
    if (!fired && !strcmp(mode(), "fsync-sigint")) { fired = 1; raise(SIGINT); errno = EINTR; return -1; }
    struct stat st;
    if (!fired && !strcmp(mode(), "prefix-sigint") && fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        fired = 1;
        static ssize_t (*rw)(int, const void*, size_t); if (!rw) rw = dlsym(RTLD_NEXT, "write");
        const int flags = fcntl(1, F_GETFL); fcntl(1, F_SETFL, flags | O_NONBLOCK);
        static const char junk[512] = {0};
        while (rw(1, junk, sizeof junk) > 0) {}
        while (rw(1, junk, 1) > 0) {}
        fcntl(1, F_SETFL, flags);
        pthread_t t; pthread_create(&t, NULL, late_sigint, NULL); pthread_detach(t);
    }
    return real(fd);
}
ssize_t write(int fd, const void* buf, size_t n) {
    static ssize_t (*real)(int, const void*, size_t); if (!real) real = dlsym(RTLD_NEXT, "write");
    const ssize_t r = real(fd, buf, n);
    if (fd == 1 && !fired && !strcmp(mode(), "after-key-full") && n == 29 && r == 29) {
        const char* c = buf; int grouped = 1;
        for (int i = 4; i < 29; i += 5) grouped &= c[i] == '-';
        if (grouped) { fired = 1; const int full = open("/dev/full", O_WRONLY); dup2(full, 1); close(full); }
    }
    return r;
}
