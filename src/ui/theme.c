/*
 * ORDL UI — Theme system
 * TOML-based + hardcoded presets (dark, light, grok, tokyo, rosepine).
 */

#include "forge/ui/ordl_ui.h"
#include "forge/ui/ordl_infercli_toml.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------- */
/* Theme struct                                                               */
/* -------------------------------------------------------------------------- */

struct ui_theme {
    ui_style_set_t *styles; /* per widget class */
    char **classes;
    size_t count, cap;
};

/* -------------------------------------------------------------------------- */
/* Color helpers                                                              */
/* -------------------------------------------------------------------------- */

static ui_color_t parse_hex(const char *s) {
    if (!s || s[0] != '#') return ui_rgb(255, 255, 255);
    size_t len = strlen(s);
    unsigned int r = 255, g = 255, b = 255, a = 255;
    if (len == 7) {
        sscanf(s + 1, "%02x%02x%02x", &r, &g, &b);
    } else if (len == 9) {
        sscanf(s + 1, "%02x%02x%02x%02x", &r, &g, &b, &a);
    }
    return ui_rgba((uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)a);
}

/* -------------------------------------------------------------------------- */
/* Theme lifecycle                                                            */
/* -------------------------------------------------------------------------- */

ui_theme_t *ui_theme_load_default(void) {
    return ui_theme_dark();
}

static ui_theme_t *theme_alloc(void) {
    ui_theme_t *t = calloc(1, sizeof(ui_theme_t));
    if (!t) return NULL;
    t->cap = 16;
    t->classes = calloc(t->cap, sizeof(char *));
    t->styles = calloc(t->cap, sizeof(ui_style_set_t));
    if (!t->classes || !t->styles) {
        free(t->classes); free(t->styles); free(t);
        return NULL;
    }
    return t;
}

void ui_theme_free(ui_theme_t *t) {
    if (!t) return;
    for (size_t i = 0; i < t->count; i++) free(t->classes[i]);
    free(t->classes);
    free(t->styles);
    free(t);
}

ui_style_set_t *ui_theme_get_style(ui_theme_t *t, const char *widget_class) {
    if (!t || !widget_class) return NULL;
    for (size_t i = 0; i < t->count; i++) {
        if (strcmp(t->classes[i], widget_class) == 0) return &t->styles[i];
    }
    return NULL;
}

static ui_style_set_t *theme_ensure_style(ui_theme_t *t, const char *widget_class) {
    ui_style_set_t *s = ui_theme_get_style(t, widget_class);
    if (s) return s;
    if (t->count >= t->cap) {
        t->cap *= 2;
        t->classes = realloc(t->classes, t->cap * sizeof(char *));
        t->styles = realloc(t->styles, t->cap * sizeof(ui_style_set_t));
    }
    if (!t->classes || !t->styles) return NULL;
    size_t idx = t->count++;
    t->classes[idx] = strdup(widget_class);
    if (!t->classes[idx]) { t->count--; return NULL; }
    /* The class string is owned by the theme and freed by ui_theme_free().
       GCC analyzer cannot track cross-function ownership; suppress false positive. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-malloc-leak"
    memset(&t->styles[idx], 0, sizeof(ui_style_set_t));
#pragma GCC diagnostic pop
    return &t->styles[idx];
}

void ui_widget_apply_theme(ui_widget_t *w, ui_theme_t *t) {
    if (!w || !t) return;
    const char *class_name = NULL;
    switch (w->type) {
        case UI_WIDGET_BOX:       class_name = "box"; break;
        case UI_WIDGET_LABEL:     class_name = "label"; break;
        case UI_WIDGET_BUTTON:    class_name = "button"; break;
        case UI_WIDGET_INPUT:     class_name = "input"; break;
        case UI_WIDGET_TEXTAREA:  class_name = "textarea"; break;
        case UI_WIDGET_SCROLL:    class_name = "scroll"; break;
        case UI_WIDGET_SPLIT:     class_name = "split"; break;
        case UI_WIDGET_TABS:      class_name = "tabs"; break;
        case UI_WIDGET_TABLE:     class_name = "table"; break;
        case UI_WIDGET_TREE:      class_name = "tree"; break;
        case UI_WIDGET_MENUBAR:   class_name = "menubar"; break;
        case UI_WIDGET_STATUSBAR: class_name = "statusbar"; break;
        default: break;
    }
    if (class_name) {
        ui_style_set_t *s = ui_theme_get_style(t, class_name);
        if (s) {
            free(w->style);
            w->style = calloc(1, sizeof(ui_style_set_t));
            if (!w->style) return;
            memcpy(w->style, s, sizeof(ui_style_set_t));
            /* Inherit NORMAL to any state that wasn't explicitly themed.
               Prevents invisible black-on-black focus/hover styles. */
            for (int i = 0; i < UI_STATE_COUNT; i++) {
                if (i == UI_STATE_NORMAL) continue;
                ui_style_t *st = &w->style->states[i];
                if (st->fg.a == 0 && st->bg.a == 0) {
                    w->style->states[i] = w->style->states[UI_STATE_NORMAL];
                }
            }
        }
    }
    for (size_t i = 0; i < w->child_count; i++) {
        ui_widget_apply_theme(w->children[i], t);
    }
}

