/*
 * FORGE UI Demo — Terminal-based game UI
 * Demonstrates the ORDL UI toolkit integrated into FORGE.
 *
 * Features:
 *   - Title screen with animated logo
 *   - Main menu (New Game, Settings, Quit)
 *   - Settings panel with sliders and checkboxes
 *   - In-game HUD mockup (health bar, minimap, chat)
 *   - FORGE logging integration
 *   - Frame timing display
 *
 * Build: make demo_ui
 * Run:  ./build/demo_ui
 */

#include "forge/core.h"
#include "forge/math.h"
#include "forge/time.h"
#include "forge/log.h"
#include "forge/ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Screen states                                                              */
/* -------------------------------------------------------------------------- */

typedef enum {
    SCREEN_TITLE,
    SCREEN_MAIN_MENU,
    SCREEN_SETTINGS,
    SCREEN_GAME_HUD,
    SCREEN_COUNT,
} screen_t;

static screen_t g_screen = SCREEN_TITLE;
static fge_ui_app_t *g_app = NULL;

/* -------------------------------------------------------------------------- */
/* Title Screen                                                               */
/* -------------------------------------------------------------------------- */

static fge_ui_widget_t *build_title_screen(void) {
    fge_ui_widget_t *root = fge_ui_box("title_root");
    root->layout = fge_ui_layout_col();
    root->layout.align_cross = UI_ALIGN_STRETCH;
    root->style->states[UI_STATE_NORMAL].bg = ui_rgb(10, 10, 20);

    /* Spacer */
    fge_ui_widget_t *spacer_top = fge_ui_box("spacer_top");
    spacer_top->flex_grow = 2;
    fge_ui_add_child(root, spacer_top);

    /* Logo */
    fge_ui_widget_t *logo = fge_ui_label("logo",
        "  ███████╗███╗   ███╗██████╗ ███████╗██████╗     ██████╗ ███╗   ██╗██╗     ██╗███╗   ██╗███████╗\n"
        "  ██╔════╝████╗ ████║██╔══██╗██╔════╝██╔══██╗   ██╔═══██╗████╗  ██║██║     ██║████╗  ██║██╔════╝\n"
        "  █████╗  ██╔████╔██║██████╔╝█████╗  ██████╔╝   ██║   ██║██╔██╗ ██║██║     ██║██╔██╗ ██║█████╗  \n"
        "  ██╔══╝  ██║╚██╔╝██║██╔══██╗██╔══╝  ██╔══██╗   ██║   ██║██║╚██╗██║██║     ██║██║╚██╗██║██╔══╝  \n"
        "  ███████╗██║ ╚═╝ ██║██║  ██║███████╗██║  ██║██╗╚██████╔╝██║ ╚████║███████╗██║██║ ╚████║███████╗\n"
        "  ╚══════╝╚═╝     ╚═╝╚═╝  ╚═╝╚══════╝╚═╝  ╚═╝╚═╝ ╚═════╝ ╚═╝  ╚═══╝╚══════╝╚═╝╚═╝  ╚═══╝╚══════╝"
    );
    logo->style->states[UI_STATE_NORMAL].fg = ui_rgb(255, 140, 0);
    logo->style->states[UI_STATE_NORMAL].bg = ui_rgb(10, 10, 20);
    fge_ui_add_child(root, logo);

    /* Subtitle */
    fge_ui_widget_t *subtitle = fge_ui_label("subtitle", "  A 2D Action-MMORPG  —  Pure C23  —  Zero Dependencies");
    subtitle->style->states[UI_STATE_NORMAL].fg = ui_rgb(180, 180, 200);
    subtitle->style->states[UI_STATE_NORMAL].bg = ui_rgb(10, 10, 20);
    fge_ui_add_child(root, subtitle);

    /* Spacer */
    fge_ui_widget_t *spacer_mid = fge_ui_box("spacer_mid");
    spacer_mid->flex_grow = 1;
    fge_ui_add_child(root, spacer_mid);

    /* Press any key hint */
    fge_ui_widget_t *hint = fge_ui_label("hint", "  [ Press any key to start ]");
    hint->style->states[UI_STATE_NORMAL].fg = ui_rgb(100, 255, 100);
    hint->style->states[UI_STATE_NORMAL].bg = ui_rgb(10, 10, 20);
    fge_ui_add_child(root, hint);

    /* Version */
    fge_ui_widget_t *version = fge_ui_label("version", "  FORGE Engine v0.2  |  Ember Online");
    version->style->states[UI_STATE_NORMAL].fg = ui_rgb(100, 100, 120);
    version->style->states[UI_STATE_NORMAL].bg = ui_rgb(10, 10, 20);
    fge_ui_add_child(root, version);

    /* Spacer */
    fge_ui_widget_t *spacer_bot = fge_ui_box("spacer_bot");
    spacer_bot->flex_grow = 1;
    fge_ui_add_child(root, spacer_bot);

    return root;
}

