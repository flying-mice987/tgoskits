/*
 * test-kcov-thorough.c — KCOV edge-case and stress test
 *
 * Tests /dev/kcov beyond the basic init/enable/disable/verify path:
 *
 *   1.  Open/close /dev/kcov
 *   2.  KCOV_INIT_TRACE: reject cover_size=0, cover_size>max; accept min/max
 *   3.  mmap rejection before INIT_TRACE; success after
 *   4.  KCOV_ENABLE rejection with invalid mode, without prior INIT_TRACE
 *   5.  KCOV_DISABLE before ENABLE (no-op, succeeds)
 *   6.  Double ENABLE without intervening DISABLE (idempotent)
 *   7.  Double INIT_TRACE replaces old buffer
 *   8.  Mode DISABLED produces no coverage; mode TRACE_CMP accepts but
 *       currently records nothing
 *   9.  Normal TRACE_PC flow: enable → heavy syscall stress → disable → count≥1
 *  10.  Buffer overflow: fill to KCOV_MAX_ENTRIES, verify count stops at max
 *  11.  Recorded PCs are in kernel address range (≥ 0xffff800000000000)
 *  12.  mmap buffer not accessible after close
 *  13.  Per-thread isolation: two threads have independent buffers
 *  14.  Close fd while tracing active cleans up without panic
 */

#define _GNU_SOURCE
#include "test_framework.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

/* ---- KCOV ioctl constants ---- */
#define KCOV_INIT_TRACE _IOR('c', 1, unsigned long)
#define KCOV_ENABLE     _IO('c', 100)
#define KCOV_DISABLE    _IO('c', 101)
#define KCOV_TRACE_PC   0x100
#define KCOV_TRACE_CMP  0x200
#define KCOV_MAX_ENTRIES 65536

/* ---- Helpers ---- */

static int open_kcov(void) {
    int fd = open("/dev/kcov", O_RDWR);
    if (fd < 0) {
        printf("SKIP: /dev/kcov not available (errno=%d: %s)\n",
               errno, strerror(errno));
        printf("      The kernel must be built with --features kcov\n");
        exit(0);
    }
    return fd;
}

static int init_trace(int fd, unsigned long cover_size) {
    return ioctl(fd, KCOV_INIT_TRACE, cover_size);
}

static int enable_trace(int fd, unsigned long mode) {
    return ioctl(fd, KCOV_ENABLE, mode);
}

static int disable_trace(int fd) {
    return ioctl(fd, KCOV_DISABLE, 0);
}

/* run a burst of syscalls to generate coverage traffic */
static void syscall_burst(int count) {
    for (volatile int i = 0; i < count; i++) {
        getpid();
        getuid();
        getppid();
        struct stat st;
        stat("/", &st);
        char buf[32];
        getcwd(buf, sizeof(buf));
        open("/dev/null", O_RDONLY); /* fd leak intentional for coverage */
    }
}

/* ---- Section 1: Device open / basic sanity ---- */

static void test_open_close(void) {
    int fd = open_kcov();
    CHECK(fd >= 0, "/dev/kcov opens successfully");
    CHECK_RET(close(fd), 0, "close /dev/kcov");
}

/* ---- Section 2: KCOV_INIT_TRACE boundary checks ---- */

static void test_init_trace_bounds(void) {
    int fd = open_kcov();

    /* zero size rejected */
    CHECK_ERR(init_trace(fd, 0), EINVAL, "KCOV_INIT_TRACE size=0 rejects");

    /* over max rejected */
    CHECK_ERR(init_trace(fd, KCOV_MAX_ENTRIES + 1), EINVAL,
              "KCOV_INIT_TRACE size>max rejects");

    /* size=1 accepted (minimum) */
    CHECK_RET(init_trace(fd, 1), 0, "KCOV_INIT_TRACE size=1 accepts");

    /* size=max accepted */
    CHECK_RET(init_trace(fd, KCOV_MAX_ENTRIES), 0,
              "KCOV_INIT_TRACE size=max accepts");

    close(fd);
}

