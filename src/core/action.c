/*
 * metadesk — action.c
 * Action format JSON encode/decode. See spec §3.2.
 */
#include "action.h"
#include <cjson/cJSON.h>
#include <string.h>
#include <stdlib.h>

static const struct {
    MdActionType type;
    const char  *str;
} action_map[] = {
    { MD_ACTION_CLICK,       "click"      },
    { MD_ACTION_DBL_CLICK,   "dbl_click"  },
    { MD_ACTION_RIGHT_CLICK, "right_click"},
    { MD_ACTION_TYPE,        "type"       },
    { MD_ACTION_KEY_COMBO,   "key_combo"  },
    { MD_ACTION_SCROLL,      "scroll"     },
    { MD_ACTION_FOCUS,       "focus"      },
    { MD_ACTION_SET_VALUE,   "set_value"  },
    { MD_ACTION_SCREENSHOT,  "screenshot" },
};

#define ACTION_MAP_LEN (sizeof(action_map) / sizeof(action_map[0]))

const char *md_action_type_str(MdActionType type) {
    for (size_t i = 0; i < ACTION_MAP_LEN; i++) {
        if (action_map[i].type == type)
            return action_map[i].str;
    }
    return "unknown";
}

MdActionType md_action_type_from_str(const char *str) {
    if (!str) return MD_ACTION_UNKNOWN;
    for (size_t i = 0; i < ACTION_MAP_LEN; i++) {
        if (strcmp(action_map[i].str, str) == 0)
            return action_map[i].type;
    }
    return MD_ACTION_UNKNOWN;
}

int md_action_parse(MdAction *action, const char *json, size_t json_len) {
    if (!action || !json || json_len == 0)
        return -1;

    memset(action, 0, sizeof(*action));

    cJSON *root = cJSON_ParseWithLength(json, json_len);
    if (!root)
        return -1;

    /* Version check */
    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "v");
    if (!cJSON_IsNumber(v) || v->valueint != 1) {
        cJSON_Delete(root);
        return -1;
    }

    /* Action type */
    cJSON *act = cJSON_GetObjectItemCaseSensitive(root, "action");
    if (!cJSON_IsString(act)) {
        cJSON_Delete(root);
        return -1;
    }
    action->type = md_action_type_from_str(act->valuestring);

    /* target_id (optional for key_combo) */
    cJSON *tid = cJSON_GetObjectItemCaseSensitive(root, "target_id");
    if (cJSON_IsString(tid)) {
        strncpy(action->target_id, tid->valuestring, sizeof(action->target_id) - 1);
        action->target_id[sizeof(action->target_id) - 1] = '\0';
    }

    /* Payload */
    cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    if (cJSON_IsObject(payload)) {
        cJSON *text = cJSON_GetObjectItemCaseSensitive(payload, "text");
        if (cJSON_IsString(text)) {
            strncpy(action->text, text->valuestring, sizeof(action->text) - 1);
            action->text[sizeof(action->text) - 1] = '\0';
        }

        cJSON *keys = cJSON_GetObjectItemCaseSensitive(payload, "keys");
        if (cJSON_IsArray(keys)) {
            int count = cJSON_GetArraySize(keys);
            if (count > MD_MAX_KEYS) count = MD_MAX_KEYS;
            for (int i = 0; i < count; i++) {
                cJSON *k = cJSON_GetArrayItem(keys, i);
                if (cJSON_IsString(k)) {
                    char *dup = strdup(k->valuestring);
                    if (!dup) {
                        /* Clean up already-allocated keys and fail */
                        md_action_cleanup(action);
                        cJSON_Delete(root);
                        return -1;
                    }
                    action->keys[action->key_count++] = dup;
                }
            }
        }

        cJSON *dx = cJSON_GetObjectItemCaseSensitive(payload, "dx");
        if (cJSON_IsNumber(dx)) action->dx = dx->valueint;

        cJSON *dy = cJSON_GetObjectItemCaseSensitive(payload, "dy");
        if (cJSON_IsNumber(dy)) action->dy = dy->valueint;

        cJSON *region = cJSON_GetObjectItemCaseSensitive(payload, "region");
        if (cJSON_IsArray(region) && cJSON_GetArraySize(region) == 4) {
            for (int i = 0; i < 4; i++) {
                cJSON *el = cJSON_GetArrayItem(region, i);
                if (cJSON_IsNumber(el))
                    action->region[i] = el->valueint;
            }
        }
    }

    cJSON_Delete(root);
    return 0;
}

char *md_action_encode(const MdAction *action) {
    if (!action)
        return NULL;

    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddNumberToObject(root, "v", 1);
    cJSON_AddStringToObject(root, "action", md_action_type_str(action->type));

    if (action->target_id[0] != '\0')
        cJSON_AddStringToObject(root, "target_id", action->target_id);

    cJSON *payload = cJSON_CreateObject();
    if (!payload) {
        cJSON_Delete(root);
        return NULL;
    }

    int has_payload = 0;

    if (action->text[0] != '\0') {
        cJSON_AddStringToObject(payload, "text", action->text);
        has_payload = 1;
    }

    if (action->key_count > 0) {
        cJSON *keys = cJSON_CreateArray();
        for (int i = 0; i < action->key_count; i++)
            cJSON_AddItemToArray(keys, cJSON_CreateString(action->keys[i]));
        cJSON_AddItemToObject(payload, "keys", keys);
        has_payload = 1;
    }

    if (action->dx != 0 || action->dy != 0) {
        cJSON_AddNumberToObject(payload, "dx", action->dx);
        cJSON_AddNumberToObject(payload, "dy", action->dy);
        has_payload = 1;
    }

    if (action->type == MD_ACTION_SCREENSHOT) {
        cJSON *region = cJSON_CreateIntArray(action->region, 4);
        cJSON_AddItemToObject(payload, "region", region);
        has_payload = 1;
    }

    if (has_payload)
        cJSON_AddItemToObject(root, "payload", payload);
    else
        cJSON_Delete(payload);

    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return str;
}

void md_action_cleanup(MdAction *action) {
    if (!action) return;
    for (int i = 0; i < action->key_count; i++) {
        free(action->keys[i]);
        action->keys[i] = NULL;
    }
    action->key_count = 0;
}
