/*
 * metadesk — capture_pipewire.c
 * Linux capture backend: PipeWire via ScreenCast portal.
 *
 * Flow:
 *   1. pw_init() — initialize PipeWire client library
 *   2. Portal negotiation (D-Bus) — deferred to portal_open()
 *   3. pw_stream_new() — create a PipeWire stream
 *   4. pw_stream_connect() — connect using fd+node from portal
 *   5. on_process() callback — invoked per frame by PipeWire
 *   6. Map buffer (SHM or DMA-BUF), store in ring for get_frame()
 *
 * PipeWire runs its own event loop (pw_main_loop) on a dedicated
 * thread. Frames are handed to the consumer via get_frame /
 * release_frame synchronised with a mutex + condition variable.
 *
 * Milestone 1.1 target: single frame to disk, DMA-BUF path
 * confirmed on T7610/P40.
 */
#include "capture.h"
#include "portal_lock.h"
#ifdef MD_HAVE_PORTAL_INPUT
#include "portal_session.h"
#endif

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/props.h>
#include <spa/debug/types.h>
#include <spa/param/video/type-info.h>
#include <spa/utils/result.h>
#include <spa/buffer/buffer.h>

#include <dbus/dbus.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include "log.h"
#include "md_time.h"
#define MD_LOG_TAG "capture_pipewire"

/* ══════════════════════════════════════════════════════════════
 * Portal ScreenCast D-Bus flow (xdg-desktop-portal)
 *
 * On Wayland compositors, PipeWire screen capture requires explicit
 * user consent via the xdg-desktop-portal ScreenCast interface.
 * The protocol is:
 *   1. CreateSession()    → session handle
 *   2. SelectSources()    → configure capture source type
 *   3. Start()            → user approves in portal dialog
 *   4. OpenPipeWireRemote → PipeWire fd for pw_context_connect_fd()
 *
 * Each method returns a Request object path; a Response signal is
 * emitted asynchronously when the operation completes.  We use the
 * handle_token approach to predict the Request path and subscribe
 * to the signal before calling the method (avoids race condition).
 * ══════════════════════════════════════════════════════════════ */

#define PORTAL_BUS_NAME   "org.freedesktop.portal.Desktop"
#define PORTAL_OBJ_PATH   "/org/freedesktop/portal/desktop"
#define PORTAL_SCREENCAST  "org.freedesktop.portal.ScreenCast"
#define PORTAL_REQUEST     "org.freedesktop.portal.Request"

/* Portal source types (bitmask) */
#define PORTAL_SOURCE_MONITOR  1u
#define PORTAL_SOURCE_WINDOW   2u

/* Portal cursor modes (bitmask) */
#define PORTAL_CURSOR_HIDDEN   1u
#define PORTAL_CURSOR_EMBEDDED 2u
#define PORTAL_CURSOR_METADATA 4u

typedef struct {
    DBusConnection *bus;
    char           *session_handle;
    int             pw_fd;       /* PipeWire fd from portal       */
    uint32_t        node_id;     /* stream node ID from portal    */
    int             token_seq;   /* counter for unique tokens     */
    char            sender[128]; /* munged bus unique name         */
    uint64_t        deadline_ns; /* one budget for the entire negotiation */
    char            owner[128]; /* unique sender of the portal method reply */
    bool            request_close_unconfirmed;
} PortalScreencast;

/* Absolute CLOCK_MONOTONIC time for pthread_cond_timedwait on the backend's
 * frame_cond (initialised with that clock). */
static struct timespec monotonic_timespec(uint64_t at_ns) {
    return (struct timespec){ .tv_sec = (time_t)(at_ns / 1000000000ULL),
                              .tv_nsec = (long)(at_ns % 1000000000ULL) };
}

/* A started stream must deliver a valid initial frame, not merely link. */
#define PW_FIRST_FRAME_DEADLINE_NS 5000000000ULL

static int portal_remaining_ms(PortalScreencast *p, int limit) {
    uint64_t now = md_monotonic_ns();
    if (now >= p->deadline_ns) return 0;
    uint64_t ms = (p->deadline_ns - now) / 1000000;
    return ms < (uint64_t)limit ? (int)ms : limit;
}

/* Munge a D-Bus unique name (":1.234" → "1_234") for request paths */
static void munge_sender(const char *name, char *out, size_t out_sz) {
    size_t j = 0;
    for (size_t i = (name[0] == ':') ? 1 : 0; name[i] && j < out_sz - 1; i++)
        out[j++] = (name[i] == '.') ? '_' : name[i];
    out[j] = '\0';
}

/* ── D-Bus a{sv} dict helpers ───────────────────────────────── */

static void dict_append_sv(DBusMessageIter *dict, const char *key,
                           int type, const char *sig, const void *val) {
    DBusMessageIter entry, variant;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, sig, &variant);
    dbus_message_iter_append_basic(&variant, type, val);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(dict, &entry);
}

static void dict_append_sv_string(DBusMessageIter *dict,
                                  const char *key, const char *val) {
    dict_append_sv(dict, key, DBUS_TYPE_STRING, "s", &val);
}

static void dict_append_sv_uint32(DBusMessageIter *dict,
                                  const char *key, uint32_t val) {
    dict_append_sv(dict, key, DBUS_TYPE_UINT32, "u", &val);
}

/* Look up a string value in an a{sv} dict iter positioned at the array */
static const char *dict_lookup_string(DBusMessageIter *dict_iter,
                                      const char *key) {
    DBusMessageIter arr;
    dbus_message_iter_recurse(dict_iter, &arr);

    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry, variant;
        dbus_message_iter_recurse(&arr, &entry);

        const char *k;
        dbus_message_iter_get_basic(&entry, &k);

        if (strcmp(k, key) == 0) {
            dbus_message_iter_next(&entry);
            dbus_message_iter_recurse(&entry, &variant);
            int type = dbus_message_iter_get_arg_type(&variant);
            if (type == DBUS_TYPE_STRING || type == DBUS_TYPE_OBJECT_PATH) {
                const char *v;
                dbus_message_iter_get_basic(&variant, &v);
                return v;
            }
            return NULL;
        }
        dbus_message_iter_next(&arr);
    }
    return NULL;
}

/* ── Portal call + response pattern ──────────────────────────── */

/*
 * Make a unique handle_token string and compute the expected request
 * object path.  The caller must free the returned token.
 */
static char *portal_make_token(PortalScreencast *p, char *path_out, size_t path_sz) {
    char token[64];
    snprintf(token, sizeof(token), "metadesk%d", p->token_seq++);

    snprintf(path_out, path_sz,
             "/org/freedesktop/portal/desktop/request/%s/%s",
             p->sender, token);

    return strdup(token);
}

/* Cleanup has its own bound, even when Start exhausted the consent budget. */
static bool portal_close_object(PortalScreencast *p, const char *path, const char *iface) {
    DBusMessage *msg = dbus_message_new_method_call(p->owner, path, iface, "Close");
    if (!msg) return false;
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(p->bus, msg, 1000, NULL);
    dbus_message_unref(msg);
    bool ack = reply && dbus_message_has_sender(reply, p->owner) && dbus_message_has_signature(reply, "");
    if (reply) dbus_message_unref(reply);
    return ack;
}

/*
 * Wait for a Response signal on @request_path.
 * Returns the response code (0 = success) and stores the signal
 * message in *out_msg (caller must unref).  Returns UINT32_MAX on
 * timeout or error.
 */