/* -------------------------------------------------------------------------- */
/* TOML loading                                                               */
/* -------------------------------------------------------------------------- */

ui_theme_t *ui_theme_load_toml(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0 || len > (long)(16 * 1024 * 1024)) { fclose(f); return NULL; } /* sanity limit 16MB */
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) { free(buf); fclose(f); return NULL; }
    buf[len] = '\0';
    fclose(f);

    arena_t arena;
    arena_init(&arena, 65536);
    toml_value_t *root = toml_parse(buf, &arena);
    free(buf);
    if (!root || root->type != TOML_TABLE) {
        arena_free_all(&arena);
        return NULL;
    }

    ui_theme_t *t = theme_alloc();
    if (!t) { arena_free_all(&arena); return NULL; }

    for (size_t i = 0; i < root->tbl.count; i++) {
        const char *section = root->tbl.keys[i];
        toml_value_t *sec_val = root->tbl.vals[i];
        if (sec_val->type != TOML_TABLE) continue;

        /* Section name: "widget_class.state" or "widget_class" */
        char widget_class[64] = {0};
        char state_name[16] = {0};
        const char *dot = strchr(section, '.');
        if (dot) {
            size_t clen = (size_t)(dot - section);
            if (clen >= sizeof(widget_class)) clen = sizeof(widget_class) - 1;
            memcpy(widget_class, section, clen);
            strncpy(state_name, dot + 1, sizeof(state_name) - 1);
        } else {
            strncpy(widget_class, section, sizeof(widget_class) - 1);
            strcpy(state_name, "normal");
        }

        ui_style_set_t *set = theme_ensure_style(t, widget_class);
        int state_idx = UI_STATE_NORMAL;
        if (strcmp(state_name, "hover") == 0) state_idx = UI_STATE_HOVER;
        else if (strcmp(state_name, "active") == 0) state_idx = UI_STATE_ACTIVE;
        else if (strcmp(state_name, "focused") == 0) state_idx = UI_STATE_FOCUSED;
        else if (strcmp(state_name, "disabled") == 0) state_idx = UI_STATE_DISABLED;

        ui_style_t *st = &set->states[state_idx];
        for (size_t j = 0; j < sec_val->tbl.count; j++) {
            const char *key = sec_val->tbl.keys[j];
            toml_value_t *v = sec_val->tbl.vals[j];
            if (strcmp(key, "fg") == 0 && v->type == TOML_STRING) st->fg = parse_hex(v->s);
            else if (strcmp(key, "bg") == 0 && v->type == TOML_STRING) st->bg = parse_hex(v->s);
            else if (strcmp(key, "border") == 0 && v->type == TOML_STRING) st->border = parse_hex(v->s);
            else if (strcmp(key, "border_width") == 0 && v->type == TOML_INT) st->border_width = (int)v->i;
            else if (strcmp(key, "radius") == 0 && v->type == TOML_INT) st->radius = (int)v->i;
        }
    }

    arena_free_all(&arena);
    return t;
}

/* -------------------------------------------------------------------------- */
/* Hardcoded presets                                                          */
/* -------------------------------------------------------------------------- */

static void theme_add(ui_theme_t *t, const char *cls, int state,
                      uint32_t fg, uint32_t bg, uint32_t border) {
    ui_style_set_t *set = theme_ensure_style(t, cls);
    if (!set) return;
    set->states[state].fg = ui_rgb((fg >> 16) & 0xFF, (fg >> 8) & 0xFF, fg & 0xFF);
    set->states[state].bg = ui_rgb((bg >> 16) & 0xFF, (bg >> 8) & 0xFF, bg & 0xFF);
    set->states[state].border = ui_rgb((border >> 16) & 0xFF, (border >> 8) & 0xFF, border & 0xFF);
}

