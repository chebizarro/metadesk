#include "managed_launch.h"
#include "management_ipc.h"
#include "session.h"
#include "md_test.h"
#include "md_time.h"
#include <dispatch/dispatch.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define KEY "abababababababababababababababababababababababababababababababab"
#define OTHER_KEY "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd"
#define SENTINEL 101
#define STALL_MARKER "/fixture/stalled-launch-reader"

/* Stop on the first failure; later steps require a live peer and valid fds. */
#define REQUIRE(cond) do { MD_CHECK(cond); if (md_test_failures) return md_test_report(); } while (0)

/* This peer never reads configuration. After checking the exec state it
 * waits for SIGTERM just as the client waits on a dedicated signal thread. */
static int stalled_peer(void) {
    sigset_t mask;
    REQUIRE(!pthread_sigmask(SIG_SETMASK, NULL, &mask));
    for (int s = 1; s < NSIG; ++s) REQUIRE(sigismember(&mask, s) == 0);
    const int signals[] = {SIGTERM, SIGINT, SIGHUP, SIGPIPE, SIGCHLD};
    for (size_t i = 0; i < sizeof(signals)/sizeof(*signals); ++i) {
        struct sigaction action;
        REQUIRE(!sigaction(signals[i], NULL, &action) && action.sa_handler == SIG_DFL);
    }
    sigemptyset(&mask); sigaddset(&mask, SIGTERM);
    REQUIRE(!pthread_sigmask(SIG_BLOCK, &mask, NULL));
    REQUIRE(md_managed_launch_message_write(3, MD_LAUNCH_SIGNER));
    int received = 0;
    REQUIRE(!sigwait(&mask, &received) && received == SIGTERM);
    REQUIRE(md_managed_launch_message_write(3, MD_LAUNCH_ENDED));
    return md_test_report();
}

typedef struct { const char *client; int result, fd; pid_t pid; dispatch_semaphore_t done; } WorkerSpawn;
static void spawn_from_worker(void *context) {
    WorkerSpawn *w = context;
    sigset_t block, previous;
    sigemptyset(&block); sigaddset(&block, SIGTERM);
    bool blocked = pthread_sigmask(SIG_BLOCK, &block, &previous) == 0;
    MD_CHECK(!pthread_main_np() && blocked);
    w->result = md_managed_launch_spawn(w->client, &w->fd, &w->pid);
    if (blocked) MD_CHECK(!pthread_sigmask(SIG_SETMASK, &previous, NULL));
    dispatch_semaphore_signal(w->done);
}
static bool await_status(int fd, MdLaunchMessage expected) {
    struct pollfd p = {.fd = fd, .events = POLLIN};
    MdLaunchMessage actual;
    return poll(&p, 1, 3000) == 1 && (p.revents & POLLIN) &&
        md_managed_launch_message_read(fd, &actual, true) == 1 && actual == expected;
}
static int worker_signal_and_stalled_write(const char *client) {
    struct sigaction ignored = {.sa_handler = SIG_IGN}, saved;
    sigemptyset(&ignored.sa_mask);
    REQUIRE(!setenv("XDG_STATE_HOME", STALL_MARKER, 1)); /* fixture process only */
    REQUIRE(!sigaction(SIGTERM, &ignored, &saved));
    WorkerSpawn w = {.client = client, .fd = -1, .pid = -1, .done = dispatch_semaphore_create(0)};
    dispatch_queue_t queue = dispatch_queue_create("metadesk.spawn-fixture", DISPATCH_QUEUE_SERIAL);
    dispatch_async_f(queue, &w, spawn_from_worker);
    if (dispatch_semaphore_wait(w.done, dispatch_time(DISPATCH_TIME_NOW, 4000000000LL))) {
        MD_CHECK_MSG(false, "worker spawn did not finish"); _exit(md_test_report());
    }
    dispatch_release(queue); dispatch_release(w.done);
    MD_CHECK(!sigaction(SIGTERM, &saved, NULL));
    REQUIRE(!w.result);
    bool ready = await_status(w.fd, MD_LAUNCH_SIGNER);
    MD_CHECK(ready);
    if (ready) {
        int buffer = 4096; socklen_t length = sizeof(buffer);
        MD_CHECK(!setsockopt(w.fd, SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer)));
        MD_CHECK(!getsockopt(w.fd, SOL_SOCKET, SO_SNDBUF, &buffer, &length));
        MdManagedLaunch c = {.address = KEY, .pin = KEY, .signer = OTHER_KEY, .port = 7700,
            .relay_count = MD_RELAY_TARGET_MAX, .capabilities = MD_CAP_VIDEO};
        for (size_t i = 0; i < c.relay_count; ++i) {
            static const char prefix[] = "ws://127.0.0.1/";
            memcpy(c.relays[i], prefix, sizeof(prefix)-1);
            memset(c.relays[i]+sizeof(prefix)-1, 'a', MD_RELAY_URL_MAX-(sizeof(prefix)-1));
        }
        MD_CHECK(c.relay_count * MD_RELAY_URL_MAX > (size_t)buffer);
        int64_t before = md_now_ms();
        MD_CHECK(!md_managed_launch_write(w.fd, &c));
        int64_t elapsed = md_now_ms() - before;
        printf("stalled config send: %lld ms (socket send buffer %d bytes)\n", (long long)elapsed, buffer);
        MD_CHECK(elapsed >= 4900 && elapsed <= 7500);
    }
    bool finished = ready && !kill(w.pid, SIGTERM) && await_status(w.fd, MD_LAUNCH_ENDED);
    MD_CHECK(finished);
    if (!finished) kill(w.pid, SIGKILL);
    close(w.fd);
    int status;
    MD_CHECK(waitpid(w.pid, &status, 0) == w.pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    return md_test_report();
}