static uint32_t portal_wait_response(PortalScreencast *p,
                                     const char *request_path,
                                     int timeout_ms,
                                     DBusMessage **out_msg) {
    uint32_t result = UINT32_MAX;
    uint64_t deadline = md_monotonic_ns() + (uint64_t)timeout_ms * 1000000;
    if (deadline > p->deadline_ns) deadline = p->deadline_ns;

    while (md_monotonic_ns() < deadline) {
        int remaining = (int)((deadline - md_monotonic_ns()) / 1000000);
        if (remaining <= 0) break;
        if (!dbus_connection_read_write(p->bus, remaining > 100 ? 100 : remaining)) break;
        DBusMessage *msg;
        while (md_monotonic_ns() < deadline &&
               (msg = dbus_connection_pop_message(p->bus)) != NULL) {
            if (dbus_message_is_signal(msg, PORTAL_REQUEST, "Response")
                && dbus_message_has_sender(msg, p->owner)
                && dbus_message_has_path(msg, request_path)) {
                if (!dbus_message_has_signature(msg, "ua{sv}")) {
                    dbus_message_unref(msg);
                    goto done;
                }
                /* Parse response code (first arg) */
                DBusMessageIter args;
                dbus_message_iter_init(msg, &args);
                dbus_message_iter_get_basic(&args, &result);

                if (out_msg)
                    *out_msg = msg;  /* caller takes ownership */
                else
                    dbus_message_unref(msg);

                goto done;
            }
            dbus_message_unref(msg);
        }
    }
    MD_LOG_I("timeout waiting for Response on %s", request_path);

done:
    if (result == UINT32_MAX)
        p->request_close_unconfirmed = !portal_close_object(p, request_path, PORTAL_REQUEST);
    return result;
}

/* All method waits, including Hello/authentication and AddMatch, consume the
 * same absolute startup budget. No unbounded flush or bus registration. */
static DBusMessage *portal_call_status(PortalScreencast *p, DBusMessage *msg, bool *rejected) {
    if (rejected) *rejected = false;
    int remaining = portal_remaining_ms(p, 180000);
    if (!msg || remaining <= 0) return NULL;
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(p->bus, msg, remaining, &err);
    if (dbus_error_is_set(&err)) {
        MD_LOG_I("portal method failed: %s", err.name);
        /* Only an error reply proves the method was handled and refused;
         * a local timeout or disconnect leaves its request state unknown. */
        if (rejected) *rejected = !dbus_error_has_name(&err, DBUS_ERROR_NO_REPLY) &&
            !dbus_error_has_name(&err, DBUS_ERROR_TIMED_OUT) &&
            !dbus_error_has_name(&err, DBUS_ERROR_DISCONNECTED);
        dbus_error_free(&err);
    }
    return reply;
}

static DBusMessage *portal_call(PortalScreencast *p, DBusMessage *msg) {
    return portal_call_status(p, msg, NULL);
}

static bool portal_request(PortalScreencast *p, DBusMessage *msg, const char *path) {
    bool rejected;
    DBusMessage *reply = portal_call_status(p, msg, &rejected);
    if (!reply) {
        /* The portal may already own @path (and a pending backend dialog)
         * when its method reply is lost or late: cancel it before any
         * Session.Close, exactly as for a missing Response. */
        if (!rejected && p->owner[0])
            p->request_close_unconfirmed = !portal_close_object(p, path, PORTAL_REQUEST);
        return false;
    }
    const char *actual = NULL;
    const char *sender = dbus_message_get_sender(reply);
    bool ok = dbus_message_has_signature(reply, "o") && sender && sender[0] == ':' &&
        strlen(sender) < sizeof(p->owner) && (!p->owner[0] || !strcmp(sender, p->owner)) &&
        dbus_message_get_args(reply, NULL, DBUS_TYPE_OBJECT_PATH, &actual, DBUS_TYPE_INVALID) &&
        strcmp(path, actual) == 0;
    if (ok) snprintf(p->owner, sizeof(p->owner), "%s", sender);
    dbus_message_unref(reply);
    return ok;
}

static void portal_screencast_close(PortalScreencast *portal);

/*
 * portal_screencast_open:
 * Execute the full ScreenCast portal flow and return a PipeWire fd
 * and stream node_id. Returns 0 on success or an MD_CAPTURE_* error.
 *
 * On success, portal->pw_fd and portal->node_id are populated.
 * The caller must eventually call portal_screencast_close().
 */
/* ── ScreenCast restore_token persistence ────────────────────────
 * A restore_token returned by a persistent Start lets later runs re-open the
 * same screencast without showing the consent dialog again. Persist it under
 * XDG_STATE_HOME so capture survives host restarts after one approval. */