ui_theme_t *ui_theme_dark(void) {
    ui_theme_t *t = theme_alloc();
    if (!t) return NULL;
    uint32_t blk = 0x000000, wht = 0xFFFFFF, gry = 0x808080, dgr = 0x303030, lgr = 0xC8C8C8;
    uint32_t blu = 0x6496FF;
    theme_add(t, "box",      UI_STATE_NORMAL, lgr, blk, gry);
    theme_add(t, "label",    UI_STATE_NORMAL, lgr, blk, blk);
    theme_add(t, "button",   UI_STATE_NORMAL, wht, dgr, gry);
    theme_add(t, "button",   UI_STATE_HOVER,  wht, 0x505050, blu);
    theme_add(t, "button",   UI_STATE_ACTIVE, wht, 0x202020, blu);
    theme_add(t, "input",    UI_STATE_NORMAL, lgr, 0x1E1E1E, gry);
    theme_add(t, "input",    UI_STATE_FOCUSED,lgr, 0x282828, blu);
    theme_add(t, "textarea", UI_STATE_NORMAL, lgr, 0x1E1E1E, gry);
    theme_add(t, "scroll",   UI_STATE_NORMAL, lgr, blk, blk);
    theme_add(t, "split",    UI_STATE_NORMAL, lgr, blk, gry);
    theme_add(t, "tabs",     UI_STATE_NORMAL, lgr, dgr, gry);
    theme_add(t, "table",    UI_STATE_NORMAL, lgr, blk, gry);
    theme_add(t, "tree",     UI_STATE_NORMAL, lgr, blk, blk);
    theme_add(t, "menubar",  UI_STATE_NORMAL, lgr, dgr, gry);
    theme_add(t, "statusbar",UI_STATE_NORMAL, lgr, 0x282828, blk);
    return t;
}

ui_theme_t *ui_theme_light(void) {
    ui_theme_t *t = theme_alloc();
    if (!t) return NULL;
    uint32_t wht = 0xFFFFFF, blk = 0x000000, gry = 0x808080, lgr = 0xE0E0E0;
    uint32_t blu = 0x0066CC;
    theme_add(t, "box",      UI_STATE_NORMAL, blk, wht, gry);
    theme_add(t, "label",    UI_STATE_NORMAL, blk, wht, wht);
    theme_add(t, "button",   UI_STATE_NORMAL, blk, lgr, gry);
    theme_add(t, "button",   UI_STATE_HOVER,  blk, 0xD0D0D0, blu);
    theme_add(t, "button",   UI_STATE_ACTIVE, blk, 0xB0B0B0, blu);
    theme_add(t, "input",    UI_STATE_NORMAL, blk, wht, gry);
    theme_add(t, "input",    UI_STATE_FOCUSED,blk, wht, blu);
    theme_add(t, "textarea", UI_STATE_NORMAL, blk, wht, gry);
    theme_add(t, "scroll",   UI_STATE_NORMAL, blk, wht, wht);
    theme_add(t, "split",    UI_STATE_NORMAL, blk, wht, gry);
    theme_add(t, "tabs",     UI_STATE_NORMAL, blk, lgr, gry);
    theme_add(t, "table",    UI_STATE_NORMAL, blk, wht, gry);
    theme_add(t, "tree",     UI_STATE_NORMAL, blk, wht, wht);
    theme_add(t, "menubar",  UI_STATE_NORMAL, blk, lgr, gry);
    theme_add(t, "statusbar",UI_STATE_NORMAL, blk, 0xF0F0F0, wht);
    return t;
}

