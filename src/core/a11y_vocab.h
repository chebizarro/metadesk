/* Shared wire vocabulary. Header-only so C11, ObjC and C++ backends cannot
 * acquire a separate linked vocabulary. Unknown native roles normalize to
 * "unknown"; targeted queries retain their raw-role fallback for external trees.
 */
#ifndef MD_A11Y_VOCAB_H
#define MD_A11Y_VOCAB_H
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define MD_A11Y_ROLES(X) \
    X(UNKNOWN, "unknown", "UNK", false) \
    X(WINDOW, "window", "WIN", true) \
    X(APPLICATION, "application", "APP", true) \
    X(DESKTOP, "desktop", "DSK", true) \
    X(DIALOG, "dialog", "DLG", true) \
    X(BUTTON, "button", "BTN", true) \
    X(TOGGLE_BUTTON, "toggle button", "TGL", true) \
    X(TEXT, "text", "TXT", true) \
    X(MENU, "menu", "MNU", true) \
    X(MENU_ITEM, "menu item", "MNI", true) \
    X(CHECK_BOX, "check box", "CHK", true) \
    X(RADIO_BUTTON, "radio button", "RAD", true) \
    X(COMBO_BOX, "combo box", "CMB", true) \
    X(LIST, "list", "LST", true) \
    X(LIST_ITEM, "list item", "LI", true) \
    X(TAB, "tab", "TAB", true) \
    X(LINK, "link", "LNK", true) \
    X(SLIDER, "slider", "SLD", true) \
    X(SPIN_BUTTON, "spin button", "SPN", true) \
    X(TREE, "tree", "TRE", true) \
    X(TABLE, "table", "TBL", true) \
    X(PANEL, "panel", "PNL", false) \
    X(LABEL, "label", "LBL", false) \
    X(SCROLL_BAR, "scroll bar", "SCR", false) \
    X(SEPARATOR, "separator", "SEP", false) \
    X(TOOL_BAR, "tool bar", "TBR", false) \
    X(IMAGE, "image", "IMG", false) \
    X(STATUS_BAR, "status bar", "STS", false) \
    X(PAGE_TAB_LIST, "page tab list", "PTL", false) \
    X(SPLIT_PANE, "split pane", "SPL", false) \
    X(PROGRESS_BAR, "progress bar", "PRG", false) \
    X(TREE_ITEM, "tree item", "TRI", true) \
    X(TABLE_ROW, "table row", "ROW", true) \
    X(TABLE_COLUMN, "table column", "COL", true) \
    X(TABLE_CELL, "table cell", "CEL", true) \
    X(SCROLL_PANE, "scroll pane", "SCP", false) \
    X(DOCUMENT, "document", "DOC", true) \
    X(DOCUMENT_WEB, "document web", "WEB", true) \
    X(HEADING, "heading", "HDG", true) \
    X(TOOL_TIP, "tool tip", "TIP", false) \
    X(CALENDAR, "calendar", "CAL", true) \
    X(CANVAS, "canvas", "CNV", false) \
    X(COLOR_CHOOSER, "color chooser", "CLR", true) \
    X(DATE_EDITOR, "date editor", "DAT", true) \
    X(FILE_CHOOSER, "file chooser", "FIL", true) \
    X(FONT_CHOOSER, "font chooser", "FNT", true) \
    X(COLUMN_HEADER, "column header", "CLH", true) \
    X(ROW_HEADER, "row header", "RWH", true) \
    X(PARAGRAPH, "paragraph", "PAR", false) \
    X(SECTION, "section", "SEC", false) \
    X(FORM, "form", "FRM", true) \
    X(TERMINAL, "terminal", "TRM", true) \
    X(NOTIFICATION, "notification", "NTF", false) \
    X(CHART, "chart", "CHT", false) \
    X(DOCUMENT_SPREADSHEET, "document spreadsheet", "XLS", true) \
    X(DOCUMENT_PRESENTATION, "document presentation", "PRS", true) \
    X(DOCUMENT_TEXT, "document text", "DTX", true) \
    X(DOCUMENT_EMAIL, "document email", "EML", true) \
    X(ARTICLE, "article", "ART", false) \
    X(LANDMARK, "landmark", "LMK", false) \
    X(COMMENT, "comment", "CMT", false) \
    X(DESCRIPTION_LIST, "description list", "DLS", false) \
    X(DESCRIPTION_TERM, "description term", "DTM", false) \
    X(DESCRIPTION_VALUE, "description value", "DVL", false)

/* Exhaustive names from libatspi 2.62 atspi_role_get_name(), including
 * historical "push button". Unsupported semantics explicitly map to UNKNOWN.
 * https://gnome.pages.gitlab.gnome.org/at-spi2-core/libatspi/enum.Role.html
 */
