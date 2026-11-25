#include "launch.h"
#include "session.h"
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static const char host[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static const char signer[] = "1000000000000000100000000000000010000000000000001000000000000000";

static int fixture(int argc, char **argv) {
    g_assert_cmpint(argc, ==, 3);
    g_assert_cmpstr(argv[1], ==, "--managed-session-fd");
    g_assert_cmpstr(argv[2], ==, "3");
    g_assert_cmpint(fcntl(17, F_GETFD), ==, -1);
    MdManagedLaunch config;
    g_assert_true(md_managed_launch_read(3, &config));
    g_assert_cmpstr(config.address, ==, host);
    g_assert_cmpstr(config.pin, ==, host);
    g_assert_cmpstr(config.signer, ==, signer);
    g_assert_cmpstr(config.route, ==, "::1");
    g_assert_cmpint(config.port, ==, 7700);
    g_assert_cmpint(config.capabilities, ==, MD_CAP_VIDEO);
    g_assert_cmpuint(config.relay_count, ==, 1);
    g_assert_cmpstr(config.relays[0], ==, "ws://127.0.0.1:9999");
    for (unsigned i = 0; i < 40; ++i)
        g_assert_true(md_managed_launch_message_write(3, MD_LAUNCH_RELAY_REJECTED));
    g_assert_true(md_managed_launch_message_write(3, MD_LAUNCH_RESOLVE));
    MdLaunchMessage msg;
    g_assert_cmpint(md_managed_launch_message_read(3, &msg, false), ==, 1);
    g_assert_cmpint(msg, ==, MD_LAUNCH_CANCEL);
    g_assert_true(md_managed_launch_message_write(3, MD_LAUNCH_CANCELLED));
    return 0;
}

typedef struct { GMainLoop *loop; MdManagerLaunch *launch; unsigned count; } Check;
static void status(MdLaunchMessage msg, gpointer data) {
    Check *check = data;
    if (++check->count <= 40) {
        g_assert_cmpint(msg, ==, MD_LAUNCH_RELAY_REJECTED);
    } else if (check->count == 41) {
        g_assert_cmpint(msg, ==, MD_LAUNCH_RESOLVE);
        g_assert_true(md_manager_launch_cancel(check->launch));
    } else {
        g_assert_cmpuint(check->count, ==, 42);
        g_assert_cmpint(msg, ==, MD_LAUNCH_CANCELLED);
        g_main_loop_quit(check->loop);
    }
}

int main(int argc, char **argv) {
    if (argc > 1) return fixture(argc, argv);
    MdManagedLaunch config = {.route = "::1", .port = 7700, .relay_count = 1, .capabilities = MD_CAP_VIDEO,
                              .relays = {"ws://127.0.0.1:9999"}};
    memcpy(config.address, host, sizeof(host));
    memcpy(config.pin, host, sizeof(host));
    memcpy(config.signer, signer, sizeof(signer));
    int dummy = open("/dev/null", O_RDONLY | O_CLOEXEC);
    g_assert_cmpint(dummy, >=, 0);
    g_assert_cmpint(dup2(dummy, 17), ==, 17);
    g_autoptr(GError) error = NULL;
    Check check = {.loop = g_main_loop_new(NULL, FALSE)};
    check.launch = md_manager_launch_start(argv[0], &config, status, &check, &error);
    g_assert_no_error(error);
    g_assert_nonnull(check.launch);
    close(17);
    if (dummy != 17) close(dummy);
    g_main_loop_run(check.loop);
    g_assert_cmpuint(check.count, ==, 42);
    md_manager_launch_free(check.launch);
    g_main_loop_unref(check.loop);
    return 0;
}