int main(int argc, char **argv) {
    MdLaunchMessage message;
    MdManagedLaunch config;
    if (argc > 1) {
        REQUIRE(argc == 3 && !strcmp(argv[1], "--managed-session-fd") && !strcmp(argv[2], "3"));
        REQUIRE(fcntl(SENTINEL, F_GETFD) == -1 && errno == EBADF);
        REQUIRE(!getenv("MD_FIXTURE_CREDENTIAL"));
        REQUIRE(fcntl(3, F_GETFD) == 0);
        REQUIRE(md_managed_launch_prepare_stream(3));
        REQUIRE(fcntl(3, F_GETFD) & FD_CLOEXEC);
        const char *state = getenv("XDG_STATE_HOME");
        if (state && !strcmp(state, STALL_MARKER)) return stalled_peer();
        REQUIRE(md_managed_launch_read(3, &config));
        REQUIRE(!strcmp(config.pin, KEY) && !strcmp(config.signer, OTHER_KEY));
        REQUIRE(!strcmp(config.relays[0], "ws://127.0.0.1:1") && config.port == 7700);
        REQUIRE(md_managed_launch_message_write(3, MD_LAUNCH_SIGNER));
        REQUIRE(md_managed_launch_message_read(3, &message, false) == 1 && message == MD_LAUNCH_CANCEL);
        REQUIRE(md_managed_launch_message_write(3, MD_LAUNCH_CANCELLED));
        return md_test_report();
    }

    int pair[2];
    REQUIRE(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    REQUIRE(md_managed_launch_prepare_stream(pair[0]));
    REQUIRE(md_management_peer_is_user(pair[0], geteuid()));
    REQUIRE(!md_management_peer_is_user(pair[0], (uint64_t)geteuid() + 1));
    REQUIRE(md_managed_launch_message_read(pair[0], &message, true) == -1 && errno == EAGAIN);
    unsigned char invalid = 0xff;
    REQUIRE(write(pair[1], &invalid, 1) == 1);
    REQUIRE(md_managed_launch_message_read(pair[0], &message, true) == -1 && errno == EPROTO);
    close(pair[1]);
    REQUIRE(md_managed_launch_message_read(pair[0], &message, false) == 0);
    REQUIRE(!md_managed_launch_message_write(pair[0], MD_LAUNCH_CANCEL)); /* no SIGPIPE */
    close(pair[0]);

    int fd = open("/dev/null", O_RDONLY);
    REQUIRE(fd >= 0 && !md_managed_launch_prepare_stream(fd));
    REQUIRE(dup2(fd, SENTINEL) == SENTINEL);
    close(fd);
    REQUIRE(fcntl(SENTINEL, F_GETFD) == 0);
    REQUIRE(!setenv("MD_FIXTURE_CREDENTIAL", "dummy fixture", 1));
    char *self = realpath(argv[0], NULL);
    REQUIRE(self);
    pid_t child;
    REQUIRE(!md_managed_launch_spawn(self, &fd, &child));
    close(SENTINEL);
    config = (MdManagedLaunch){.address = KEY, .pin = KEY, .signer = OTHER_KEY,
        .relays = {"ws://127.0.0.1:1"}, .relay_count = 1, .port = 7700, .capabilities = MD_CAP_VIDEO};
    REQUIRE(md_managed_launch_write(fd, &config));
    REQUIRE(await_status(fd, MD_LAUNCH_SIGNER));
    REQUIRE(md_managed_launch_message_write(fd, MD_LAUNCH_CANCEL));
    REQUIRE(await_status(fd, MD_LAUNCH_CANCELLED));
    struct pollfd drained = {.fd = fd, .events = POLLIN};
    REQUIRE(poll(&drained, 1, 3000) == 1 && md_managed_launch_message_read(fd, &message, true) == 0);
    close(fd);
    int status;
    REQUIRE(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    puts("private Darwin channel spawn, peer check, status and cancellation passed");
    int result = worker_signal_and_stalled_write(self);
    free(self);
    return result;
}