ui_theme_t *ui_theme_grok(void) {
    ui_theme_t *t = theme_alloc();
    if (!t) return NULL;
    uint32_t bg = 0x0A0A0A, fg = 0xE8E8E8, accent = 0xFF6B35, muted = 0x5A5A5A;
    uint32_t panel = 0x141414, border = 0x333333;
    theme_add(t, "box",      UI_STATE_NORMAL, fg, bg, border);
    theme_add(t, "label",    UI_STATE_NORMAL, fg, bg, bg);
    theme_add(t, "button",   UI_STATE_NORMAL, fg, panel, border);
    theme_add(t, "button",   UI_STATE_HOVER,  bg, accent, accent);
    theme_add(t, "button",   UI_STATE_ACTIVE, fg, 0xD95A2B, accent);
    theme_add(t, "input",    UI_STATE_NORMAL, fg, panel, border);
    theme_add(t, "input",    UI_STATE_FOCUSED,fg, 0x1E1E1E, accent);
    theme_add(t, "textarea", UI_STATE_NORMAL, fg, panel, border);
    theme_add(t, "scroll",   UI_STATE_NORMAL, fg, bg, bg);
    theme_add(t, "split",    UI_STATE_NORMAL, fg, bg, border);
    theme_add(t, "tabs",     UI_STATE_NORMAL, fg, panel, border);
    theme_add(t, "table",    UI_STATE_NORMAL, fg, bg, border);
    theme_add(t, "tree",     UI_STATE_NORMAL, fg, bg, bg);
    theme_add(t, "menubar",  UI_STATE_NORMAL, fg, panel, border);
    theme_add(t, "statusbar",UI_STATE_NORMAL, muted, 0x0F0F0F, bg);
    return t;
}

ui_theme_t *ui_theme_tokyo(void) {
    ui_theme_t *t = theme_alloc();
    if (!t) return NULL;
    uint32_t bg = 0x1A1B26, fg = 0xA9B1D6, cyan = 0x7AA2F7;
    uint32_t panel = 0x24283B, border = 0x414868;
    theme_add(t, "box",      UI_STATE_NORMAL, fg, bg, border);
    theme_add(t, "label",    UI_STATE_NORMAL, fg, bg, bg);
    theme_add(t, "button",   UI_STATE_NORMAL, fg, panel, border);
    theme_add(t, "button",   UI_STATE_HOVER,  bg, cyan, cyan);
    theme_add(t, "button",   UI_STATE_ACTIVE, bg, 0x5D87E6, cyan);
    theme_add(t, "input",    UI_STATE_NORMAL, fg, panel, border);
    theme_add(t, "input",    UI_STATE_FOCUSED,fg, 0x2E3347, cyan);
    theme_add(t, "textarea", UI_STATE_NORMAL, fg, panel, border);
    theme_add(t, "scroll",   UI_STATE_NORMAL, fg, bg, bg);
    theme_add(t, "split",    UI_STATE_NORMAL, fg, bg, border);
    theme_add(t, "tabs",     UI_STATE_NORMAL, fg, panel, border);
    theme_add(t, "table",    UI_STATE_NORMAL, fg, bg, border);
    theme_add(t, "tree",     UI_STATE_NORMAL, fg, bg, bg);
    theme_add(t, "menubar",  UI_STATE_NORMAL, fg, panel, border);
    theme_add(t, "statusbar",UI_STATE_NORMAL, 0x565F89, 0x16161E, bg);
    return t;
}

ui_theme_t *ui_theme_rosepine(void) {
    ui_theme_t *t = theme_alloc();
    if (!t) return NULL;
    uint32_t base = 0x191724, text = 0xE0DEF4, rose = 0xEBBCBA;
    uint32_t overlay = 0x26233A, border = 0x524F67;
    theme_add(t, "box",      UI_STATE_NORMAL, text, base, border);
    theme_add(t, "label",    UI_STATE_NORMAL, text, base, base);
    theme_add(t, "button",   UI_STATE_NORMAL, text, overlay, border);
    theme_add(t, "button",   UI_STATE_HOVER,  base, rose, rose);
    theme_add(t, "button",   UI_STATE_ACTIVE, base, 0xD4A3A1, rose);
    theme_add(t, "input",    UI_STATE_NORMAL, text, overlay, border);
    theme_add(t, "input",    UI_STATE_FOCUSED,text, 0x312C45, rose);
    theme_add(t, "textarea", UI_STATE_NORMAL, text, overlay, border);
    theme_add(t, "scroll",   UI_STATE_NORMAL, text, base, base);
    theme_add(t, "split",    UI_STATE_NORMAL, text, base, border);
    theme_add(t, "tabs",     UI_STATE_NORMAL, text, overlay, border);
    theme_add(t, "table",    UI_STATE_NORMAL, text, base, border);
    theme_add(t, "tree",     UI_STATE_NORMAL, text, base, base);
    theme_add(t, "menubar",  UI_STATE_NORMAL, text, overlay, border);
    theme_add(t, "statusbar",UI_STATE_NORMAL, 0x6E6A86, 0x13111F, base);
    return t;
}