static int portal_screencast_open(PortalScreencast *portal, const MdCaptureConfig *cfg) {
    char restore_token[1024] = {0};
    int status = MD_CAPTURE_ERROR;
    memset(portal, 0, sizeof(*portal));
    portal->pw_fd = -1;
    portal->deadline_ns = md_monotonic_ns() + (uint64_t)md_capture_consent_timeout_ms(cfg) * 1000000;

    /* Connect to a private session-bus connection: no shared message draining. */
    DBusError err;
    dbus_error_init(&err);
    const char *address = getenv("DBUS_SESSION_BUS_ADDRESS");
    /* Do not auto-launch a session or discover/connect to a different desktop.
     * Consent requires an explicitly available local session bus. */
    if (!address || strncmp(address, "unix:", 5) != 0 || strchr(address, ';')) return -1;
    portal->bus = dbus_connection_open_private(address, &err);
    if (!portal->bus) {
        MD_LOG_I("cannot connect to session bus: %s", err.message);
        dbus_error_free(&err);
        return -1;
    }

    dbus_connection_set_exit_on_disconnect(portal->bus, FALSE);
    /* libdbus's blocking method helper does not bound authentication. Drive
     * it explicitly before Hello so a silent peer cannot hold consent open. */
    while (!dbus_connection_get_is_authenticated(portal->bus)) {
        int remaining = portal_remaining_ms(portal, 100);
        if (remaining <= 0 || !dbus_connection_read_write(portal->bus, remaining)) goto fail;
    }
    DBusMessage *setup = dbus_message_new_method_call(DBUS_SERVICE_DBUS,
        DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "Hello");
    DBusMessage *reply = portal_call(portal, setup);
    if (setup) dbus_message_unref(setup);
    if (!reply) goto fail;
    const char *name = NULL;
    bool registered = dbus_message_has_signature(reply, "s") &&
        dbus_message_get_args(reply, NULL, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID) &&
        dbus_validate_bus_name(name, NULL) && dbus_bus_set_unique_name(portal->bus, name);
    dbus_message_unref(reply);
    if (!registered) goto fail;
    bool gnome_seen = false;
    int lock = md_portal_check_lock(portal->bus, portal->deadline_ns, !cfg->unattended, &gnome_seen);
    if (lock) { status = lock; goto fail; }

    /* Subscribe before any portal method; ordered AddMatch has a bounded reply. */
    setup = dbus_message_new_method_call(DBUS_SERVICE_DBUS,
        DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "AddMatch");
    if (!setup) goto fail;
    const char *rule = "type='signal',sender='" PORTAL_BUS_NAME "',interface='" PORTAL_REQUEST "',member='Response'";
    dbus_message_append_args(setup, DBUS_TYPE_STRING, &rule, DBUS_TYPE_INVALID);
    reply = portal_call(portal, setup);
    dbus_message_unref(setup);
    if (!reply) goto fail;
    dbus_message_unref(reply);

    /* Munge our unique name for request object paths */
    const char *unique = dbus_bus_get_unique_name(portal->bus);
    if (!unique) goto fail;
    munge_sender(unique, portal->sender, sizeof(portal->sender));

    /* ── Step 1: CreateSession ──────────────────────────────── */
    {
        char req_path[512];
        char *token = portal_make_token(portal, req_path, sizeof(req_path));
        if (!token) goto fail;

        DBusMessage *msg = dbus_message_new_method_call(
            PORTAL_BUS_NAME, PORTAL_OBJ_PATH,
            PORTAL_SCREENCAST, "CreateSession");
        if (!msg) { free(token); goto fail; }

        DBusMessageIter args, dict;
        dbus_message_iter_init_append(msg, &args);
        dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
        dict_append_sv_string(&dict, "handle_token", token);
        dict_append_sv_string(&dict, "session_handle_token", "metadesk_session");
        dbus_message_iter_close_container(&args, &dict);

        bool accepted = portal_request(portal, msg, req_path);
        dbus_message_unref(msg);
        free(token);
        if (!accepted) goto fail;

        DBusMessage *resp = NULL;
        uint32_t rc = portal_wait_response(portal, req_path, 5000, &resp);
        if (rc != 0 || !resp) {
            MD_LOG_I("CreateSession failed (rc=%u)", rc);
            if (resp) dbus_message_unref(resp);
            goto fail;
        }

        /* Extract session_handle from results dict */
        DBusMessageIter resp_args;
        dbus_message_iter_init(resp, &resp_args);
        dbus_message_iter_next(&resp_args); /* skip response code */

        const char *sh = dict_lookup_string(&resp_args, "session_handle");
        if (!sh || !dbus_validate_path(sh, NULL)) {
            MD_LOG_I("no session_handle in CreateSession response");
            dbus_message_unref(resp);
            goto fail;
        }
        portal->session_handle = strdup(sh);
        dbus_message_unref(resp);
        if (!portal->session_handle) goto fail;

        MD_LOG_I("session created: %s", portal->session_handle);
    }

    /* ── Step 2: SelectSources ─────────────────────────────── */
    {
        char req_path[512];
        char *token = portal_make_token(portal, req_path, sizeof(req_path));
        if (!token) goto fail;

        DBusMessage *msg = dbus_message_new_method_call(
            PORTAL_BUS_NAME, PORTAL_OBJ_PATH,
            PORTAL_SCREENCAST, "SelectSources");
        if (!msg) { free(token); goto fail; }

        const char *sh = portal->session_handle;
        DBusMessageIter args, dict;
        dbus_message_iter_init_append(msg, &args);
        dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH, &sh);
        dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
        dict_append_sv_string(&dict, "handle_token", token);
        dict_append_sv_uint32(&dict, "types", PORTAL_SOURCE_MONITOR);
        dict_append_sv_uint32(&dict, "cursor_mode", PORTAL_CURSOR_EMBEDDED);
        /* persist_mode 'u' (ScreenCast spec): 2 = persist until revoked, so a
         * returned restore_token lets later runs skip the consent dialog.
         * (It must be a uint32; a boolean makes the portal reject the call
         * with a method error and never emit a Response.) */
        dict_append_sv_uint32(&dict, "persist_mode", 2);
        char rtoken[1024];
        size_t token_len = md_capture_restore_token_load(rtoken, sizeof(rtoken));
        if (token_len && !dbus_validate_utf8(rtoken, NULL)) {
            MD_LOG_I("restore token is not valid UTF-8; refusing capture");
            dbus_message_iter_abandon_container(&args, &dict);
            dbus_message_unref(msg);
            free(token);
            goto fail;
        }
        if (token_len) dict_append_sv_string(&dict, "restore_token", rtoken);
        dbus_message_iter_close_container(&args, &dict);

        bool accepted = portal_request(portal, msg, req_path);
        dbus_message_unref(msg);
        free(token);
        if (!accepted) goto fail;

        uint32_t rc = portal_wait_response(portal, req_path, 5000, NULL);
        if (rc != 0) {
            MD_LOG_I("SelectSources failed (rc=%u)", rc);
            goto fail;
        }
        MD_LOG_I("sources selected (monitor, cursor embedded)");
    }

    lock = md_portal_check_lock(portal->bus, portal->deadline_ns, false, &gnome_seen);
    if (lock) { status = lock; goto fail; }

    /* ── Step 3: Start (shows user consent dialog) ────────── */
    {
        char req_path[512];
        char *token = portal_make_token(portal, req_path, sizeof(req_path));
        if (!token) goto fail;

        DBusMessage *msg = dbus_message_new_method_call(
            PORTAL_BUS_NAME, PORTAL_OBJ_PATH,
            PORTAL_SCREENCAST, "Start");
        if (!msg) { free(token); goto fail; }

        const char *sh = portal->session_handle;
        const char *parent = ""; /* no parent window */
        DBusMessageIter args, dict;
        dbus_message_iter_init_append(msg, &args);
        dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH, &sh);
        dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &parent);
        dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
        dict_append_sv_string(&dict, "handle_token", token);
        dbus_message_iter_close_container(&args, &dict);

        bool accepted = portal_request(portal, msg, req_path);
        dbus_message_unref(msg);
        free(token);
        if (!accepted) goto fail;

        /* Interactive callers may wait longer; unattended has one capped
         * budget across CreateSession, SelectSources, Start and remote open. */
        DBusMessage *resp = NULL;
        uint32_t rc = portal_wait_response(portal, req_path, md_capture_consent_timeout_ms(cfg), &resp);
        if (rc != 0 || !resp) {
            MD_LOG_I("Start failed or user cancelled (rc=%u)", rc);
            if (resp) dbus_message_unref(resp);
            goto fail;
        }

        /* Extract streams array from results.
         * Response is: (u response, a{sv} results)
         * results["streams"] is a(ua{sv}) — array of (node_id, props) */
        DBusMessageIter resp_args;
        dbus_message_iter_init(resp, &resp_args);
        dbus_message_iter_next(&resp_args); /* skip response code */

        /* Iterate the results dict looking for "streams" */
        DBusMessageIter results_arr;
        dbus_message_iter_recurse(&resp_args, &results_arr);

        bool found_streams = false;
        while (dbus_message_iter_get_arg_type(&results_arr) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter entry, variant;
            dbus_message_iter_recurse(&results_arr, &entry);

            const char *key;
            dbus_message_iter_get_basic(&entry, &key);

            if (strcmp(key, "streams") == 0) {
                dbus_message_iter_next(&entry);
                dbus_message_iter_recurse(&entry, &variant);

                if (dbus_message_iter_get_arg_type(&variant) != DBUS_TYPE_ARRAY) {
                    dbus_message_unref(resp);
                    goto fail;
                }
                char *signature = dbus_message_iter_get_signature(&variant);
                bool valid = signature && strcmp(signature, "a(ua{sv})") == 0;
                dbus_free(signature);
                if (!valid) { dbus_message_unref(resp); goto fail; }
                /* variant contains a(ua{sv}) */
                DBusMessageIter streams_arr, stream_struct;
                dbus_message_iter_recurse(&variant, &streams_arr);

                if (dbus_message_iter_get_arg_type(&streams_arr) == DBUS_TYPE_STRUCT) {
                    dbus_message_iter_recurse(&streams_arr, &stream_struct);
                    dbus_message_iter_get_basic(&stream_struct, &portal->node_id);
                    found_streams = portal->node_id != 0 && portal->node_id != PW_ID_ANY;
                    MD_LOG_I("stream node_id=%u", portal->node_id);
                }
            } else if (strcmp(key, "restore_token") == 0) {
                dbus_message_iter_next(&entry);
                dbus_message_iter_recurse(&entry, &variant);
                if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_STRING) {
                    const char *rt = NULL;
                    dbus_message_iter_get_basic(&variant, &rt);
                    if (!rt || strlen(rt) >= sizeof(restore_token)) {
                        dbus_message_unref(resp);
                        goto fail;
                    }
                    snprintf(restore_token, sizeof(restore_token), "%s", rt);
                }
            }
            dbus_message_iter_next(&results_arr);
        }
        dbus_message_unref(resp);

        if (!found_streams) {
            MD_LOG_I("no streams in Start response");
            goto fail;
        }
    }

    /* ── Step 4: OpenPipeWireRemote (get fd) ──────────────── */
    {
        DBusMessage *msg = dbus_message_new_method_call(
            PORTAL_BUS_NAME, PORTAL_OBJ_PATH,
            PORTAL_SCREENCAST, "OpenPipeWireRemote");
        if (!msg) goto fail;

        const char *sh = portal->session_handle;
        DBusMessageIter args, dict;
        dbus_message_iter_init_append(msg, &args);
        dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH, &sh);
        dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
        dbus_message_iter_close_container(&args, &dict);

        int remaining = portal_remaining_ms(portal, 5000);
        if (remaining <= 0) { dbus_message_unref(msg); goto fail; }
        DBusMessage *reply = dbus_connection_send_with_reply_and_block(
            portal->bus, msg, remaining, &err);
        dbus_message_unref(msg);

        if (!reply || dbus_error_is_set(&err)) {
            MD_LOG_I("OpenPipeWireRemote failed");
            dbus_error_free(&err);
            goto fail;
        }

        /* Reply contains a Unix FD */
        int fd = -1;
        if (!dbus_message_has_sender(reply, portal->owner) ||
            !dbus_message_has_signature(reply, "h") ||
            !dbus_message_get_args(reply, &err,
                                  DBUS_TYPE_UNIX_FD, &fd,
                                  DBUS_TYPE_INVALID)) {
            MD_LOG_I("invalid OpenPipeWireRemote fd reply");
            dbus_error_free(&err);
            dbus_message_unref(reply);
            goto fail;
        }
        /* dbus_message_get_args duplicates UNIX_FD; this copy is ours. */
        portal->pw_fd = fd;
        dbus_message_unref(reply);
        if (portal->pw_fd < 0) goto fail;
        MD_LOG_I("PipeWire fd=%d, node_id=%u", portal->pw_fd, portal->node_id);
    }

    if (restore_token[0]) {
        if (md_capture_restore_token_save(restore_token) == 0)
            MD_LOG_I("screencast consent persisted");
        else
            MD_LOG_I("cannot persist screencast consent securely");
    }
    return 0;

