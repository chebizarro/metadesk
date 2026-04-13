/*
 * metadesk — action.h
 * Action format encode/decode (JSON). See spec §3.2.
 */
#ifndef MD_ACTION_H
#define MD_ACTION_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Action types — spec §3.2 */
typedef enum {
    MD_ACTION_CLICK,
    MD_ACTION_DBL_CLICK,
    MD_ACTION_RIGHT_CLICK,
    MD_ACTION_TYPE,
    MD_ACTION_KEY_COMBO,
    MD_ACTION_SCROLL,
    MD_ACTION_FOCUS,
    MD_ACTION_SET_VALUE,
    MD_ACTION_SCREENSHOT,
    MD_ACTION_UNKNOWN,
} MdActionType;

/* Maximum keys in a combo */
#define MD_MAX_KEYS 8

/* Parsed action */
typedef struct {
    MdActionType type;
    char         target_id[64]; /* empty for key_combo */

    /* Payload fields — which are valid depends on type */
    char         text[1024];
    char        *keys[MD_MAX_KEYS];
    int          key_count;
    int          dx, dy;
    int          region[4]; /* x, y, w, h for screenshot */
} MdAction;

/*
 * Parse action from JSON string. Returns 0 on success.
 * action must be zeroed before calling.
 */
int md_action_parse(MdAction *action, const char *json, size_t json_len);

/*
 * Encode action to JSON string. Caller frees returned string.
 * Returns NULL on error.
 */
char *md_action_encode(const MdAction *action);

/* Convert action type to/from string */
const char *md_action_type_str(MdActionType type);
MdActionType md_action_type_from_str(const char *str);

/* Free internal key strings in an MdAction */
void md_action_cleanup(MdAction *action);

#ifdef __cplusplus
}
#endif

#endif /* MD_ACTION_H */
