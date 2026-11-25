#include "managed_launch.h"
#include "management_ipc.h"
#include "session.h"
#include "md_test.h"
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

/* Stop on the first failure; later steps require a live peer and valid fds. */
#define REQUIRE(cond) do { MD_CHECK(cond); if (md_test_failures) return md_test_report(); } while (0)

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
    free(self);
    close(SENTINEL);
    config = (MdManagedLaunch){.address = KEY, .pin = KEY, .signer = OTHER_KEY,
        .relays = {"ws://127.0.0.1:1"}, .relay_count = 1, .port = 7700, .capabilities = MD_CAP_VIDEO};
    REQUIRE(md_managed_launch_write(fd, &config));
    REQUIRE(md_managed_launch_message_read(fd, &message, false) == 1 && message == MD_LAUNCH_SIGNER);
    REQUIRE(md_managed_launch_message_write(fd, MD_LAUNCH_CANCEL));
    REQUIRE(md_managed_launch_message_read(fd, &message, false) == 1 && message == MD_LAUNCH_CANCELLED);
    REQUIRE(md_managed_launch_message_read(fd, &message, false) == 0);
    close(fd);
    int status;
    REQUIRE(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    puts("private Darwin channel spawn, peer check, status and cancellation passed");
    return md_test_report();
}