#define MD_A11Y_ROLE_ALIASES(X) \
    X("invalid", MD_A11Y_ROLE_UNKNOWN) \
    X("accelerator label", MD_A11Y_ROLE_LABEL) \
    X("alert", MD_A11Y_ROLE_DIALOG) \
    X("animation", MD_A11Y_ROLE_UNKNOWN) \
    X("arrow", MD_A11Y_ROLE_UNKNOWN) \
    X("check menu item", MD_A11Y_ROLE_CHECK_BOX) \
    X("desktop icon", MD_A11Y_ROLE_UNKNOWN) \
    X("desktop frame", MD_A11Y_ROLE_DESKTOP) \
    X("dial", MD_A11Y_ROLE_SLIDER) \
    X("directory pane", MD_A11Y_ROLE_PANEL) \
    X("drawing area", MD_A11Y_ROLE_CANVAS) \
    X("filler", MD_A11Y_ROLE_PANEL) \
    X("focus traversable", MD_A11Y_ROLE_PANEL) \
    X("frame", MD_A11Y_ROLE_WINDOW) \
    X("glass pane", MD_A11Y_ROLE_PANEL) \
    X("html container", MD_A11Y_ROLE_DOCUMENT_WEB) \
    X("icon", MD_A11Y_ROLE_IMAGE) \
    X("internal frame", MD_A11Y_ROLE_WINDOW) \
    X("layered pane", MD_A11Y_ROLE_PANEL) \
    X("menu bar", MD_A11Y_ROLE_MENU) \
    X("option pane", MD_A11Y_ROLE_PANEL) \
    X("page tab", MD_A11Y_ROLE_TAB) \
    X("password text", MD_A11Y_ROLE_TEXT) \
    X("popup menu", MD_A11Y_ROLE_MENU) \
    X("radio menu item", MD_A11Y_ROLE_RADIO_BUTTON) \
    X("root pane", MD_A11Y_ROLE_PANEL) \
    X("table column header", MD_A11Y_ROLE_COLUMN_HEADER) \
    X("table row header", MD_A11Y_ROLE_ROW_HEADER) \
    X("tearoff menu item", MD_A11Y_ROLE_MENU_ITEM) \
    X("tree table", MD_A11Y_ROLE_TREE) \
    X("viewport", MD_A11Y_ROLE_SCROLL_PANE) \
    X("extended", MD_A11Y_ROLE_UNKNOWN) \
    X("header", MD_A11Y_ROLE_HEADING) \
    X("footer", MD_A11Y_ROLE_PANEL) \
    X("ruler", MD_A11Y_ROLE_UNKNOWN) \
    X("autocomplete", MD_A11Y_ROLE_COMBO_BOX) \
    X("editbar", MD_A11Y_ROLE_TEXT) \
    X("embedded", MD_A11Y_ROLE_PANEL) \
    X("entry", MD_A11Y_ROLE_TEXT) \
    X("caption", MD_A11Y_ROLE_LABEL) \
    X("document frame", MD_A11Y_ROLE_DOCUMENT) \
    X("page", MD_A11Y_ROLE_DOCUMENT) \
    X("redundant object", MD_A11Y_ROLE_UNKNOWN) \
    X("input method window", MD_A11Y_ROLE_WINDOW) \
    X("list box", MD_A11Y_ROLE_LIST) \
    X("grouping", MD_A11Y_ROLE_PANEL) \
    X("image map", MD_A11Y_ROLE_IMAGE) \
    X("info bar", MD_A11Y_ROLE_STATUS_BAR) \
    X("level bar", MD_A11Y_ROLE_PROGRESS_BAR) \
    X("title bar", MD_A11Y_ROLE_LABEL) \
    X("block quote", MD_A11Y_ROLE_UNKNOWN) \
    X("audio", MD_A11Y_ROLE_UNKNOWN) \
    X("video", MD_A11Y_ROLE_UNKNOWN) \
    X("definition", MD_A11Y_ROLE_UNKNOWN) \
    X("log", MD_A11Y_ROLE_UNKNOWN) \
    X("marquee", MD_A11Y_ROLE_UNKNOWN) \
    X("math", MD_A11Y_ROLE_UNKNOWN) \
    X("rating", MD_A11Y_ROLE_UNKNOWN) \
    X("timer", MD_A11Y_ROLE_UNKNOWN) \
    X("static", MD_A11Y_ROLE_LABEL) \
    X("math fraction", MD_A11Y_ROLE_UNKNOWN) \
    X("math root", MD_A11Y_ROLE_UNKNOWN) \
    X("subscript", MD_A11Y_ROLE_UNKNOWN) \
    X("superscript", MD_A11Y_ROLE_UNKNOWN) \
    X("footnote", MD_A11Y_ROLE_UNKNOWN) \
    X("content deletion", MD_A11Y_ROLE_UNKNOWN) \
    X("content insertion", MD_A11Y_ROLE_UNKNOWN) \
    X("mark", MD_A11Y_ROLE_UNKNOWN) \
    X("suggestion", MD_A11Y_ROLE_UNKNOWN) \
    X("push button menu", MD_A11Y_ROLE_BUTTON) \
    X("switch", MD_A11Y_ROLE_TOGGLE_BUTTON) \
    X("push button", MD_A11Y_ROLE_BUTTON)