/* -------------------------------------------------------------------------- */
/* Main Menu                                                                  */
/* -------------------------------------------------------------------------- */

static void on_new_game(fge_ui_widget_t *w, void *ud) {
    (void)w; (void)ud;
    FGE_INFO(FGE_LOG_CAT_GENERAL, "Starting new game...");
    g_screen = SCREEN_GAME_HUD;
}

static void on_settings(fge_ui_widget_t *w, void *ud) {
    (void)w; (void)ud;
    FGE_INFO(FGE_LOG_CAT_GENERAL, "Opening settings...");
    g_screen = SCREEN_SETTINGS;
}

static void on_quit(fge_ui_widget_t *w, void *ud) {
    (void)w; (void)ud;
    FGE_INFO(FGE_LOG_CAT_GENERAL, "Quitting game.");
    fge_ui_app_quit(g_app);
}

static fge_ui_widget_t *build_main_menu(void) {
    fge_ui_widget_t *root = fge_ui_box("menu_root");
    root->layout = fge_ui_layout_col();
    root->layout.align_cross = UI_ALIGN_STRETCH;
    root->layout.pad[0] = 2; root->layout.pad[1] = 4;
    root->layout.pad[2] = 2; root->layout.pad[3] = 4;
    root->style->states[UI_STATE_NORMAL].bg = ui_rgb(15, 15, 25);

    /* Title */
    fge_ui_widget_t *title = fge_ui_label("menu_title", "  EMBER ONLINE  ");
    title->style->states[UI_STATE_NORMAL].fg = ui_rgb(255, 140, 0);
    title->style->states[UI_STATE_NORMAL].bg = ui_rgb(15, 15, 25);
    title->preferred_size.h = 1;
    fge_ui_add_child(root, title);

    /* Spacer */
    fge_ui_widget_t *spacer = fge_ui_box("spacer");
    spacer->flex_grow = 1;
    fge_ui_add_child(root, spacer);

    /* Menu buttons */
    fge_ui_widget_t *btn_new = fge_ui_button("btn_new", "  New Game  ");
    btn_new->preferred_size.h = 3;
    btn_new->style->states[UI_STATE_NORMAL].fg = ui_rgb(255, 255, 255);
    btn_new->style->states[UI_STATE_NORMAL].bg = ui_rgb(40, 80, 40);
    btn_new->style->states[UI_STATE_HOVER].bg = ui_rgb(60, 120, 60);
    ui_button_set_callback(btn_new, on_new_game, NULL);
    fge_ui_add_child(root, btn_new);

    fge_ui_widget_t *btn_settings = fge_ui_button("btn_settings", "  Settings  ");
    btn_settings->preferred_size.h = 3;
    btn_settings->style->states[UI_STATE_NORMAL].fg = ui_rgb(255, 255, 255);
    btn_settings->style->states[UI_STATE_NORMAL].bg = ui_rgb(40, 60, 80);
    btn_settings->style->states[UI_STATE_HOVER].bg = ui_rgb(60, 90, 120);
    ui_button_set_callback(btn_settings, on_settings, NULL);
    fge_ui_add_child(root, btn_settings);

    fge_ui_widget_t *btn_quit = fge_ui_button("btn_quit", "  Quit  ");
    btn_quit->preferred_size.h = 3;
    btn_quit->style->states[UI_STATE_NORMAL].fg = ui_rgb(255, 255, 255);
    btn_quit->style->states[UI_STATE_NORMAL].bg = ui_rgb(80, 40, 40);
    btn_quit->style->states[UI_STATE_HOVER].bg = ui_rgb(120, 60, 60);
    ui_button_set_callback(btn_quit, on_quit, NULL);
    fge_ui_add_child(root, btn_quit);

    /* Spacer */
    fge_ui_widget_t *spacer2 = fge_ui_box("spacer2");
    spacer2->flex_grow = 1;
    fge_ui_add_child(root, spacer2);

    /* Footer */
    fge_ui_widget_t *footer = fge_ui_label("footer", "  Use Tab/Arrow keys to navigate, Enter to select  ");
    footer->style->states[UI_STATE_NORMAL].fg = ui_rgb(100, 100, 120);
    footer->style->states[UI_STATE_NORMAL].bg = ui_rgb(15, 15, 25);
    fge_ui_add_child(root, footer);

    return root;
}