fail:
    if (status == MD_CAPTURE_DESKTOP_LOCKED) MD_LOG_W("desktop locked; ScreenCast Start not sent");
    portal_screencast_close(portal);
    return status;
}

static void portal_screencast_close(PortalScreencast *portal) {
    if (portal->pw_fd >= 0) {
        close(portal->pw_fd);
        portal->pw_fd = -1;
    }
    if (portal->request_close_unconfirmed) {
        /* Don't race an in-flight Request.Close with an explicit Session.Close.
         * Disconnect leaves remaining cleanup to the portal. */
        MD_LOG_W("portal request Close unconfirmed; disconnecting without Session.Close");
    } else if (portal->bus && portal->session_handle &&
               !portal_close_object(portal, portal->session_handle, "org.freedesktop.portal.Session")) {
        MD_LOG_I("portal session Close was not acknowledged");
    }
    free(portal->session_handle);
    portal->session_handle = NULL;
    if (portal->bus) {
        dbus_connection_close(portal->bus);
        dbus_connection_unref(portal->bus);
        portal->bus = NULL;
    }
}

/* ── Backend-private state ───────────────────────────────────── */

typedef struct {
    /* PipeWire state */
    struct pw_main_loop    *loop;
    struct spa_source      *stop_event;
    bool                    loop_exit; /* under frame_lock */
    struct pw_context      *pw_ctx;
    struct pw_core         *core;
    struct pw_stream       *stream;
    struct spa_hook         stream_listener;
    struct spa_hook         core_listener;

    /* Thread running pw_main_loop */
    pthread_t               thread;
    bool                    thread_started;

    /* Negotiated format. src_* is the stream's (native) size; ctx->width/
     * height publish md_capture_fit_size() of it under config's cap. */
    uint32_t                stride;
    uint32_t                spa_format;
    uint32_t                src_width, src_height;

    /* Size cap (md-awgm): the consumer-thread target of the box scale.
     * Touched only by get_frame/release's caller, which releases a frame
     * before requesting the next, so one buffer suffices; freed in destroy. */
    uint8_t                *scaled;
    size_t                  scaled_size;

    /* Frame delivery (pw thread → consumer) */
    pthread_mutex_t         frame_lock;
    pthread_cond_t          frame_cond;
    MdFrame                 pending_frame;
    bool                    frame_ready;
    bool                    received_frame;
    bool                    first_frame_deadline_reported;
    bool                    allocation_fault_reported;
    bool                    broken;
    bool                    stopping;
    uint64_t                started_ns;
    uint64_t                last_timestamp_ns;
    struct pw_buffer       *held_pw_buf; /* currently held by consumer */

    /* DMA-BUF preference */
    bool                    prefer_dmabuf;
    bool                    trace_buffers;
    uint32_t                generation;

    /* Sequence counter */
    atomic_uint_least32_t   seq;

    /* Back-pointer to ctx for callbacks */
    MdCaptureCtx           *ctx;

    /* Portal state (cleaned up on destroy) */
    PortalScreencast        portal;
    bool                    portal_active;
#ifdef MD_HAVE_PORTAL_INPUT
    MdPortalSession        *paired;
#endif
} PipewireState;

/* ── PipeWire stream events ──────────────────────────────────── */

/* Private diagnostics only: addresses/FDs describe this process, not ownership
 * transferable to a caller. Enable with MD_CAPTURE_TRACE_BUFFERS=1. */
static void trace_buffer(PipewireState *pw, const char *event, struct pw_buffer *buffer) {
    if (!pw->trace_buffers) return;
    struct spa_buffer *b = buffer ? buffer->buffer : NULL;
    MD_LOG_I("buffer %s gen=%u pw=%p spa=%p planes=%u", event, pw->generation,
             (void *)buffer, (void *)b, b ? b->n_datas : 0);
    for (uint32_t i = 0; b && i < b->n_datas; i++) {
        struct spa_data *d = &b->datas[i];
        MD_LOG_I(" plane=%u type=%u flags=%u fd=%ld data=%p mapoffset=%u max=%u chunk=%p offset=%u size=%u stride=%d",
                 i, d->type, d->flags, (long)d->fd, d->data, d->mapoffset,
                 d->maxsize, (void *)d->chunk, d->chunk ? d->chunk->offset : 0,
                 d->chunk ? d->chunk->size : 0, d->chunk ? d->chunk->stride : 0);
        if (strcmp(event, "add") == 0 && d->fd >= 0) {
            char path[64], target[256];
            snprintf(path, sizeof(path), "/proc/self/fd/%ld", (long)d->fd);
            ssize_t len = readlink(path, target, sizeof(target) - 1);
            if (len >= 0) {
                target[len] = '\0';
                struct stat st;
                if (fstat((int)d->fd, &st) == 0)
                    MD_LOG_I(" backing=%s file_size=%lld", target, (long long)st.st_size);
            }
        }
    }
}

static void on_add_buffer(void *userdata, struct pw_buffer *buffer) {
    trace_buffer(userdata, "add", buffer);
}

static void on_remove_buffer(void *userdata, struct pw_buffer *buffer) {
    trace_buffer(userdata, "remove", buffer);
}