/* ---- Section 3: mmap before INIT_TRACE rejected ---- */

static void test_mmap_before_init(void) {
    int fd = open_kcov();
    size_t sz = (1 + 256) * sizeof(uint64_t);
    void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(p == MAP_FAILED, "mmap before INIT_TRACE fails");
    close(fd);
}

/* ---- Section 4: KCOV_ENABLE rejection paths ---- */

static void test_enable_reject(void) {
    int fd = open_kcov();

    /* enable without init */
    CHECK_ERR(enable_trace(fd, KCOV_TRACE_PC), ENXIO,
              "ENABLE without INIT_TRACE fails (ENXIO)");

    /* init, then enable with invalid mode */
    CHECK_RET(init_trace(fd, 256), 0, "INIT_TRACE for enable-reject test");
    CHECK_ERR(enable_trace(fd, 0x999), EINVAL,
              "ENABLE with invalid mode 0x999 fails");
    CHECK_ERR(enable_trace(fd, 0), EINVAL,
              "ENABLE with mode=DISABLED(0) fails");

    close(fd);
}

/* ---- Section 5: DISABLE before ENABLE (no-op) ---- */

static void test_disable_before_enable(void) {
    int fd = open_kcov();
    CHECK_RET(init_trace(fd, 256), 0, "INIT_TRACE");
    CHECK_RET(disable_trace(fd), 0, "DISABLE before ENABLE succeeds (no-op)");
    /* after disable-before-enable, we can still enable */
    CHECK_RET(enable_trace(fd, KCOV_TRACE_PC), 0, "ENABLE after early disable");
    CHECK_RET(disable_trace(fd), 0, "DISABLE after enable");
    close(fd);
}

/* ---- Section 6: Double ENABLE (idempotent) ---- */

static void test_double_enable(void) {
    int fd = open_kcov();
    CHECK_RET(init_trace(fd, 256), 0, "INIT_TRACE");
    CHECK_RET(enable_trace(fd, KCOV_TRACE_PC), 0, "ENABLE first");
    CHECK_RET(enable_trace(fd, KCOV_TRACE_PC), 0,
              "ENABLE second (idempotent)");
    CHECK_RET(disable_trace(fd), 0, "DISABLE");
    close(fd);
}

/* ---- Section 7: Double INIT_TRACE replaces buffer ---- */