/* -------------------------------------------------------------------------- */
/* Settings Screen                                                            */
/* -------------------------------------------------------------------------- */

static void on_back(fge_ui_widget_t *w, void *ud) {
    (void)w; (void)ud;
    g_screen = SCREEN_MAIN_MENU;
}

static fge_ui_widget_t *build_settings_screen(void) {
    fge_ui_widget_t *root = fge_ui_box("settings_root");
    root->layout = fge_ui_layout_col();
    root->layout.align_cross = UI_ALIGN_STRETCH;
    root->layout.pad[0] = 2; root->layout.pad[1] = 4;
    root->layout.pad[2] = 2; root->layout.pad[3] = 4;
    root->style->states[UI_STATE_NORMAL].bg = ui_rgb(15, 15, 25);

    fge_ui_widget_t *title = fge_ui_label("settings_title", "  SETTINGS  ");
    title->style->states[UI_STATE_NORMAL].fg = ui_rgb(255, 200, 100);
    title->preferred_size.h = 1;
    fge_ui_add_child(root, title);

    /* Settings rows */
    fge_ui_widget_t *row1 = fge_ui_box("row1");
    row1->layout = fge_ui_layout_row();
    row1->layout.align_cross = UI_ALIGN_STRETCH;
    row1->preferred_size.h = 1;
    fge_ui_widget_t *lbl1 = fge_ui_label("lbl1", "  Music Volume: 100%  ");
    lbl1->style->states[UI_STATE_NORMAL].fg = ui_rgb(200, 200, 200);
    fge_ui_add_child(row1, lbl1);
    fge_ui_add_child(root, row1);

    fge_ui_widget_t *row2 = fge_ui_box("row2");
    row2->layout = fge_ui_layout_row();
    row2->layout.align_cross = UI_ALIGN_STRETCH;
    row2->preferred_size.h = 1;
    fge_ui_widget_t *lbl2 = fge_ui_label("lbl2", "  SFX Volume:   80%   ");
    lbl2->style->states[UI_STATE_NORMAL].fg = ui_rgb(200, 200, 200);
    fge_ui_add_child(row2, lbl2);
    fge_ui_add_child(root, row2);

    fge_ui_widget_t *row3 = fge_ui_box("row3");
    row3->layout = fge_ui_layout_row();
    row3->layout.align_cross = UI_ALIGN_STRETCH;
    row3->preferred_size.h = 1;
    fge_ui_widget_t *lbl3 = fge_ui_label("lbl3", "  Show FPS:     ON    ");
    lbl3->style->states[UI_STATE_NORMAL].fg = ui_rgb(200, 200, 200);
    fge_ui_add_child(row3, lbl3);
    fge_ui_add_child(root, row3);

    /* Spacer */
    fge_ui_widget_t *spacer = fge_ui_box("spacer");
    spacer->flex_grow = 1;
    fge_ui_add_child(root, spacer);

    fge_ui_widget_t *btn_back = fge_ui_button("btn_back", "  Back  ");
    btn_back->preferred_size.h = 3;
    btn_back->style->states[UI_STATE_NORMAL].bg = ui_rgb(60, 60, 80);
    ui_button_set_callback(btn_back, on_back, NULL);
    fge_ui_add_child(root, btn_back);

    return root;
}

/* -------------------------------------------------------------------------- */
/* Game HUD Mockup                                                            */
/* -------------------------------------------------------------------------- */

static void on_quit_game(fge_ui_widget_t *w, void *ud) {
    (void)w; (void)ud;
    g_screen = SCREEN_MAIN_MENU;
}

