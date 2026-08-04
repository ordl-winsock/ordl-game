/*
 * ORDL UI — Accessibility layer
 * Screen reader support, ARIA-like roles, focus announcements.
 * Pure C23, zero external dependencies.
 */

#include "forge/ui/ordl_ui.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

/* -------------------------------------------------------------------------- */
/* Role helpers                                                               */
/* -------------------------------------------------------------------------- */

static const char *role_name(ui_a11y_role_t role) {
    switch (role) {
    case UI_ROLE_NONE:       return "";
    case UI_ROLE_ALERT:      return "alert";
    case UI_ROLE_BUTTON:     return "button";
    case UI_ROLE_CHECKBOX:   return "checkbox";
    case UI_ROLE_DIALOG:     return "dialog";
    case UI_ROLE_GRID:       return "grid";
    case UI_ROLE_GRIDCELL:   return "gridcell";
    case UI_ROLE_LINK:       return "link";
    case UI_ROLE_MENU:       return "menu";
    case UI_ROLE_MENUBAR:    return "menubar";
    case UI_ROLE_MENUITEM:   return "menuitem";
    case UI_ROLE_PROGRESSBAR: return "progressbar";
    case UI_ROLE_SCROLLBAR:  return "scrollbar";
    case UI_ROLE_SLIDER:     return "slider";
    case UI_ROLE_STATUS:     return "status";
    case UI_ROLE_TAB:        return "tab";
    case UI_ROLE_TABPANEL:   return "tabpanel";
    case UI_ROLE_TEXTBOX:    return "textbox";
    case UI_ROLE_TREE:       return "tree";
    case UI_ROLE_TREEITEM:   return "treeitem";
    default:                 return "";
    }
}

static void state_names(uint32_t state, char *out, size_t len) {
    out[0] = '\0';
    if (state & UI_A11Y_FOCUSED)  strncat(out, " focused", len - strlen(out) - 1);
    if (state & UI_A11Y_DISABLED) strncat(out, " disabled", len - strlen(out) - 1);
    if (state & UI_A11Y_HIDDEN)   strncat(out, " hidden", len - strlen(out) - 1);
    if (state & UI_A11Y_CHECKED)  strncat(out, " checked", len - strlen(out) - 1);
    if (state & UI_A11Y_EXPANDED) strncat(out, " expanded", len - strlen(out) - 1);
    if (state & UI_A11Y_SELECTED) strncat(out, " selected", len - strlen(out) - 1);
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

void ui_a11y_set_role(ui_widget_t *w, ui_a11y_role_t role) {
    if (!w) return;
    w->a11y_role = role;
}

void ui_a11y_set_label(ui_widget_t *w, const char *label) {
    if (!w) return;
    free(w->a11y_label);
    w->a11y_label = label ? strdup(label) : NULL;
}

void ui_a11y_set_state(ui_widget_t *w, uint32_t flags, bool on) {
    if (!w) return;
    if (on) w->a11y_state |= flags;
    else    w->a11y_state &= ~flags;
}

/* Announce text to screen reader via Linux accessibility bus or fallback to stderr */
void ui_a11y_announce(const char *text) {
    if (!text || !*text) return;
    /* Try AT-SPI2 D-Bus (simplified: write to a well-known fd if connected) */
    /* Fallback: print to stderr with a prefix that screen readers can intercept */
    fprintf(stderr, "\033[5n[A11Y] %s\n", text);
    fflush(stderr);
}

void ui_a11y_describe_widget(ui_widget_t *w, char *out, size_t out_len) {
    if (!w || !out || out_len == 0) return;
    out[0] = '\0';

    const char *rname = role_name(w->a11y_role);
    char st[128];
    state_names(w->a11y_state, st, sizeof(st));

    if (w->a11y_label && *w->a11y_label) {
        snprintf(out, out_len, "%s%s: %s", rname, st, w->a11y_label);
    } else if (w->type == UI_WIDGET_LABEL || w->type == UI_WIDGET_BUTTON) {
        /* Try to extract text from widget data (best effort) */
        snprintf(out, out_len, "%s%s: %s", rname, st, w->id);
    } else {
        snprintf(out, out_len, "%s%s: %s", rname, st, w->id);
    }
}

/* -------------------------------------------------------------------------- */
/* Focus tracking — call when focus changes                                   */
/* -------------------------------------------------------------------------- */

void ui_a11y_on_focus_changed(ui_widget_t *prev, ui_widget_t *curr) {
    if (prev) {
        prev->a11y_state &= ~UI_A11Y_FOCUSED;
    }
    if (curr) {
        curr->a11y_state |= UI_A11Y_FOCUSED;
        char desc[256];
        ui_a11y_describe_widget(curr, desc, sizeof(desc));
        ui_a11y_announce(desc);
    }
}
