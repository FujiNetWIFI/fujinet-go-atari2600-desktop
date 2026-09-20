/*
 * The Preferences dialog: a programmatic AdwPreferencesDialog, no .ui file.
 *
 * Controller types and the analog switches apply LIVE -- a FujiNet-booted
 * game survives a change of controller, as it would on the real console.
 * The TV format and the host options are read when the session starts, so
 * those restart the session once, on close, if anything changed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "prefs.h"

#include "window.h"

typedef struct {
    A2600Window *window;
    a2600session *session;
    void (*restart)(A2600Window *window);
    gboolean dirty;
    GtkWidget *pad_group;
    GtkWidget *pad_rows[8];
    int pad_row_count;
    unsigned pad_generation;
    guint pad_timer;
    GtkWidget *port_row[2];
} PrefsState;

typedef struct {
    PrefsState *state;
    const char *key;
    int def;
    int live_port;   /* -1: a restart option; 0/1: a live port-type row */
} RowBinding;

static void binding_free(gpointer p, GClosure *closure)
{
    (void)closure;
    g_free(p);
}

static RowBinding *binding_new(PrefsState *state, const char *key, int def, int live_port)
{
    RowBinding *b = g_new0(RowBinding, 1);
    b->state = state;
    b->key = key;
    b->def = def;
    b->live_port = live_port;
    return b;
}

static void update_port_subtitle(PrefsState *state, int port)
{
    char sub[96];
    int type = a2600session_port_type(state->session, port);
    int det = a2600session_detected_port_type(state->session, port);
    if (type == A2600_CTRL_AUTO)
        g_snprintf(sub, sizeof sub, "Auto: Stella attached %s", a2600_ctrl_type_name(det));
    else
        g_snprintf(sub, sizeof sub, "%s", "Forced for every cartridge");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(state->port_row[port]), sub);
}

static void combo_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    RowBinding *b = user_data;
    int sel = (int)adw_combo_row_get_selected(ADW_COMBO_ROW(row));
    (void)pspec;
    if (b->live_port >= 0) {
        a2600session_set_port_type(b->state->session, b->live_port, sel);
        update_port_subtitle(b->state, b->live_port);
        return;
    }
    if (a2600session_get_int(b->state->session, b->key, b->def) == sel)
        return;
    a2600session_set_int(b->state->session, b->key, sel);
    b->state->dirty = TRUE;
}

static void analog_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    PrefsState *state = user_data;
    (void)row; (void)pspec;
    a2600session_set_analog(state->session,
        a2600session_get_int(state->session, "analog_joystick", 1),
        a2600session_get_int(state->session, "analog_paddle", 1),
        a2600session_get_int(state->session, "analog_driving", 1));
}

static void switch_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    RowBinding *b = user_data;
    int on = adw_switch_row_get_active(ADW_SWITCH_ROW(row)) ? 1 : 0;
    (void)pspec;
    if (a2600session_get_int(b->state->session, b->key, b->def) == on)
        return;
    a2600session_set_int(b->state->session, b->key, on);
    if (b->live_port == -2)          /* an analog switch: live */
        analog_changed(row, NULL, b->state);
    else
        b->state->dirty = TRUE;
}

static void volume_changed(GtkRange *range, gpointer user_data)
{
    PrefsState *state = user_data;
    a2600session_set_volume(state->session, (int)gtk_range_get_value(range));
}

static GtkWidget *combo_row(PrefsState *state, const char *title,
                            const char *subtitle, const char *key, int def,
                            const char *(*name_fn)(int), int live_port)
{
    GtkWidget *row = adw_combo_row_new();
    GtkStringList *model = gtk_string_list_new(NULL);
    int i;

    for (i = 0; name_fn(i); i++)
        gtk_string_list_append(model, name_fn(i));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    if (subtitle)
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));
    adw_combo_row_set_selected(ADW_COMBO_ROW(row),
        (guint)a2600session_get_int(state->session, key, def));
    g_signal_connect_data(row, "notify::selected", G_CALLBACK(combo_changed),
                          binding_new(state, key, def, live_port), binding_free, 0);
    g_object_unref(model);
    return row;
}