static void on_param_changed(void *userdata, uint32_t id,
                             const struct spa_pod *param) {
    PipewireState *pw = userdata;
    MdCaptureCtx  *ctx = pw->ctx;

    if (!param || id != SPA_PARAM_Format)
        return;

    struct spa_video_info_raw info;
    if (spa_format_video_raw_parse(param, &info) < 0)
        return;

    /* xdpw and mutter offer monitor streams at one fixed size, so a cap is
     * met by scaling on the consumer side (pw_get_frame), not negotiation:
     * offering only sizes within the cap fails xdpw 0.7.1's link with "no
     * more input formats" (md-awgm, video-validation.md). */
    uint32_t out_w = 0, out_h = 0;
    if (md_capture_fit_size(info.size.width, info.size.height, ctx->config.max_width,
                            ctx->config.max_height, &out_w, &out_h) != 0)
        out_w = out_h = 0;
    if (out_w != info.size.width || out_h != info.size.height)
        MD_LOG_I("stream %ux%u, size cap %ux%u: delivering %ux%u (CPU box scale)",
                 info.size.width, info.size.height, ctx->config.max_width,
                 ctx->config.max_height, out_w, out_h);

    /* Published under the frame lock: md_capture_wait_size() waits on it. */
    pthread_mutex_lock(&pw->frame_lock);
    pw->src_width  = info.size.width;
    pw->src_height = info.size.height;
    ctx->width     = out_w;
    ctx->height    = out_h;
    pw->spa_format = info.format;
    /* Every negotiated format is 4 bytes/pixel. on_process prefers the
     * producer's chunk->stride; this is only the fallback when it is 0. */
    pw->stride = pw->src_width * 4;
    pthread_cond_broadcast(&pw->frame_cond);
    pthread_mutex_unlock(&pw->frame_lock);

    /* Tell PipeWire what buffer types we support.
     * Prefer DMA-BUF if configured, always support SHM as fallback. */
    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));

    uint32_t data_type = (1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr);
    if (pw->prefer_dmabuf)
        data_type |= (1 << SPA_DATA_DmaBuf);

    const struct spa_pod *params[1];
    params[0] = spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(data_type));

    int rc = pw_stream_update_params(pw->stream, params, 1);
    if (pw->trace_buffers)
        MD_LOG_I("buffer params gen=%u format=%u size=%ux%u dataType=%u map_buffers=1 rc=%d",
                 pw->generation, info.format, info.size.width, info.size.height, data_type, rc);
}

/* Map the negotiated SPA format (the closed EnumFormat set offered in
 * pw_start) to a capture format. Anything else is refused, not guessed. */
static bool spa_to_capture_fmt(uint32_t spa_fmt, MdCapturePixFmt *out) {
    switch (spa_fmt) {
    case SPA_VIDEO_FORMAT_BGRx: *out = MD_PIX_CAPTURE_BGRX; return true;
    case SPA_VIDEO_FORMAT_BGRA: *out = MD_PIX_CAPTURE_BGRA; return true;
    case SPA_VIDEO_FORMAT_RGBx: *out = MD_PIX_CAPTURE_RGBX; return true;
    case SPA_VIDEO_FORMAT_RGBA: *out = MD_PIX_CAPTURE_RGBA; return true;
    default:                    return false;
    }
}

static void on_process(void *userdata) {
    PipewireState *pw = userdata;
    MdCaptureCtx  *ctx = pw->ctx;

    struct pw_buffer *pw_buf = pw_stream_dequeue_buffer(pw->stream);
    if (!pw_buf) return;

    trace_buffer(pw, "dequeue", pw_buf);
    struct spa_buffer *spa_buf = pw_buf->buffer;
    if (!spa_buf || spa_buf->n_datas == 0)
        goto requeue;

    struct spa_data *d = &spa_buf->datas[0];
    if (!d->chunk || d->chunk->size == 0)
        goto requeue;

    /* MAP_BUFFERS does not guarantee every producer buffer is readable.
     * Reject invalid mappings/chunks while PW still owns the dequeued buffer;
     * never advertise a CPU frame whose backing pointer is NULL. */
    /* The source buffer is the stream's size; any cap is applied when the
     * consumer acquires the frame. */
    const uint32_t width = pw->src_width, height = pw->src_height;
    uint32_t stride = d->chunk->stride > 0 ? (uint32_t)d->chunk->stride : pw->stride;
    uint64_t needed = height ? (uint64_t)(height - 1) * stride + (uint64_t)width * 4 : 0;
    MdCapturePixFmt format;
    if (!spa_to_capture_fmt(pw->spa_format, &format) ||
        !width || !height || !ctx->width || !ctx->height || stride < (uint64_t)width * 4 ||
        /* Access bits may be absent: xdpw/PW 1.0.5 uses flags=0, newer
         * runtimes use MAPPABLE alone. DYNAMIC/MAPPABLE are not access bits.
         * Keep the legacy default, but reject explicit write-only access. */
        d->chunk->stride < 0 || ((d->flags & SPA_DATA_FLAG_WRITABLE) &&
                               !(d->flags & SPA_DATA_FLAG_READABLE)) ||
        d->chunk->offset > d->maxsize || d->chunk->size > d->maxsize - d->chunk->offset ||
        needed > d->chunk->size ||
        (d->type == SPA_DATA_DmaBuf && (!pw->prefer_dmabuf || d->fd < 0)) ||
        (d->type != SPA_DATA_DmaBuf &&
         ((d->type != SPA_DATA_MemFd && d->type != SPA_DATA_MemPtr) || !d->data))) {
        MD_LOG_I("reject buffer: format=%u type=%u flags=%u mapped=%d max=%u offset=%u size=%u stride=%d",
                 pw->spa_format, d->type, d->flags, d->data != NULL, d->maxsize,
                 d->chunk->offset, d->chunk->size, d->chunk->stride);
        if (d->type == SPA_ID_INVALID && !d->data && !d->maxsize &&
            !pw->allocation_fault_reported) {
            pw->allocation_fault_reported = true;
            MD_LOG_E("PipeWire backend allocation/publication fault: unresolved buffer skeleton "
                     "(SPA_ID_INVALID, unmapped, maxsize=0); rejecting unsafe frames "
                     "(client library %s; md-atg)", pw_get_library_version());
        }
        pthread_mutex_lock(&pw->frame_lock);
        pw->broken = true;
        pthread_cond_broadcast(&pw->frame_cond);
        pthread_mutex_unlock(&pw->frame_lock);
        goto requeue;
    }
    /* pw_buffer.time is zero on xdpw 0.7.1. Use CLOCK_MONOTONIC receipt
     * time in ns, not that unspecified producer value. Ownership is unchanged. */
    uint64_t timestamp = md_monotonic_ns();
    if (timestamp <= pw->last_timestamp_ns) timestamp = pw->last_timestamp_ns + 1;
    pw->last_timestamp_ns = timestamp;

    /* Build MdFrame */
    MdFrame frame = {
        .width        = width,
        .height       = height,
        .stride       = stride,
        .format       = format,
        .seq          = atomic_fetch_add_explicit(&pw->seq, 1, memory_order_relaxed),
        .timestamp_ns = timestamp,
    };

    if (d->type == SPA_DATA_DmaBuf) {
        frame.buf_type   = MD_BUF_DMABUF;
        frame.dmabuf_fd  = d->fd;
        frame.data       = NULL;
        frame.gpu_handle = NULL;
        frame.data_size  = d->maxsize;
    } else {
        frame.buf_type   = MD_BUF_CPU;
        frame.dmabuf_fd  = -1;
        frame.data       = (uint8_t *)d->data + d->chunk->offset;
        frame.gpu_handle = NULL;
        frame.data_size  = d->chunk->size;
    }

    /* Hand frame to consumer via lock + cond.
     * If a previous frame is still pending (not yet consumed), drop it and
     * requeue its PipeWire buffer before replacing it. If the consumer is
     * still holding the previous frame, do not overwrite held_pw_buf: requeue
     * this newer buffer instead so release_frame() can safely return the
     * consumer-owned buffer later. */
    pthread_mutex_lock(&pw->frame_lock);
    if (pw->stopping) {
        pthread_mutex_unlock(&pw->frame_lock);
        goto requeue;
    }
    if (pw->held_pw_buf) {
        if (pw->frame_ready) {
            pw_stream_queue_buffer(pw->stream, pw->held_pw_buf);
            pw->held_pw_buf = NULL;
        } else {
            pthread_mutex_unlock(&pw->frame_lock);
            pw_stream_queue_buffer(pw->stream, pw_buf);
            return;
        }
    }

    pw->broken = false;
    pw->received_frame = true;
    pw->pending_frame = frame;
    pw->held_pw_buf   = pw_buf;
    pw->frame_ready   = true;
    pthread_cond_broadcast(&pw->frame_cond);
    pthread_mutex_unlock(&pw->frame_lock);
    return; /* don't requeue — consumer calls release_frame */

requeue:
    pw_stream_queue_buffer(pw->stream, pw_buf);
}