#define MD_A11Y_STATES(X) \
    X(ACTIVE, "active") \
    X(ENABLED, "enabled") \
    X(FOCUSED, "focused") \
    X(VISIBLE, "visible") \
    X(SHOWING, "showing") \
    X(SELECTED, "selected") \
    X(CHECKED, "checked") \
    X(EDITABLE, "editable") \
    X(EXPANDABLE, "expandable") \
    X(EXPANDED, "expanded") \
    X(FOCUSABLE, "focusable") \
    X(SENSITIVE, "sensitive") \
    X(SELECTABLE, "selectable") \
    X(MODAL, "modal") \
    X(MULTILINE, "multiline") \
    X(SINGLELINE, "singleline") \
    X(DEFUNCT, "defunct") \
    X(DISABLED, "disabled") \
    X(PRESSED, "pressed")

enum MdA11yRole {
#define MD_ROLE_ENUM(id, name, abbr, interactive) MD_A11Y_ROLE_##id,
    MD_A11Y_ROLES(MD_ROLE_ENUM)
#undef MD_ROLE_ENUM
    MD_A11Y_ROLE_COUNT
};
enum MdA11yState {
    MD_A11Y_STATE_UNKNOWN = -1,
#define MD_STATE_ENUM(id, name) MD_A11Y_STATE_##id,
    MD_A11Y_STATES(MD_STATE_ENUM)
#undef MD_STATE_ENUM
    MD_A11Y_STATE_COUNT
};

static inline const char *md_a11y_role_name(enum MdA11yRole role) {
    switch (role) {
#define MD_ROLE_NAME(id, name, abbr, interactive) case MD_A11Y_ROLE_##id: return name;
        MD_A11Y_ROLES(MD_ROLE_NAME)
#undef MD_ROLE_NAME
        default: return "unknown";
    }
}
static inline enum MdA11yRole md_a11y_role_lookup(const char *name) {
    if (!name) return MD_A11Y_ROLE_UNKNOWN;
#define MD_ROLE_LOOKUP(id, wire, abbr, interactive) if (strcmp(name, wire) == 0) return MD_A11Y_ROLE_##id;
    MD_A11Y_ROLES(MD_ROLE_LOOKUP)
#undef MD_ROLE_LOOKUP
#define MD_ALIAS_LOOKUP(wire, role) if (strcmp(name, wire) == 0) return role;
    MD_A11Y_ROLE_ALIASES(MD_ALIAS_LOOKUP)
#undef MD_ALIAS_LOOKUP
    return MD_A11Y_ROLE_UNKNOWN;
}
static inline const char *md_a11y_role_abbrev(const char *name) {
    if (!name) return "???";
    switch (md_a11y_role_lookup(name)) {
#define MD_ROLE_ABBREV(id, wire, abbr, interactive) case MD_A11Y_ROLE_##id: return abbr;
        MD_A11Y_ROLES(MD_ROLE_ABBREV)
#undef MD_ROLE_ABBREV
        default: return "UNK";
    }
}
static inline bool md_a11y_role_is_interactable(const char *name) {
    switch (md_a11y_role_lookup(name)) {
#define MD_ROLE_INTERACTIVE(id, wire, abbr, interactive) case MD_A11Y_ROLE_##id: return interactive;
        MD_A11Y_ROLES(MD_ROLE_INTERACTIVE)
#undef MD_ROLE_INTERACTIVE
        default: return false;
    }
}
static inline const char *md_a11y_state_name(enum MdA11yState state) {
    switch (state) {
#define MD_STATE_NAME(id, name) case MD_A11Y_STATE_##id: return name;
        MD_A11Y_STATES(MD_STATE_NAME)
#undef MD_STATE_NAME
        default: return NULL;
    }
}
static inline enum MdA11yState md_a11y_state_lookup(const char *name) {
    if (!name) return MD_A11Y_STATE_UNKNOWN;
#define MD_STATE_LOOKUP(id, wire) if (strcmp(name, wire) == 0) return MD_A11Y_STATE_##id;
    MD_A11Y_STATES(MD_STATE_LOOKUP)
#undef MD_STATE_LOOKUP
    return MD_A11Y_STATE_UNKNOWN;
}
#endif