static GtkWidget *switch_row(PrefsState *state, const char *title,
                             const char *subtitle, const char *key, int def,
                             int live)
{
    GtkWidget *row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    if (subtitle)
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    adw_switch_row_set_active(ADW_SWITCH_ROW(row),
                              a2600session_get_int(state->session, key, def));
    g_signal_connect_data(row, "notify::active", G_CALLBACK(switch_changed),
                          binding_new(state, key, def, live ? -2 : -1), binding_free, 0);
    return row;
}

/* ---- gamepads: one row per pad, with its port assignment ------------------- */

static void pad_assign_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    PrefsState *state = user_data;
    int idx = GPOINTER_TO_INT(g_object_get_data(row, "pad-index"));
    int sel = (int)adw_combo_row_get_selected(ADW_COMBO_ROW(row));
    (void)pspec;
    a2600session_gamepad_assign(state->session, idx, sel - 1);
}

static void rebuild_pad_rows(PrefsState *state)
{
    static const char *const choices[] = { "Automatic", "Left port", "Right port", NULL };
    int i, n = a2600session_gamepad_count(state->session);
    char name[128], sub[64];

    for (i = 0; i < state->pad_row_count; i++)
        adw_preferences_group_remove(ADW_PREFERENCES_GROUP(state->pad_group), state->pad_rows[i]);
    state->pad_row_count = 0;

    if (n == 0) {
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "No gamepads connected");
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), "Plug one in: it is picked up as it appears");
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(state->pad_group), row);
        state->pad_rows[state->pad_row_count++] = row;
        return;
    }
    for (i = 0; i < n && i < 8; i++) {
        GtkWidget *row = adw_combo_row_new();
        GtkStringList *model = gtk_string_list_new(choices);
        int eff = a2600session_gamepad_effective_port(state->session, i);
        a2600session_gamepad_name(state->session, i, name, sizeof name);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), name);
        g_snprintf(sub, sizeof sub, "Driving the %s port",
                   eff == 0 ? "left" : eff == 1 ? "right" : "no");
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sub);
        adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));
        adw_combo_row_set_selected(ADW_COMBO_ROW(row),
            (guint)(a2600session_gamepad_assignment(state->session, i) + 1));
        g_object_set_data(G_OBJECT(row), "pad-index", GINT_TO_POINTER(i));
        g_signal_connect(row, "notify::selected", G_CALLBACK(pad_assign_changed), state);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(state->pad_group), row);
        state->pad_rows[state->pad_row_count++] = row;
        g_object_unref(model);
    }
}

static gboolean pad_tick(gpointer user_data)
{
    PrefsState *state = user_data;
    unsigned gen = a2600session_gamepad_generation(state->session);
    if (gen != state->pad_generation) {
        state->pad_generation = gen;
        rebuild_pad_rows(state);
    }
    update_port_subtitle(state, 0);
    update_port_subtitle(state, 1);
    return G_SOURCE_CONTINUE;
}

static void prefs_closed(AdwDialog *dialog, gpointer user_data)
{
    PrefsState *state = user_data;
    (void)dialog;
    if (state->pad_timer) g_source_remove(state->pad_timer);
    if (state->dirty && state->restart)
        state->restart(state->window);
    g_free(state);
}

