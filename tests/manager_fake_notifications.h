#pragma once
/* Private org.gtk.Notifications service exercises GNotification's real D-Bus
 * backend: exact identity/target, no approval action, removal on cancellation
 * and expiry. It is not a GNOME Shell visual-notification qualification. */
typedef struct {
    GDBusConnection *bus;
    guint registration, added, removed;
    GHashTable *live;
} ManagerFakeNotifications;
static void notification_call(GDBusConnection *bus,const char *sender,const char *path,
    const char *interface,const char *method,GVariant *parameters,GDBusMethodInvocation *inv,gpointer data) {
    (void)bus; (void)sender; (void)path; (void)interface;
    ManagerFakeNotifications *fake=data; const char *app,*id;
    if (!strcmp(method,"AddNotification")) {
        GVariant *values; g_variant_get(parameters,"(&s&s@a{sv})",&app,&id,&values);
        ++fake->added; g_hash_table_add(fake->live,g_strdup(id));
        const char *action=NULL; MD_CHECK(g_variant_lookup(values,"default-action","&s",&action));
        MD_CHECK(action && !strcmp(action,"app.review-request"));
        g_autoptr(GVariant) target=g_variant_lookup_value(values,"default-action-target",G_VARIANT_TYPE("(ss)"));
        MD_CHECK(target);
        if(target) {
            const char *generation,*request; g_variant_get(target,"(&s&s)",&generation,&request);
            g_autofree char *expected=g_strdup_printf("request-%s-%s",generation,request); MD_CHECK(!strcmp(expected,id));
        }
        g_autoptr(GVariant) buttons=g_variant_lookup_value(values,"buttons",NULL);
        MD_CHECK(!buttons || g_variant_n_children(buttons)==0);
        g_variant_unref(values);
    } else {
        MD_CHECK(!strcmp(method,"RemoveNotification"));
        g_variant_get(parameters,"(&s&s)",&app,&id); ++fake->removed;
        MD_CHECK(g_hash_table_remove(fake->live,id));
    }
    g_dbus_method_invocation_return_value(inv,NULL);
}
static void notifications_start(ManagerFakeNotifications *fake) {
    memset(fake,0,sizeof(*fake)); fake->bus=g_bus_get_sync(G_BUS_TYPE_SESSION,NULL,NULL); MD_CHECK(fake->bus);
    fake->live=g_hash_table_new_full(g_str_hash,g_str_equal,g_free,NULL);
    const char *xml="<node><interface name='org.gtk.Notifications'>"
        "<method name='AddNotification'><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='a{sv}' direction='in'/></method>"
        "<method name='RemoveNotification'><arg type='s' direction='in'/><arg type='s' direction='in'/></method>"
        "</interface></node>";
    g_autoptr(GDBusNodeInfo) info=g_dbus_node_info_new_for_xml(xml,NULL);
    static const GDBusInterfaceVTable table={.method_call=notification_call};
    fake->registration=g_dbus_connection_register_object(fake->bus,"/org/gtk/Notifications",info->interfaces[0],&table,fake,NULL,NULL);
    MD_CHECK(fake->registration);
    g_autoptr(GVariant) reply=g_dbus_connection_call_sync(fake->bus,"org.freedesktop.DBus","/org/freedesktop/DBus","org.freedesktop.DBus","RequestName",g_variant_new("(su)","org.gtk.Notifications",4u),G_VARIANT_TYPE("(u)"),G_DBUS_CALL_FLAGS_NONE,5000,NULL,NULL);
    MD_CHECK(reply);
}
static void notifications_stop(ManagerFakeNotifications *fake) {
    g_dbus_connection_unregister_object(fake->bus,fake->registration); g_object_unref(fake->bus); g_hash_table_unref(fake->live);
}