static void test_double_init(void) {
    int fd = open_kcov();

    CHECK_RET(init_trace(fd, 128), 0, "INIT_TRACE size=128");
    size_t sz1 = (1 + 128) * sizeof(uint64_t);
    uint64_t *buf1 = mmap(NULL, sz1, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK_PTR(buf1, 1, "mmap after first INIT succeeds");
    buf1[0] = 0xAB; /* dirty it */

    /* re-init with different size replaces buffer */
    CHECK_RET(init_trace(fd, 512), 0, "INIT_TRACE size=512 (replace)");
    munmap(buf1, sz1);

    size_t sz2 = (1 + 512) * sizeof(uint64_t);
    uint64_t *buf2 = mmap(NULL, sz2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK_PTR(buf2, 1, "mmap after second INIT succeeds");
    CHECK(buf2[0] == 0, "new buffer count starts at 0 (fresh buffer)");
    munmap(buf2, sz2);
    close(fd);
}

/* ---- Section 8: Mode behaviour ---- */

static void test_mode_disabled(void) {
    int fd = open_kcov();
    CHECK_RET(init_trace(fd, 256), 0, "INIT_TRACE");

    /* enable then immediately disable */
    CHECK_RET(enable_trace(fd, KCOV_TRACE_PC), 0, "ENABLE TRACE_PC");
    CHECK_RET(disable_trace(fd), 0, "DISABLE");

    /* After DISABLE, the mode is KCOV_MODE_DISABLED internally.
     * Run syscalls and check that count stays at 0 because mode=DISABLED. */
    size_t sz = (1 + 256) * sizeof(uint64_t);
    uint64_t *buf = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK_PTR(buf, 1, "mmap for disabled-mode test");
    uint64_t saved = buf[0];

    syscall_burst(50);

    CHECK(buf[0] == saved,
          "count unchanged after disable (mode DISABLED records nothing)");
    munmap(buf, sz);
    close(fd);
}

static void test_mode_trace_cmp(void) {
    int fd = open_kcov();
    CHECK_RET(init_trace(fd, 256), 0, "INIT_TRACE");
    /* TRACE_CMP is accepted by the ioctl even though recording is not yet
     * implemented for this mode. */
    CHECK_RET(enable_trace(fd, KCOV_TRACE_CMP), 0,
              "ENABLE TRACE_CMP accepted");
    CHECK_RET(disable_trace(fd), 0, "DISABLE");
    close(fd);
}

/* ---- Section 9: Normal trace flow with heavy stress ---- */

static void test_normal_trace(void) {
    int fd = open_kcov();
    CHECK_RET(init_trace(fd, 256), 0, "INIT_TRACE size=256");

    size_t sz = (1 + 256) * sizeof(uint64_t);
    uint64_t *buf = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK_PTR(buf, 1, "mmap succeeds");
    CHECK(buf[0] == 0, "initial count is 0");

    CHECK_RET(enable_trace(fd, KCOV_TRACE_PC), 0, "ENABLE TRACE_PC");

    /* heavy syscall stress to generate substantial coverage */
    syscall_burst(2000);

    CHECK_RET(disable_trace(fd), 0, "DISABLE");

    uint64_t count = buf[0];
    printf("  INFO: recorded %lu coverage entries\n", count);
    CHECK(count >= 1, "coverage count >= 1 after syscall stress");

    /* verify recorded PCs are in kernel address range */
    if (count >= 1) {
        int all_kernel = 1;
        for (uint64_t i = 1; i <= count && i <= 10; i++) {
            uint64_t pc = buf[i];
            /* StarryOS kernel is loaded at 0xffff800000000000+ */
            if (pc < 0xffff800000000000ULL) {
                printf("  WARN: buf[%lu]=0x%016lx not in kernel range\n",
                       i, pc);
                all_kernel = 0;
            }
        }
        CHECK(all_kernel, "recorded PCs are in kernel address range");
    }

    /* buffer still writable */
    buf[1] = 0xFEEDFACECAFEBEEFULL;
    CHECK(buf[1] == 0xFEEDFACECAFEBEEFULL,
          "mmap'd buffer writable after DISABLE");

    munmap(buf, sz);
    close(fd);
}

/* ---- Section 10: Buffer overflow ---- */

static void test_buffer_overflow(void) {
    /* Use a small buffer so we can overflow it quickly */
    int fd = open_kcov();
    CHECK_RET(init_trace(fd, 256), 0, "INIT_TRACE size=256");

    size_t sz = (1 + 256) * sizeof(uint64_t);
    uint64_t *buf = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK_PTR(buf, 1, "mmap succeeds");

    CHECK_RET(enable_trace(fd, KCOV_TRACE_PC), 0, "ENABLE");

    /* Run enough syscalls to fill the 256-entry buffer */
    syscall_burst(20000);

    CHECK_RET(disable_trace(fd), 0, "DISABLE");

    uint64_t count = buf[0];
    printf("  INFO: count after overflow attempt: %lu (max %d)\n",
           count, 256);
    CHECK(count <= 256, "count does not exceed buffer capacity");

    munmap(buf, sz);
    close(fd);
}

/* ---- Section 11: Close during active trace ---- */

static void test_close_during_active_trace(void) {
    int fd = open_kcov();
    CHECK_RET(init_trace(fd, 256), 0, "INIT_TRACE");

    size_t sz = (1 + 256) * sizeof(uint64_t);
    uint64_t *buf = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK_PTR(buf, 1, "mmap succeeds");

    CHECK_RET(enable_trace(fd, KCOV_TRACE_PC), 0, "ENABLE");

    /* run some syscalls to accumulate coverage */
    syscall_burst(100);

    /* close fd while trace is still active */
    munmap(buf, sz);
    CHECK_RET(close(fd), 0, "close while active");

    /* verify we can open a fresh kcov fd afterwards */
    int fd2 = open_kcov();
    CHECK_RET(init_trace(fd2, 64), 0, "INIT_TRACE on fresh fd after close");
    CHECK_RET(close(fd2), 0, "close fresh fd");
}

/* ---- Section 12: mmap after close fails ---- */

static void test_mmap_after_close(void) {
    int fd = open_kcov();
    CHECK_RET(init_trace(fd, 256), 0, "INIT_TRACE");

    size_t sz = (1 + 256) * sizeof(uint64_t);
    uint64_t *buf = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK_PTR(buf, 1, "mmap succeeds");
    munmap(buf, sz);

    CHECK_RET(close(fd), 0, "close fd");

    /* mmap on closed fd */
    void *bad = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(bad == MAP_FAILED, "mmap after close fails");
}

/* ---- Section 13: Per-thread isolation ---- */

typedef struct {
    int tid;
    uint64_t count;
    int ok;
} thread_result_t;

static void *thread_trace(void *arg) {
    thread_result_t *r = (thread_result_t *)arg;
    r->ok = 0;

    int fd = open("/dev/kcov", O_RDWR);
    if (fd < 0) {
        r->count = 0;
        r->ok = -1;
        return NULL;
    }

    if (init_trace(fd, 128) != 0) { close(fd); return NULL; }

    size_t sz = (1 + 128) * sizeof(uint64_t);
    uint64_t *buf = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (buf == MAP_FAILED) { close(fd); return NULL; }

    if (enable_trace(fd, KCOV_TRACE_PC) != 0) {
        munmap(buf, sz); close(fd); return NULL;
    }

    /* each thread runs its own syscall pattern */
    for (volatile int i = 0; i < 500; i++) {
        getpid();
        gettid();
    }

    disable_trace(fd);
    r->count = buf[0];
    r->tid = (int)gettid();

    munmap(buf, sz);
    close(fd);
    r->ok = 1;
    return NULL;
}

static void test_per_thread_isolation(void) {
    pthread_t t1, t2;
    thread_result_t r1 = {0}, r2 = {0};

    int rc1 = pthread_create(&t1, NULL, thread_trace, &r1);
    int rc2 = pthread_create(&t2, NULL, thread_trace, &r2);

    if (rc1 != 0 || rc2 != 0) {
        printf("  SKIP: pthread_create failed (rc1=%d rc2=%d)\n", rc1, rc2);
        return;
    }

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    CHECK(r1.ok == 1, "thread 1: kcov flow completed");
    CHECK(r2.ok == 1, "thread 2: kcov flow completed");

    printf("  INFO: thread %d recorded %lu entries\n", r1.tid, r1.count);
    printf("  INFO: thread %d recorded %lu entries\n", r2.tid, r2.count);

    /* both threads recorded coverage independently */
    CHECK(r1.count >= 1, "thread 1 recorded coverage");
    CHECK(r2.count >= 1, "thread 2 recorded coverage");
}

/* ---- Main ---- */

int main(void) {
    TEST_START("KCOV thorough edge-case suite");

    test_open_close();
    test_init_trace_bounds();
    test_mmap_before_init();
    test_enable_reject();
    test_disable_before_enable();
    test_double_enable();
    test_double_init();
    test_mode_disabled();
    test_mode_trace_cmp();
    test_normal_trace();
    test_buffer_overflow();
    test_close_during_active_trace();
    test_mmap_after_close();
    test_per_thread_isolation();

    TEST_DONE();
}