void a2600_prefs_show(A2600Window *parent, a2600session *session,
                      void (*restart)(A2600Window *parent))
{
    PrefsState *state = g_new0(PrefsState, 1);
    AdwPreferencesDialog *dialog = ADW_PREFERENCES_DIALOG(adw_preferences_dialog_new());
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    AdwPreferencesGroup *machine = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *ports = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *analog = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *pads = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *audio = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *host = ADW_PREFERENCES_GROUP(adw_preferences_group_new());

    state->window = parent;
    state->session = session;
    state->restart = restart;
    state->pad_group = GTK_WIDGET(pads);

    adw_dialog_set_title(ADW_DIALOG(dialog), "Preferences");
    adw_dialog_set_content_width(ADW_DIALOG(dialog), 560);
    adw_preferences_page_set_title(page, "Machine");
    adw_preferences_page_set_icon_name(page, "applications-games-symbolic");

    adw_preferences_group_set_title(machine, "Machine");
    adw_preferences_group_set_description(machine,
        "Applied by restarting the session when this dialog closes");
    adw_preferences_group_add(machine,
        combo_row(state, "TV format", "Auto lets each cartridge decide (frame layout detection)",
                  "tv_format", A2600_TV_AUTO, a2600_tv_format_name, -1));

    adw_preferences_group_set_title(ports, "Controllers");
    adw_preferences_group_set_description(ports,
        "What is plugged into each port. Applies immediately, even to a game "
        "booted over FujiNet.");
    state->port_row[0] = combo_row(state, "Left port", "", "port0_type", A2600_CTRL_AUTO,
                                   a2600_ctrl_type_name, 0);
    state->port_row[1] = combo_row(state, "Right port", "", "port1_type", A2600_CTRL_AUTO,
                                   a2600_ctrl_type_name, 1);
    adw_preferences_group_add(ports, state->port_row[0]);
    adw_preferences_group_add(ports, state->port_row[1]);
    update_port_subtitle(state, 0);
    update_port_subtitle(state, 1);

    adw_preferences_group_set_title(analog, "Analog sticks drive");
    adw_preferences_group_set_description(analog,
        "Whether a gamepad's sticks move these controllers, or only its D-pad "
        "and shoulders do");
    adw_preferences_group_add(analog,
        switch_row(state, "Joystick", "Left stick as the four directions", "analog_joystick", 1, 1));
    adw_preferences_group_add(analog,
        switch_row(state, "Paddles", "Left stick X is paddle A, right stick X is paddle B", "analog_paddle", 1, 1));
    adw_preferences_group_add(analog,
        switch_row(state, "Driving controller", "Left stick X as the wheel", "analog_driving", 1, 1));

    adw_preferences_group_set_title(pads, "Gamepads");
    adw_preferences_group_set_description(pads,
        "Assigned to ports in connection order unless chosen here");
    rebuild_pad_rows(state);
    state->pad_generation = a2600session_gamepad_generation(session);

    adw_preferences_group_set_title(audio, "Audio");
    {
        GtkWidget *row = adw_action_row_new();
        GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 5);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Volume");
        gtk_range_set_value(GTK_RANGE(scale), a2600session_get_int(session, "volume", 100));
        gtk_widget_set_size_request(scale, 200, -1);
        gtk_widget_set_valign(scale, GTK_ALIGN_CENTER);
        g_signal_connect(scale, "value-changed", G_CALLBACK(volume_changed), state);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), scale);
        adw_preferences_group_add(audio, row);
    }

    adw_preferences_group_set_title(host, "Host");
    adw_preferences_group_set_description(host, "Applied by restarting the session");
    adw_preferences_group_add(host,
        switch_row(state, "Enable FujiNet",
                   "Run the in-process FujiNet the cartridge dials into. Off "
                   "means no network and a link-down CONFIG client.",
                   "enable_fujinet", 1, 0));
    adw_preferences_group_add(host,
        switch_row(state, "Audio", "Open the system audio device", "enable_audio", 1, 0));
    adw_preferences_group_add(host,
        switch_row(state, "Gamepads", "Poll USB/Bluetooth gamepads", "enable_gamepad", 1, 0));

    adw_preferences_page_add(page, machine);
    adw_preferences_page_add(page, ports);
    adw_preferences_page_add(page, analog);
    adw_preferences_page_add(page, pads);
    adw_preferences_page_add(page, audio);
    adw_preferences_page_add(page, host);
    adw_preferences_dialog_add(dialog, page);
    g_signal_connect(dialog, "closed", G_CALLBACK(prefs_closed), state);
    state->pad_timer = g_timeout_add(1000, pad_tick, state);
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(parent));
}
