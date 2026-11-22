/* Isolate I/O ownership from asynchronous initialization in media DLLs. */
#include "io_util.h"
#include "md_test.h"
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
static void pipe_pair(int fds[2]) { MD_CHECK(_pipe(fds, 4096, _O_BINARY | _O_NOINHERIT) == 0); }
static void read_cancel_and_reuse(void) {
    int fds[2]; pipe_pair(fds);
    intptr_t handle = _get_osfhandle(fds[0]);
    char byte = 0;
    MD_CHECK(md_fd_read_timeout(fds[0], &byte, 1, 10) == -1 && errno == EAGAIN);
    MD_CHECK(_get_osfhandle(fds[0]) == handle);
    MD_CHECK(_write(fds[1], "x", 1) == 1);
    MD_CHECK(md_fd_read_timeout(fds[0], &byte, 1, 2000) == 1 && byte == 'x');
    _close(fds[1]);
    MD_CHECK(md_fd_read_timeout(fds[0], &byte, 1, 2000) == 0);
    _close(fds[0]);
}
static void write_cancel(const char *large, bool broken) {
    int fds[2]; pipe_pair(fds);
    if (broken) _close(fds[0]);
    size_t committed = 0;
    int64_t start = md_now_ms();
    MD_CHECK(md_fd_write_deadline(fds[1], large, 1024 * 1024, start + 30, false, &committed) == -1);
    MD_CHECK(errno == (broken ? EPIPE : ETIMEDOUT));
    MD_CHECK(md_now_ms() - start < 5000);
    if (!broken) _close(fds[0]);
    _close(fds[1]);
}
int main(void) {
    /* Also exercises first-use thread/CRT initialization before counting
     * operation-owned handles. No success sleep or retry-to-green. */
    read_cancel_and_reuse();
    char *large = malloc(1024 * 1024); MD_CHECK(large != NULL);
    if (!large) return md_test_report();
    memset(large, 'x', 1024 * 1024);
    DWORD before, after;
    MD_CHECK(GetProcessHandleCount(GetCurrentProcess(), &before));
    for (int i = 0; i < 12; i++) {
        read_cancel_and_reuse();
        write_cancel(large, false); write_cancel(large, true);
    }
    MD_CHECK(GetProcessHandleCount(GetCurrentProcess(), &after));
    printf("isolated native I/O handles: before=%lu after=%lu\n", (unsigned long)before, (unsigned long)after);
    MD_CHECK(after == before);
    free(large);
    return md_test_report();
}