static void on_state_changed(void *userdata, enum pw_stream_state old,
                             enum pw_stream_state state, const char *error) {
    PipewireState *pw = userdata;
    MdCaptureCtx  *ctx = pw->ctx;
    if (pw->trace_buffers)
        MD_LOG_I("stream state gen=%u %s -> %s error=%s", pw->generation,
                 pw_stream_state_as_string(old), pw_stream_state_as_string(state),
                 error ? error : "none");

    pthread_mutex_lock(&pw->frame_lock);
    ctx->active = state == PW_STREAM_STATE_STREAMING;
    pw->broken = state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED;
    pthread_cond_broadcast(&pw->frame_cond);
    pthread_mutex_unlock(&pw->frame_lock);
}

static void on_core_error(void *userdata, uint32_t id, int seq, int res, const char *message) {
    PipewireState *pw = userdata;
    (void)seq; (void)message;
    if (id != PW_ID_CORE) return;
    MD_LOG_I("PipeWire core error: %d", res);
    pthread_mutex_lock(&pw->frame_lock);
    pw->broken = true;
    pw->received_frame = false;
    pw->loop_exit = true;
    pthread_cond_broadcast(&pw->frame_cond);
    pthread_mutex_unlock(&pw->frame_lock);
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .error = on_core_error,
};

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .param_changed = on_param_changed,
    .process       = on_process,
    .add_buffer    = on_add_buffer,
    .remove_buffer = on_remove_buffer,
    .state_changed = on_state_changed,
};

/* ── PipeWire thread ─────────────────────────────────────────── */

static void on_stop_event(void *data, uint64_t count) {
    (void)data;
    (void)count;
}

static void *pw_thread_func(void *arg) {
    PipewireState *pw = arg;
    struct pw_loop *loop = pw_main_loop_get_loop(pw->loop);
    pw_loop_enter(loop);
    for (;;) {
        pthread_mutex_lock(&pw->frame_lock);
        bool stop = pw->loop_exit;
        pthread_mutex_unlock(&pw->frame_lock);
        if (stop) break;
        int rc = pw_loop_iterate(loop, -1);
        if (rc < 0 && rc != -EINTR) break;
    }
    pw_loop_leave(loop);
    pthread_mutex_lock(&pw->frame_lock);
    pw->broken = true;
    pw->received_frame = false;
    pthread_cond_broadcast(&pw->frame_cond);
    pthread_mutex_unlock(&pw->frame_lock);
    return NULL;
}

/* ── Backend vtable implementation ───────────────────────────── */

static int pw_backend_init(MdCaptureCtx *ctx) {
    /* Initialize PipeWire (idempotent, safe to call multiple times) */
    pw_init(NULL, NULL);
    if (!dbus_threads_init_default()) return -1;

    PipewireState *pw = calloc(1, sizeof(PipewireState));
    if (!pw) return -1;

    pw->ctx = ctx;
    const char *trace = getenv("MD_CAPTURE_TRACE_BUFFERS");
    pw->trace_buffers = trace && strcmp(trace, "1") == 0;
    /* SHM default; backends may expose knob later. A size cap needs CPU
     * frames (pw_scale_frame), so such a knob must stay off under a cap. */
    pw->prefer_dmabuf = false;
    pw->portal.pw_fd = -1;
    pw->broken = true;
    pthread_mutex_init(&pw->frame_lock, NULL);
    /* Timed waits are deadlines on the monotonic clock, immune to wall-clock
     * steps. */
    pthread_condattr_t cond_attr;
    pthread_condattr_init(&cond_attr);
    pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
    pthread_cond_init(&pw->frame_cond, &cond_attr);
    pthread_condattr_destroy(&cond_attr);

    pw->loop = pw_main_loop_new(NULL);
    if (!pw->loop) goto fail;

    struct pw_loop *loop = pw_main_loop_get_loop(pw->loop);
    pw->stop_event = pw_loop_add_event(loop, on_stop_event, NULL);
    if (!pw->stop_event) goto fail;
    pw->pw_ctx = pw_context_new(loop, NULL, 0);
    if (!pw->pw_ctx) goto fail;

    ctx->backend_data = pw;
    return 0;

fail:
    if (pw->pw_ctx) pw_context_destroy(pw->pw_ctx);
    if (pw->stop_event) pw_loop_destroy_source(pw_main_loop_get_loop(pw->loop), pw->stop_event);
    if (pw->loop) pw_main_loop_destroy(pw->loop);
    pthread_mutex_destroy(&pw->frame_lock);
    pthread_cond_destroy(&pw->frame_cond);
    free(pw);
    return -1;
}

static void pw_cleanup_start_resources(PipewireState *pw) {
    if (pw->trace_buffers)
        MD_LOG_I("capture cleanup gen=%u stream=%p core=%p", pw->generation,
                 (void *)pw->stream, (void *)pw->core);
    /* Called only after joining the loop, or before it starts. The pending
     * pointer belongs to this stream and cannot survive stream destruction. */
    pthread_mutex_lock(&pw->frame_lock);
    pw->held_pw_buf = NULL;
    pw->frame_ready = false;
    memset(&pw->pending_frame, 0, sizeof(pw->pending_frame));
    pw->ctx->width = pw->ctx->height = 0;
    pw->src_width = pw->src_height = 0;
    pw->received_frame = false;
    pw->broken = true;
    pthread_mutex_unlock(&pw->frame_lock);
    if (pw->stream) {
        pw_stream_disconnect(pw->stream);
        pw_stream_destroy(pw->stream);
        pw->stream = NULL;
    }
    if (pw->core) {
        pw_core_disconnect(pw->core);
        pw->core = NULL;
    }
    if (pw->portal_active || pw->portal.bus || pw->portal.session_handle || pw->portal.pw_fd >= 0) {
        portal_screencast_close(&pw->portal);
        pw->portal_active = false;
    }
}

static bool pw_capture_revoked(const PipewireState *pw) {
#ifdef MD_HAVE_PORTAL_INPUT
    return pw->paired && md_portal_session_revoked(pw->paired);
#else
    (void)pw;
    return false;
#endif
}

