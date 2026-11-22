/* Native AT-SPI adapter with a fake global Component and two owned objects.
 * No bus, input device, display, or provider is created. */
#include <atspi/atspi.h>
#include "md_test.h"
static AtspiAccessible *desktop, *front;
static bool component_available = true, point_error;
static AtspiAccessible *fake_desktop(int index) { MD_CHECK(index == 0); return g_object_ref(desktop); }
static AtspiComponent *fake_component(AtspiAccessible *acc) {
    MD_CHECK(acc == desktop);
    return component_available ? (AtspiComponent *)g_object_ref(acc) : NULL;
}
static AtspiAccessible *fake_point(AtspiComponent *component, int x, int y,
                                    AtspiCoordType coords, GError **error) {
    MD_CHECK((AtspiAccessible *)component == desktop);
    MD_CHECK(x == 140 && y == 220 && coords == ATSPI_COORD_TYPE_SCREEN);
    if (point_error) g_set_error_literal(error, g_quark_from_static_string("fixture"), 1, "unavailable");
    return front ? g_object_ref(front) : NULL;
}
static bool deferred_drained;
static gboolean fake_deferred(gpointer data) { (void)data; deferred_drained = true; return G_SOURCE_REMOVE; }
static int fake_exit(void) {
    MD_CHECK(g_main_context_is_owner(g_main_context_default()));
    MD_CHECK(g_main_depth() > 0); /* queued dispatch, not invoke_full inline */
    MD_CHECK(deferred_drained);
    return 0;
}
#define atspi_exit fake_exit
#define atspi_get_desktop fake_desktop
#define atspi_accessible_get_component_iface fake_component
#define atspi_component_get_accessible_at_point fake_point
#include "../src/core/a11y_atspi.c"

static AtspiAccessible *owned(AtspiApplication *app, const char *path) {
    AtspiAccessible *acc = g_object_new(ATSPI_TYPE_ACCESSIBLE, NULL);
    ATSPI_OBJECT(acc)->app = g_object_ref(app);
    ATSPI_OBJECT(acc)->path = g_strdup(path);
    return acc;
}
int main(void) {
    AtspiApplication *app = g_object_new(ATSPI_TYPE_APPLICATION, NULL);
    app->bus_name = g_strdup(":1.42");
    app->hash = g_hash_table_new(g_str_hash, g_str_equal);
    desktop = owned(app, "/desktop");
    AtspiAccessible *target = owned(app, "/target"), *occluder = owned(app, "/occluder");
    AtspiState state = {0};
    atomic_init(&state.connected, true);
    g_mutex_init(&state.lock);
    state.event_loop = g_main_loop_new(NULL, FALSE);
    state.event_thread = g_thread_new("fixture-atspi", atspi_event_loop_thread, &state);
    MdA11yCtx ctx = { .vtable = &atspi_backend, .backend_data = &state };
    char *id = atspi_element_id(target);
    front = occluder;
    MD_CHECK(!md_a11y_target_at_point(&ctx, id, 140, 220));
    front = target;
    MD_CHECK(md_a11y_target_at_point(&ctx, id, 140, 220));
    point_error = true;
    MD_CHECK(!md_a11y_target_at_point(&ctx, id, 140, 220));
    point_error = false; component_available = false;
    MD_CHECK(!md_a11y_target_at_point(&ctx, id, 140, 220));
    component_available = true; front = NULL;
    MD_CHECK(!md_a11y_target_at_point(&ctx, id, 140, 220));
    atspi_stop_event_thread(&state);
    g_main_loop_unref(state.event_loop); g_mutex_clear(&state.lock);
    /* Cleanup is queued behind already-deferred cache messages and stops
     * its dispatcher in the same context callback. No bus is involved. */
    GSource *deferred = g_idle_source_new();
    g_source_set_callback(deferred, fake_deferred, NULL, NULL);
    g_source_attach(deferred, g_main_context_default());
    atspi_runtime_initialized = TRUE;
    atspi_runtime_shutdown();
    g_source_destroy(deferred); g_source_unref(deferred);
    MD_CHECK(!atspi_runtime_initialized);
    free(id);
    g_object_unref(target); g_object_unref(occluder); g_object_unref(desktop); g_object_unref(app);
    return md_test_report();
}
