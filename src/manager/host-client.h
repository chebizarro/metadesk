#pragma once
#include <gio/gio.h>
#include "host_access.h"
#include <cjson/cJSON.h>
#define MD_TYPE_MANAGER_HOST (md_manager_host_get_type())
G_DECLARE_FINAL_TYPE(MdManagerHost, md_manager_host, MD, MANAGER_HOST, GObject)
MdManagerHost *md_manager_host_new(void);
void md_manager_host_connect(MdManagerHost *self, const char *path);
void md_manager_host_stop(MdManagerHost *self);
void md_manager_host_refresh_access(MdManagerHost *self);
void md_manager_host_command(MdManagerHost *self, const cJSON *command);
const cJSON *md_manager_host_snapshot(MdManagerHost *self);
const cJSON *md_manager_host_policy(MdManagerHost *self);
const char *md_manager_host_status(MdManagerHost *self);
gboolean md_manager_host_ready(MdManagerHost *self);
gboolean md_manager_host_busy(MdManagerHost *self);
/* Pure model helpers. Unknown capability bits remain visible, never selected. */
gboolean md_manager_host_validate(const cJSON *json, GError **error);
char *md_manager_caps(guint32 bits);
const char *md_manager_json_string(const cJSON *json, const char *key);
guint64 md_manager_json_uint(const cJSON *json, const char *key);
cJSON *md_manager_host_mutation(const cJSON *snapshot, const char *method);