#ifdef MD_HAVE_PORTAL_INPUT
MdPortalSession *md_capture_portal_session(const MdCaptureCtx *ctx) {
    if (!ctx || !ctx->backend_data) return NULL;
    const PipewireState *pw = ctx->backend_data;
    return pw->paired;
}

static void pw_wake_revoked(void *data) {
    PipewireState *pw = data;
    pthread_mutex_lock(&pw->frame_lock);
    pthread_cond_broadcast(&pw->frame_cond);
    pthread_mutex_unlock(&pw->frame_lock);
}
#endif

static int pw_start(MdCaptureCtx *ctx) {
    /* ScreenCast has no no-prompt contract, even with a restore token. Do
     * not call Start and hope the compositor silently restores consent. */
    if (ctx->config.no_interaction) {
        MD_LOG_E("managed capture requires interaction: portal cannot guarantee prompt-free consent; use an interactive host");
        return MD_CAPTURE_INTERACTION_REQUIRED;
    }
    PipewireState *pw = ctx->backend_data;
    if (!pw) return -1;

    /* Portal authorization is mandatory. A denial/error is never permission
     * to bypass the portal with a direct connection. */
    if (pw->thread_started || pw->stream) return -1;
    pw->generation++;
    if (pw->trace_buffers) MD_LOG_I("capture start gen=%u", pw->generation);
    pw->received_frame = false;
    pw->first_frame_deadline_reported = false;
    pw->allocation_fault_reported = false;
    pw->broken = false;
    pw->stopping = false;
    uint32_t stream_node_id = PW_ID_ANY;
    int status = MD_CAPTURE_ERROR;

    if (ctx->config.portal_input) {
#ifdef MD_HAVE_PORTAL_INPUT
        if (!pw->paired) {
            pw->paired = md_portal_session_create(&ctx->config,
                                                  md_capture_consent_timeout_ms(&ctx->config), &status);
            if (pw->paired) md_portal_session_watch(pw->paired, pw_wake_revoked, pw);
        }
        if (pw->paired) {
            int fd = md_portal_session_open_capture(pw->paired, &stream_node_id);
            if (fd >= 0) pw->core = pw_context_connect_fd(pw->pw_ctx, fd, NULL, 0);
        }
#endif
    } else if ((status = portal_screencast_open(&pw->portal, &ctx->config)) == 0) {
        /* Portal succeeded — connect using the portal's PipeWire fd */
        pw->portal_active = true;
        pw->core = pw_context_connect_fd(pw->pw_ctx,
                                         fcntl(pw->portal.pw_fd, F_DUPFD_CLOEXEC, 3),
                                         NULL, 0);
        stream_node_id = pw->portal.node_id;
        MD_LOG_I("connected via portal (node=%u)", stream_node_id);
    }
    if (!pw->core) {
        pw_cleanup_start_resources(pw);
        return status == MD_CAPTURE_DESKTOP_LOCKED ? status : MD_CAPTURE_ERROR;
    }

    pw_core_add_listener(pw->core, &pw->core_listener, &core_events, pw);

    /* Create capture stream */
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE,    "Video",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE,    "Screen",
        NULL);

    pw->stream = pw_stream_new(pw->core, "metadesk-capture", props);
    if (!pw->stream) {
        pw_cleanup_start_resources(pw);
        return -1;
    }

    /* Listen for stream events */
    pw_stream_add_listener(pw->stream, &pw->stream_listener,
                           &stream_events, pw);

    /* Build format negotiation: request raw video, any format/size */
    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));

    uint32_t target_fps = ctx->config.target_fps ? ctx->config.target_fps : 60;

    const struct spa_pod *params[1];
    params[0] = spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(5,
            SPA_VIDEO_FORMAT_BGRx,
            SPA_VIDEO_FORMAT_BGRx,
            SPA_VIDEO_FORMAT_RGBx,
            SPA_VIDEO_FORMAT_RGBA,
            SPA_VIDEO_FORMAT_BGRA),
        SPA_FORMAT_VIDEO_size,   SPA_POD_CHOICE_RANGE_Rectangle(
            &SPA_RECTANGLE(1920, 1080),
            &SPA_RECTANGLE(1, 1),
            &SPA_RECTANGLE(4096, 4096)),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(
            &SPA_FRACTION(target_fps, 1),
            &SPA_FRACTION(0, 1),
            &SPA_FRACTION(120, 1)));

    /* Connect stream — use portal node_id when available, PW_ID_ANY as fallback */
    int ret = pw_stream_connect(pw->stream,
        PW_DIRECTION_INPUT, stream_node_id,
        PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
        params, 1);
    if (ret < 0) {
        pw_cleanup_start_resources(pw);
        return -1;
    }

    /* Consent can take arbitrarily long. Start the first-frame deadline only
     * once the authorized stream is ready to run, not before the portal UI. */
    pw->started_ns = md_monotonic_ns();
    /* The stop predicate is set before waking the loop; unlike calling
     * pw_main_loop_quit before run(), a stop before the thread enters is not lost. */
    pw->loop_exit = false;
    if (pthread_create(&pw->thread, NULL, pw_thread_func, pw) != 0) {
        pw_cleanup_start_resources(pw);
        return -1;
    }
    pw->thread_started = true;

    return 0;
}

static void pw_release_frame(MdCaptureCtx *ctx, MdFrame *frame);

/* Size cap: box-scale an acquired frame into pw->scaled. The PipeWire buffer
 * stays held (and stop keeps waiting for it) until release_frame, exactly as
 * for an unscaled frame; this runs outside the lock because a held buffer is
 * consumer-owned. Only consumed frames pay for the scale. Fails closed: a
 * frame that cannot be scaled is released, never delivered at the wrong size. */
static int pw_scale_frame(MdCaptureCtx *ctx, PipewireState *pw, MdFrame *frame) {
    uint32_t w = 0, h = 0;
    if (md_capture_fit_size(frame->width, frame->height, ctx->config.max_width,
                            ctx->config.max_height, &w, &h) != 0)
        goto fail;
    if (w == frame->width && h == frame->height) return 0;
    /* DMA-BUF is never negotiated (prefer_dmabuf is off), so a frame to scale
     * is a validated CPU mapping; anything else cannot be scaled here. */
    if (frame->buf_type != MD_BUF_CPU || !frame->data) goto fail;
    const size_t stride = (size_t)w * 4, size = stride * h;
    if (size > pw->scaled_size) {
        uint8_t *grown = realloc(pw->scaled, size);
        if (!grown) goto fail;
        pw->scaled = grown;
        pw->scaled_size = size;
    }
    if (md_capture_scale_frame(frame->data, frame->width, frame->height, frame->stride,
                               pw->scaled, w, h, stride) != 0)
        goto fail;
    frame->width     = w;
    frame->height    = h;
    frame->stride    = (uint32_t)stride;
    frame->data      = pw->scaled;
    frame->data_size = size;
    return 0;

fail:
    MD_LOG_E("cannot scale a %ux%u frame to the size cap", frame->width, frame->height);
    pw_release_frame(ctx, frame);
    memset(frame, 0, sizeof(*frame));
    frame->dmabuf_fd = -1;
    return MD_CAPTURE_ERROR;
}

