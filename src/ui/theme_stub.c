/*
 * FORGE UI — Theme stubs
 * Minimal implementations until full TOML-based theme loading is integrated.
 */

#include "forge/ui/ordl_ui.h"
#include <stdlib.h>

/* Stub theme — returns NULL to indicate no theme loaded */
ui_theme_t *ui_theme_load_default(void) { return NULL; }
ui_theme_t *ui_theme_load_toml(const char *path) { (void)path; return NULL; }
void ui_theme_free(ui_theme_t *t) { (void)t; }
ui_style_set_t *ui_theme_get_style(ui_theme_t *t, const char *widget_class) {
    (void)t; (void)widget_class; return NULL;
}
void ui_widget_apply_theme(ui_widget_t *w, ui_theme_t *t) { (void)w; (void)t; }

/* Predefined themes — all return NULL (use inline styles instead) */
ui_theme_t *ui_theme_dark(void)    { return NULL; }
ui_theme_t *ui_theme_light(void)   { return NULL; }
ui_theme_t *ui_theme_grok(void)    { return NULL; }
ui_theme_t *ui_theme_tokyo(void)   { return NULL; }
ui_theme_t *ui_theme_rosepine(void){ return NULL; }