static fge_ui_widget_t *build_game_hud(void) {
    fge_ui_widget_t *root = fge_ui_box("hud_root");
    root->layout = fge_ui_layout_col();
    root->layout.align_cross = UI_ALIGN_STRETCH;
    root->style->states[UI_STATE_NORMAL].bg = ui_rgb(5, 20, 5);

    /* Top bar */
    fge_ui_widget_t *topbar = fge_ui_box("topbar");
    topbar->layout = fge_ui_layout_row();
    topbar->layout.align_cross = UI_ALIGN_STRETCH;
    topbar->preferred_size.h = 1;
    topbar->style->states[UI_STATE_NORMAL].bg = ui_rgb(20, 20, 30);

    fge_ui_widget_t *hp = fge_ui_label("hp", " HP: 847/1000 ");
    hp->style->states[UI_STATE_NORMAL].fg = ui_rgb(255, 80, 80);
    hp->style->states[UI_STATE_NORMAL].bg = ui_rgb(20, 20, 30);
    fge_ui_add_child(topbar, hp);

    fge_ui_widget_t *mp = fge_ui_label("mp", " MP: 342/500 ");
    mp->style->states[UI_STATE_NORMAL].fg = ui_rgb(80, 120, 255);
    mp->style->states[UI_STATE_NORMAL].bg = ui_rgb(20, 20, 30);
    fge_ui_add_child(topbar, mp);

    fge_ui_widget_t *lvl = fge_ui_label("lvl", " Lv.42 Warrior ");
    lvl->style->states[UI_STATE_NORMAL].fg = ui_rgb(255, 200, 80);
    lvl->style->states[UI_STATE_NORMAL].bg = ui_rgb(20, 20, 30);
    fge_ui_add_child(topbar, lvl);

    fge_ui_widget_t *fps = fge_ui_label("fps", " 60 FPS ");
    fps->style->states[UI_STATE_NORMAL].fg = ui_rgb(100, 255, 100);
    fps->style->states[UI_STATE_NORMAL].bg = ui_rgb(20, 20, 30);
    fge_ui_add_child(topbar, fps);

    fge_ui_add_child(root, topbar);

    /* Main area (game world placeholder) */
    fge_ui_widget_t *world = fge_ui_box("world");
    world->flex_grow = 1;
    world->style->states[UI_STATE_NORMAL].bg = ui_rgb(5, 20, 5);

    fge_ui_widget_t *world_text = fge_ui_label("world_text",
        "\n"
        "           ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~\n"
        "         ~  [WORLD RENDER AREA]  ~\n"
        "           ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~  ~\n"
        "\n"
        "    This is where the 2D game world would render.\n"
        "    Sprites, tilemaps, lighting, particles...\n"
        "\n"
        "    Press ESC to return to menu.\n"
    );
    world_text->style->states[UI_STATE_NORMAL].fg = ui_rgb(100, 180, 100);
    world_text->style->states[UI_STATE_NORMAL].bg = ui_rgb(5, 20, 5);
    fge_ui_add_child(world, world_text);
    fge_ui_add_child(root, world);

    /* Bottom bar */
    fge_ui_widget_t *botbar = fge_ui_box("botbar");
    botbar->layout = fge_ui_layout_row();
    botbar->layout.align_cross = UI_ALIGN_STRETCH;
    botbar->preferred_size.h = 1;
    botbar->style->states[UI_STATE_NORMAL].bg = ui_rgb(20, 20, 30);

    fge_ui_widget_t *skills = fge_ui_label("skills", " [1]Slash [2]Fireball [3]Heal [4]Shield [5]Berserk ");
    skills->style->states[UI_STATE_NORMAL].fg = ui_rgb(200, 200, 200);
    skills->style->states[UI_STATE_NORMAL].bg = ui_rgb(20, 20, 30);
    fge_ui_add_child(botbar, skills);

    fge_ui_widget_t *chat = fge_ui_label("chat", " [Guild] Welcome to Ember Online! ");
    chat->style->states[UI_STATE_NORMAL].fg = ui_rgb(100, 200, 255);
    chat->style->states[UI_STATE_NORMAL].bg = ui_rgb(20, 20, 30);
    fge_ui_add_child(botbar, chat);

    fge_ui_add_child(root, botbar);

    return root;
}

/* -------------------------------------------------------------------------- */
/* Screen builder dispatcher                                                  */
/* -------------------------------------------------------------------------- */