static int pw_get_frame(MdCaptureCtx *ctx, MdFrame *out, uint32_t timeout_ms) {
    PipewireState *pw = ctx->backend_data;
    if (!pw) return MD_CAPTURE_ERROR;

    uint64_t until = md_monotonic_ns() + (uint64_t)timeout_ms * 1000000ULL;
    pthread_mutex_lock(&pw->frame_lock);

    /* PAUSED/STREAMING alone does not prove a usable link: restored portal
     * nodes can enter those states without delivering any buffers. Require a
     * valid first frame within the startup deadline. After that, damage-driven
     * capture can legitimately remain idle indefinitely. */
    while (!pw->frame_ready && !pw->broken && !pw->stopping && !pw_capture_revoked(pw)) {
        uint64_t limit = until;
        if (!pw->received_frame && pw->started_ns + PW_FIRST_FRAME_DEADLINE_NS < limit)
            limit = pw->started_ns + PW_FIRST_FRAME_DEADLINE_NS;
        if (md_monotonic_ns() >= limit) break;
        struct timespec ts = monotonic_timespec(limit);
        pthread_cond_timedwait(&pw->frame_cond, &pw->frame_lock, &ts);
    }

    if (pw_capture_revoked(pw)) {
        pthread_mutex_unlock(&pw->frame_lock);
        return MD_CAPTURE_ERROR;
    }
    if (pw->stopping || !pw->frame_ready) {
        bool first_frame_expired = !pw->received_frame &&
            md_monotonic_ns() - pw->started_ns >= PW_FIRST_FRAME_DEADLINE_NS;
        if (first_frame_expired && !pw->stopping && !pw->broken &&
            !pw->first_frame_deadline_reported) {
            pw->first_frame_deadline_reported = true;
            MD_LOG_E("PipeWire first-frame deadline exceeded: no valid frame within 5000ms "
                     "of stream start (client library %s)", pw_get_library_version());
        }
        bool broken = pw->stopping || pw->broken || first_frame_expired;
        pthread_mutex_unlock(&pw->frame_lock);
        return broken ? MD_CAPTURE_ERROR : MD_CAPTURE_IDLE;
    }

    trace_buffer(pw, "acquire", pw->held_pw_buf);
    *out = pw->pending_frame;
    pw->frame_ready = false;
    pthread_mutex_unlock(&pw->frame_lock);
    int rc = pw_scale_frame(ctx, pw, out);
    if (!rc && pw_capture_revoked(pw)) {
        pw_release_frame(ctx, out);
        return MD_CAPTURE_ERROR;
    }
    return rc;
}

/* Format negotiation (on_param_changed) publishes the size asynchronously. */
static int pw_wait_size(MdCaptureCtx *ctx, uint32_t timeout_ms,
                        uint32_t *width, uint32_t *height) {
    PipewireState *pw = ctx->backend_data;
    if (!pw) return -1;

    struct timespec deadline = monotonic_timespec(md_monotonic_ns() +
                                                  (uint64_t)timeout_ms * 1000000ULL);
    pthread_mutex_lock(&pw->frame_lock);
    while ((!ctx->width || !ctx->height) && !pw->broken && !pw->stopping && !pw_capture_revoked(pw) &&
           pthread_cond_timedwait(&pw->frame_cond, &pw->frame_lock, &deadline) != ETIMEDOUT) {}
    int rc = ctx->width && ctx->height && !pw->broken && !pw->stopping && !pw_capture_revoked(pw) ? 0 : -1;
    if (rc == 0) {
        *width  = ctx->width;
        *height = ctx->height;
    }
    pthread_mutex_unlock(&pw->frame_lock);
    return rc;
}

static void pw_release_frame(MdCaptureCtx *ctx, MdFrame *frame) {
    PipewireState *pw = ctx->backend_data;
    (void)frame;
    if (!pw) return;

    pthread_mutex_lock(&pw->frame_lock);
    if (pw->held_pw_buf) {
        trace_buffer(pw, "release", pw->held_pw_buf);
        pw_stream_queue_buffer(pw->stream, pw->held_pw_buf);
        pw->held_pw_buf = NULL;
        pthread_cond_broadcast(&pw->frame_cond);
    }
    pthread_mutex_unlock(&pw->frame_lock);
}

static void pw_stop(MdCaptureCtx *ctx) {
    PipewireState *pw = ctx->backend_data;
    if (!pw) return;

    pthread_mutex_lock(&pw->frame_lock);
    ctx->active = false;
    pw->stopping = true;
    pw->loop_exit = true;
    pw->broken = true;
    pthread_cond_broadcast(&pw->frame_cond);
    pthread_mutex_unlock(&pw->frame_lock);

    if (pw->thread_started) {
        pw_loop_signal_event(pw_main_loop_get_loop(pw->loop), pw->stop_event);
        pthread_join(pw->thread, NULL);
        pw->thread_started = false;
    }

    /* A concurrent consumer may still be encoding a borrowed mapping. Stop
     * prevents new acquisitions, then waits for that owner to release before
     * destroying the stream. A pending (unacquired) frame has no such owner. */
    pthread_mutex_lock(&pw->frame_lock);
    while (pw->held_pw_buf && !pw->frame_ready)
        pthread_cond_wait(&pw->frame_cond, &pw->frame_lock);
    pthread_mutex_unlock(&pw->frame_lock);

    /* Tear down the per-start resources (stream, core, portal) so a later
     * pw_start() can cleanly re-open them — used by the host's stall-recovery
     * restart. The loop and context are kept and reused. The pw loop thread
     * has been joined above, so touching these objects here is safe. */
    pw_cleanup_start_resources(pw);
    if (pw->trace_buffers) MD_LOG_I("capture stopped gen=%u", pw->generation);
}

/* Delete the persisted screencast restore_token so the next pw_start() shows
 * the consent dialog again (used after repeated capture failures). */
static void pw_forget_authorization(MdCaptureCtx *ctx) {
    if (ctx->config.portal_input) return; /* Never touch the independent ScreenCast token. */
    if (md_capture_restore_token_forget() == 0)
        MD_LOG_I("cleared screencast restore_token");
}

static void pw_destroy(MdCaptureCtx *ctx) {
    PipewireState *pw = ctx->backend_data;
    if (!pw) return;

    if (pw->stream) {
        pw_stream_disconnect(pw->stream);
        pw_stream_destroy(pw->stream);
    }
    if (pw->core)
        pw_core_disconnect(pw->core);
    if (pw->pw_ctx)
        pw_context_destroy(pw->pw_ctx);
    pw_loop_destroy_source(pw_main_loop_get_loop(pw->loop), pw->stop_event);
    pw_main_loop_destroy(pw->loop);

    /* Release portal resources (D-Bus session, fd) */
    if (pw->portal_active)
        portal_screencast_close(&pw->portal);

#ifdef MD_HAVE_PORTAL_INPUT
    if (pw->paired) {
        md_portal_session_watch(pw->paired, NULL, NULL);
        md_portal_session_unref(pw->paired);
    }
#endif
    pthread_mutex_destroy(&pw->frame_lock);
    pthread_cond_destroy(&pw->frame_cond);

    free(pw->scaled);
    free(pw);
    ctx->backend_data = NULL;
}

/* ── Singleton vtable ────────────────────────────────────────── */

static const MdCaptureBackend pipewire_backend = {
    .init          = pw_backend_init,
    .start         = pw_start,
    .get_frame     = pw_get_frame,
    .release_frame = pw_release_frame,
    .stop          = pw_stop,
    .destroy       = pw_destroy,
    .forget_authorization = pw_forget_authorization,
    .wait_size     = pw_wait_size,
};

const MdCaptureBackend *md_capture_backend_create(void) {
    return &pipewire_backend;
}

int md_capture_outputs(MdCaptureOutputs *out) {
    if (!out) return -1;
    memset(out,0,sizeof(*out)); out->count=1;
    strcpy(out->items[0].id,"portal"); strcpy(out->items[0].name,"Portal-selected display (local consent required to switch)");
    out->items[0].primary=true; strcpy(out->selected,"portal"); return 0;
}