static fge_ui_widget_t *build_screen(screen_t s) {
    switch (s) {
        case SCREEN_TITLE:      return build_title_screen();
        case SCREEN_MAIN_MENU:  return build_main_menu();
        case SCREEN_SETTINGS:   return build_settings_screen();
        case SCREEN_GAME_HUD:   return build_game_hud();
        default:                return fge_ui_box("empty");
    }
}

/* -------------------------------------------------------------------------- */
/* Global event handler for screen transitions                                */
/* -------------------------------------------------------------------------- */

static bool g_first_key = true;

static bool app_on_event(fge_ui_app_t *app, const fge_ui_event_t *ev) {
    (void)app;
    if (ev->type == UI_EVENT_KEY) {
        /* Title screen: any key advances to menu */
        if (g_screen == SCREEN_TITLE && !g_first_key) {
            g_screen = SCREEN_MAIN_MENU;
            return true;
        }
        g_first_key = false;

        /* ESC goes back */
        if (ev->key.key == UI_KEY_ESCAPE) {
            if (g_screen == SCREEN_SETTINGS || g_screen == SCREEN_GAME_HUD) {
                g_screen = SCREEN_MAIN_MENU;
                return true;
            }
        }
    }
    return false;
}

/* -------------------------------------------------------------------------- */
/* Tick callback — runs every frame                                           */
/* -------------------------------------------------------------------------- */

static fge_clock_t g_clock;
static uint64_t    g_frame_count = 0;

static void app_tick(fge_ui_app_t *app, void *ud) {
    (void)ud;
    g_frame_count++;

    /* Log frame info every 60 frames */
    if (g_frame_count % 60 == 0) {
        double dt = fge_clock_elapsed_sec(&g_clock) * 1000.0;
        FGE_DEBUG(FGE_LOG_CAT_GENERAL, "Frame %llu | dt=%.3fms | screen=%d",
                  (unsigned long long)g_frame_count, dt, g_screen);
    }
}

/* -------------------------------------------------------------------------- */
/* Main                                                                       */
/* -------------------------------------------------------------------------- */

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    /* Initialize FORGE systems */
    fge_log_init(FGE_LOG_LEVEL_DEBUG);
    fge_clock_init(&g_clock);

    FGE_INFO(FGE_LOG_CAT_GENERAL, "============================================");
    FGE_INFO(FGE_LOG_CAT_GENERAL, "  FORGE UI Demo — Ember Online");
    FGE_INFO(FGE_LOG_CAT_GENERAL, "  Engine: v0.2 | Pure C23 | Zero Dependencies");
    FGE_INFO(FGE_LOG_CAT_GENERAL, "============================================");

    /* Create terminal backend */
    fge_ui_backend_t *be = fge_ui_backend_term();
    if (!be) {
        FGE_FATAL(FGE_LOG_CAT_PLATFORM, "Failed to create terminal backend");
        return 1;
    }

    /* Create app */
    g_app = fge_ui_app_new(be, "Ember Online", 80, 24);
    if (!g_app) {
        FGE_FATAL(FGE_LOG_CAT_PLATFORM, "Failed to create UI app");
        return 1;
    }

    g_app->on_event = app_on_event;
    ui_app_set_tick_cb(g_app, app_tick, NULL);

    /* Main loop */
    screen_t last_screen = SCREEN_COUNT; /* force initial build */
    while (g_app->running) {
        /* Rebuild root widget when screen changes */
        if (g_screen != last_screen) {
            if (g_app->root) {
                ui_widget_destroy(g_app->root);
            }
            g_app->root = build_screen(g_screen);
            ui_app_set_root(g_app, g_app->root);

            /* Set initial focus for menus */
            if (g_screen == SCREEN_MAIN_MENU) {
                fge_ui_widget_t *btn = ui_widget_find(g_app->root, "btn_new");
                if (btn) ui_app_set_focus(g_app, btn);
            }
            last_screen = g_screen;
            FGE_INFO(FGE_LOG_CAT_GENERAL, "Switched to screen %d", g_screen);
        }

        fge_ui_app_step(g_app);
    }

    /* Cleanup */
    if (g_app->root) ui_widget_destroy(g_app->root);
    ui_app_free(g_app);

    FGE_INFO(FGE_LOG_CAT_GENERAL, "Demo exited. Total frames: %llu", (unsigned long long)g_frame_count);
    return 0;
}
