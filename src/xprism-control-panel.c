/* SPDX-License-Identifier: MIT
 *
 * xprism-control-panel: XPrism's settings in a window (GTK 4).
 *
 * Four pages, each a thin view over libxpctl:
 *   Displays     resolution, refresh rate, position, rotation, primary,
 *                on/off; HDR, EDR and wide gamut per output
 *   Namespaces   create, delete, and choose what each may do
 *   Apps         connected applications (refreshed every two seconds)
 *   Prompts      answer confined applications' permission requests
 */
#include <gtk/gtk.h>
#include <glib-unix.h>
#include <unistd.h>

#include "xpctl.h"

static xpctl *x;
static GtkWidget *window, *banner;
static GtkWidget *displays_box, *ns_list, *ns_checks[XPCTL_NUM_CAPS], *ns_title, *ns_delete,
                 *apps_box, *agent_switch, *agent_label;
static char ns_selected[256];
static gboolean updating;       /* refreshing widgets: ignore their signals */
static guint agent_watch;

/* ---- helpers ------------------------------------------------------- */

static void
show_error(const char *what)
{
    gtk_label_set_text(GTK_LABEL(banner), what);
    gtk_widget_set_visible(banner, TRUE);
}

static void
clear_error(void)
{
    gtk_widget_set_visible(banner, FALSE);
}

static void
clear_box(GtkWidget *box)
{
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(box)))
        gtk_box_remove(GTK_BOX(box), child);
}

static GtkWidget *
label(const char *text, const char *css)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    if (css)
        gtk_widget_add_css_class(l, css);
    return l;
}

static GtkWidget *
page(const char *title, const char *intro)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(box, 24);
    gtk_widget_set_margin_end(box, 24);
    gtk_widget_set_margin_top(box, 18);
    gtk_widget_set_margin_bottom(box, 18);
    gtk_box_append(GTK_BOX(box), label(title, "title-2"));
    if (intro)
        gtk_box_append(GTK_BOX(box), label(intro, "dim-label"));
    return box;
}

static GtkWidget *
scrolled(GtkWidget *child)
{
    GtkWidget *s = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(s), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(s), child);
    gtk_widget_set_vexpand(s, TRUE);
    return s;
}

static char *
grants_text(uint32_t g, bool admin)
{
    if (admin)
        return g_strdup("everything");
    if (!g)
        return g_strdup("nothing beyond its own windows");
    GString *s = g_string_new(NULL);
    for (int i = 0; i < XPCTL_NUM_CAPS; i++)
        if (g & (1u << i))
            g_string_append_printf(s, "%s%s", s->len ? ", " : "", xpctl_cap_name(i));
    return g_string_free(s, FALSE);
}

/* ---- Displays ------------------------------------------------------ */

static void refresh_displays(void);

static gboolean
on_setting(GtkSwitch *sw, gboolean on, gpointer data)
{
    if (updating)
        return FALSE;
    enum xpctl_setting s = GPOINTER_TO_INT(data);
    const char *output = g_object_get_data(G_OBJECT(sw), "output");
    if (xpctl_output_set(x, output, s, on) < 0) {
        show_error(xpctl_error(x));
        refresh_displays();
        return TRUE;            /* keep the switch where it was */
    }
    clear_error();
    gtk_switch_set_state(sw, on);
    refresh_displays();
    return TRUE;
}

static GtkWidget *
setting_row(const xpctl_output *o, enum xpctl_setting s, const char *title, const char *what,
            int value)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(text, TRUE);
    gtk_box_append(GTK_BOX(text), label(title, NULL));
    gtk_box_append(GTK_BOX(text), label(value == XPCTL_UNAVAILABLE
                                        ? "Not available on this output" : what,
                                        "dim-label"));
    gtk_box_append(GTK_BOX(row), text);
    GtkWidget *sw = gtk_switch_new();
    gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
    gtk_switch_set_active(GTK_SWITCH(sw), value == 1);
    gtk_widget_set_sensitive(sw, value != XPCTL_UNAVAILABLE);
    g_object_set_data_full(G_OBJECT(sw), "output", g_strdup(o->name), g_free);
    g_signal_connect(sw, "state-set", G_CALLBACK(on_setting), GINT_TO_POINTER(s));
    gtk_box_append(GTK_BOX(row), sw);
    return row;
}


/* Layout changes are applied at once and confirmed like on other systems:
   a bar asks to keep them, and without an answer in 15 seconds the old
   layout comes back (a refresh rate the monitor cannot show leaves it
   black). Keep also saves the layout for the next login. */
#define CONFIRM_SECONDS 15
static GtkWidget *confirm_bar, *confirm_label;
static xpctl_layout *undo_layout;
static int undo_n;
static guint confirm_timer;
static int confirm_left;

static void
confirm_done(void)
{
    if (confirm_timer)
        g_source_remove(confirm_timer);
    confirm_timer = 0;
    free(undo_layout);
    undo_layout = NULL;
    undo_n = 0;
    gtk_widget_set_visible(confirm_bar, FALSE);
}

static void
on_revert(GtkButton *b, gpointer data)
{
    (void) b; (void) data;
    if (undo_layout && xpctl_layout_apply(x, undo_layout, undo_n) < 0)
        show_error(xpctl_error(x));
    confirm_done();
    refresh_displays();
}

static void
on_keep(GtkButton *b, gpointer data)
{
    (void) b; (void) data;
    confirm_done();
    xpctl_layout *l;
    int n;
    if (xpctl_layout_get(x, &l, &n) == 0) {
        if (xpctl_layout_save(x, NULL, l, n) < 0)
            show_error(xpctl_error(x));
        free(l);
    }
}

static void
confirm_text(void)
{
    char *t = g_strdup_printf("Keep these display settings? Going back in %d s.", confirm_left);
    gtk_label_set_text(GTK_LABEL(confirm_label), t);
    g_free(t);
}

static gboolean
on_confirm_tick(gpointer data)
{
    (void) data;
    if (--confirm_left > 0) {
        confirm_text();
        return G_SOURCE_CONTINUE;
    }
    confirm_timer = 0;
    on_revert(NULL, NULL);
    return G_SOURCE_REMOVE;
}

/* rebuilding the page destroys the widget whose signal is running: later */
static gboolean
refresh_displays_idle(gpointer data)
{
    (void) data;
    refresh_displays();
    return G_SOURCE_REMOVE;
}

/* changes one output: takes the layout now, lets edit() change the
   output's entry, applies it, and asks to keep it */
static void
change_output(const char *name, void (*edit)(xpctl_layout *l, gpointer arg), gpointer arg)
{
    xpctl_layout *l;
    int n;
    if (xpctl_layout_get(x, &l, &n) < 0) {
        show_error(xpctl_error(x));
        return;
    }
    xpctl_layout *mine = NULL;
    for (int i = 0; i < n; i++)
        if (!strcmp(l[i].name, name))
            mine = &l[i];
    if (!mine) {
        free(l);
        g_idle_add(refresh_displays_idle, NULL);
        return;
    }
    xpctl_layout *before = g_memdup2(l, n * sizeof(*l));
    edit(mine, arg);
    if (xpctl_layout_apply(x, mine, 1) < 0) {
        show_error(xpctl_error(x));
        g_free(before);
    } else {
        clear_error();
        if (!undo_layout) {     /* several changes: back to before the first */
            undo_layout = malloc(n * sizeof(*l));
            if (undo_layout) {
                memcpy(undo_layout, before, n * sizeof(*l));
                undo_n = n;
            }
        }
        g_free(before);
        confirm_left = CONFIRM_SECONDS;
        confirm_text();
        gtk_widget_set_visible(confirm_bar, TRUE);
        if (!confirm_timer)
            confirm_timer = g_timeout_add_seconds(1, on_confirm_tick, NULL);
    }
    free(l);
    g_idle_add(refresh_displays_idle, NULL);
}

typedef struct {
    xpctl_mode *modes;
    int n;
    int sizes[64][2];           /* the resolution dropdown's entries */
    int nsizes;
    double rates[64];           /* the refresh dropdown's entries */
    int nrates;
} output_modes;

static void
output_modes_free(gpointer p)
{
    output_modes *m = p;
    free(m->modes);
    g_free(m);
}

static void
edit_size(xpctl_layout *l, gpointer arg)
{
    int *wh = arg;
    l->enabled = true;
    l->width = wh[0];
    l->height = wh[1];
    l->refresh = 0;             /* the fastest at that size */
}

static void
on_size(GObject *dd, GParamSpec *ps, gpointer data)
{
    (void) ps; (void) data;
    if (updating)
        return;
    output_modes *m = g_object_get_data(dd, "modes");
    guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    if (i >= (guint) m->nsizes)
        return;
    change_output(g_object_get_data(dd, "output"), edit_size, m->sizes[i]);
}

static void
edit_rate(xpctl_layout *l, gpointer arg)
{
    l->refresh = *(double *) arg;
}

static void
on_rate(GObject *dd, GParamSpec *ps, gpointer data)
{
    (void) ps; (void) data;
    if (updating)
        return;
    output_modes *m = g_object_get_data(dd, "modes");
    guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    if (i >= (guint) m->nrates)
        return;
    change_output(g_object_get_data(dd, "output"), edit_rate, &m->rates[i]);
}

static const int rotations[] = { 0, 90, 180, 270 };

static void
edit_rotation(xpctl_layout *l, gpointer arg)
{
    l->rotation = GPOINTER_TO_INT(arg);
}

static void
on_rotation(GObject *dd, GParamSpec *ps, gpointer data)
{
    (void) ps; (void) data;
    if (updating)
        return;
    guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    if (i < G_N_ELEMENTS(rotations))
        change_output(g_object_get_data(dd, "output"), edit_rotation,
                      GINT_TO_POINTER(rotations[i]));
}

static void
edit_primary(xpctl_layout *l, gpointer arg)
{
    (void) arg;
    l->primary = true;
}

static void
on_primary(GtkCheckButton *c, gpointer data)
{
    (void) data;
    if (updating || !gtk_check_button_get_active(c))
        return;
    change_output(g_object_get_data(G_OBJECT(c), "output"), edit_primary, NULL);
}

static void
edit_position(xpctl_layout *l, gpointer arg)
{
    int *xy = arg;
    l->x = xy[0];
    l->y = xy[1];
}

static void
on_move(GtkButton *b, gpointer data)
{
    (void) data;
    GtkSpinButton *sx = g_object_get_data(G_OBJECT(b), "x"),
                  *sy = g_object_get_data(G_OBJECT(b), "y");
    int xy[2] = { gtk_spin_button_get_value_as_int(sx), gtk_spin_button_get_value_as_int(sy) };
    change_output(g_object_get_data(G_OBJECT(b), "output"), edit_position, xy);
}

static void
edit_enabled(xpctl_layout *l, gpointer arg)
{
    xpctl_layout *want = arg;
    *l = *want;
}

static gboolean
on_enabled(GtkSwitch *sw, gboolean on, gpointer data)
{
    (void) data;
    if (updating)
        return FALSE;
    const char *name = g_object_get_data(G_OBJECT(sw), "output");
    xpctl_layout want = { 0 };
    g_strlcpy(want.name, name, sizeof(want.name));
    if (on) {
        /* its preferred mode, right of the others */
        output_modes *m = g_object_get_data(G_OBJECT(sw), "modes");
        if (!m || !m->n)
            return TRUE;
        int best = 0;
        for (int i = 0; i < m->n; i++)
            if (m->modes[i].preferred) {
                best = i;
                break;
            }
        want.enabled = true;
        want.width = m->modes[best].width;
        want.height = m->modes[best].height;
        want.refresh = m->modes[best].refresh;
        xpctl_layout *l;
        int n;
        if (xpctl_layout_get(x, &l, &n) == 0) {
            for (int i = 0; i < n; i++)
                if (l[i].enabled) {
                    int w = (l[i].rotation % 180) ? l[i].height : l[i].width;
                    if (l[i].x + w > want.x)
                        want.x = l[i].x + w;
                }
            free(l);
        }
    }
    change_output(name, edit_enabled, &want);
    return TRUE;                /* refresh_displays sets the switch */
}

static GtkWidget *
row(const char *title, GtkWidget *control)
{
    GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *t = label(title, NULL);
    gtk_widget_set_hexpand(t, TRUE);
    gtk_box_append(GTK_BOX(r), t);
    gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(r), control);
    return r;
}

static GtkWidget *
named(GtkWidget *w, const char *output, output_modes *m)
{
    g_object_set_data_full(G_OBJECT(w), "output", g_strdup(output), g_free);
    if (m)
        g_object_set_data(G_OBJECT(w), "modes", m);
    return w;
}

/* the layout rows of one output, into box */
static void
layout_rows(GtkWidget *box, GtkWidget *frame, const xpctl_layout *l)
{
    output_modes *m = g_new0(output_modes, 1);
    xpctl_output_modes(x, l->name, &m->modes, &m->n);
    /* the frame owns it: every widget below dies with the frame */
    g_object_set_data_full(G_OBJECT(frame), "modes", m, output_modes_free);

    GtkStringList *sizes = gtk_string_list_new(NULL);
    guint size_sel = GTK_INVALID_LIST_POSITION;
    for (int i = 0; i < m->n && m->nsizes < 64; i++) {
        bool seen = false;
        for (int j = 0; j < m->nsizes; j++)
            if (m->sizes[j][0] == m->modes[i].width && m->sizes[j][1] == m->modes[i].height)
                seen = true;
        if (seen)
            continue;
        bool pref = false;
        for (int j = 0; j < m->n; j++)
            if (m->modes[j].width == m->modes[i].width &&
                m->modes[j].height == m->modes[i].height && m->modes[j].preferred)
                pref = true;
        char *t = g_strdup_printf("%d × %d%s", m->modes[i].width, m->modes[i].height,
                                  pref ? " (recommended)" : "");
        gtk_string_list_append(sizes, t);
        g_free(t);
        if (l->enabled && m->modes[i].width == l->width && m->modes[i].height == l->height)
            size_sel = m->nsizes;
        m->sizes[m->nsizes][0] = m->modes[i].width;
        m->sizes[m->nsizes][1] = m->modes[i].height;
        m->nsizes++;
    }
    GtkWidget *size = named(gtk_drop_down_new(G_LIST_MODEL(sizes), NULL), l->name, m);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(size), size_sel);
    g_signal_connect(size, "notify::selected", G_CALLBACK(on_size), NULL);
    gtk_widget_set_sensitive(size, l->enabled);
    gtk_box_append(GTK_BOX(box), row("Resolution", size));

    GtkStringList *rates = gtk_string_list_new(NULL);
    guint rate_sel = GTK_INVALID_LIST_POSITION;
    double best = 1e9;
    for (int i = 0; i < m->n && m->nrates < 64; i++) {
        if (m->modes[i].width != l->width || m->modes[i].height != l->height)
            continue;
        char *t = m->modes[i].refresh >= 1 ? g_strdup_printf("%.2f Hz", m->modes[i].refresh)
                                           : g_strdup("not reported");
        gtk_string_list_append(rates, t);
        g_free(t);
        double d = m->modes[i].refresh - l->refresh;
        if (d < 0)
            d = -d;
        if (d < best) {
            best = d;
            rate_sel = m->nrates;
        }
        m->rates[m->nrates++] = m->modes[i].refresh;
    }
    GtkWidget *rate = named(gtk_drop_down_new(G_LIST_MODEL(rates), NULL), l->name, m);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(rate), rate_sel);
    g_signal_connect(rate, "notify::selected", G_CALLBACK(on_rate), NULL);
    gtk_widget_set_sensitive(rate, l->enabled);
    gtk_box_append(GTK_BOX(box), row("Refresh rate", rate));

    const char *rot_names[] = { "Normal", "Left", "Upside down", "Right", NULL };
    GtkWidget *rot = named(gtk_drop_down_new_from_strings(rot_names), l->name, NULL);
    for (guint i = 0; i < G_N_ELEMENTS(rotations); i++)
        if (rotations[i] == l->rotation)
            gtk_drop_down_set_selected(GTK_DROP_DOWN(rot), i);
    g_signal_connect(rot, "notify::selected", G_CALLBACK(on_rotation), NULL);
    gtk_widget_set_sensitive(rot, l->enabled);
    gtk_box_append(GTK_BOX(box), row("Rotation", rot));

    GtkWidget *pos = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *sx = gtk_spin_button_new_with_range(0, 32767, 1);
    GtkWidget *sy = gtk_spin_button_new_with_range(0, 32767, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(sx), l->x);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(sy), l->y);
    GtkWidget *move = named(gtk_button_new_with_label("Move"), l->name, NULL);
    g_object_set_data(G_OBJECT(move), "x", sx);
    g_object_set_data(G_OBJECT(move), "y", sy);
    g_signal_connect(move, "clicked", G_CALLBACK(on_move), NULL);
    gtk_box_append(GTK_BOX(pos), label("x", "dim-label"));
    gtk_box_append(GTK_BOX(pos), sx);
    gtk_box_append(GTK_BOX(pos), label("y", "dim-label"));
    gtk_box_append(GTK_BOX(pos), sy);
    gtk_box_append(GTK_BOX(pos), move);
    gtk_widget_set_sensitive(pos, l->enabled);
    gtk_box_append(GTK_BOX(box), row("Position", pos));

    GtkWidget *prim = named(gtk_check_button_new(), l->name, NULL);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(prim), l->primary);
    gtk_widget_set_sensitive(prim, l->enabled && !l->primary);
    g_signal_connect(prim, "toggled", G_CALLBACK(on_primary), NULL);
    gtk_box_append(GTK_BOX(box), row("Main display (panels and new windows)", prim));
}
static void
refresh_displays(void)
{
    updating = TRUE;
    clear_box(displays_box);
    xpctl_output *o;
    int n;
    if (xpctl_outputs(x, &o, &n) < 0) {
        gtk_box_append(GTK_BOX(displays_box), label(xpctl_error(x), NULL));
        updating = FALSE;
        return;
    }
    xpctl_layout *lay = NULL;
    int nlay = 0;
    xpctl_layout_get(x, &lay, &nlay);
    for (int i = 0; i < n; i++) {
        if (!o[i].connected)
            continue;
        GtkWidget *frame = gtk_frame_new(NULL);
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
        gtk_widget_set_margin_start(box, 14);
        gtk_widget_set_margin_end(box, 14);
        gtk_widget_set_margin_top(box, 12);
        gtk_widget_set_margin_bottom(box, 12);
        char *head = !o[i].enabled ? g_strdup_printf("%s — off", o[i].name)
            : o[i].refresh >= 1 ? g_strdup_printf("%s — %d×%d, %.0f Hz", o[i].name, o[i].width,
                                                  o[i].height, o[i].refresh)
            : g_strdup_printf("%s — %d×%d", o[i].name, o[i].width, o[i].height);
        GtkWidget *headrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        GtkWidget *headlabel = label(head, "heading");
        gtk_widget_set_hexpand(headlabel, TRUE);
        gtk_box_append(GTK_BOX(headrow), headlabel);
        g_free(head);
        const xpctl_layout *l = NULL;
        for (int j = 0; j < nlay; j++)
            if (!strcmp(lay[j].name, o[i].name))
                l = &lay[j];
        GtkWidget *on = NULL;
        if (l) {
            on = named(gtk_switch_new(), o[i].name, NULL);
            gtk_switch_set_active(GTK_SWITCH(on), l->enabled);
            gtk_widget_set_valign(on, GTK_ALIGN_CENTER);
            gtk_box_append(GTK_BOX(headrow), on);
        }
        gtk_box_append(GTK_BOX(box), headrow);
        if (l) {
            layout_rows(box, frame, l);
            g_object_set_data(G_OBJECT(on), "modes", g_object_get_data(G_OBJECT(frame), "modes"));
            g_signal_connect(on, "state-set", G_CALLBACK(on_enabled), NULL);
        }
        if (o[i].color_known) {
            char *shows;
            if (!strcmp(o[i].transfer, "PQ") || !strcmp(o[i].transfer, "HLG"))
                shows = g_strdup_printf("Shows %s %s, up to %u cd/m²", o[i].primaries,
                                        o[i].transfer, o[i].max_lum);
            else if (o[i].max_lum > o[i].ref_white && o[i].ref_white)
                shows = g_strdup_printf("Shows %s with EDR: highlights up to %.1f× white",
                                        o[i].primaries,
                                        (double) o[i].max_lum / o[i].ref_white);
            else
                shows = g_strdup_printf("Shows %s", o[i].primaries);
            gtk_box_append(GTK_BOX(box), label(shows, "dim-label"));
            g_free(shows);
        }
        gtk_box_append(GTK_BOX(box), setting_row(&o[i], XPCTL_HDR, "HDR",
            "Send HDR (BT.2020 PQ) to the monitor", o[i].hdr));
        gtk_box_append(GTK_BOX(box), setting_row(&o[i], XPCTL_EDR, "Extended dynamic range",
            "Raise the backlight for HDR content, keep everything else as bright as now",
            o[i].edr));
        gtk_box_append(GTK_BOX(box), setting_row(&o[i], XPCTL_WIDE_GAMUT, "Wide gamut",
            "Use the monitor's full colour range", o[i].wide_gamut));
        gtk_frame_set_child(GTK_FRAME(frame), box);
        gtk_box_append(GTK_BOX(displays_box), frame);
    }
    free(lay);
    free(o);
    updating = FALSE;
}

/* ---- Namespaces ---------------------------------------------------- */

static void refresh_namespaces(void);

static void
select_namespace(const xpctl_namespace *ns)
{
    updating = TRUE;
    snprintf(ns_selected, sizeof(ns_selected), "%s", ns ? ns->name : "");
    bool editable = ns && !ns->builtin && !ns->admin;
    char *title = !ns ? g_strdup("Choose a namespace")
                : ns->admin ? g_strdup_printf("%s may do everything (built in)", ns->name)
                : ns->builtin ? g_strdup_printf("%s: built in, fixed", ns->name)
                : g_strdup_printf("Applications in %s may also:", ns->name);
    gtk_label_set_text(GTK_LABEL(ns_title), title);
    g_free(title);
    for (int i = 0; i < XPCTL_NUM_CAPS; i++) {
        gtk_check_button_set_active(GTK_CHECK_BUTTON(ns_checks[i]),
                                    ns && (ns->admin || (ns->grants & (1u << i))));
        gtk_widget_set_sensitive(ns_checks[i], editable);
    }
    gtk_widget_set_sensitive(ns_delete, editable);
    updating = FALSE;
}

static void
on_ns_row(GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
    (void) list;
    (void) data;
    if (updating || !row)
        return;
    xpctl_namespace *all;
    int n;
    if (xpctl_namespaces(x, &all, &n) < 0)
        return;
    const char *name = g_object_get_data(G_OBJECT(row), "name");
    for (int i = 0; i < n; i++)
        if (!strcmp(all[i].name, name))
            select_namespace(&all[i]);
    free(all);
}

static void
on_grant(GtkCheckButton *check, gpointer data)
{
    (void) data;
    if (updating || !ns_selected[0])
        return;
    uint32_t g = 0;
    for (int i = 0; i < XPCTL_NUM_CAPS; i++)
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(ns_checks[i])))
            g |= 1u << i;
    (void) check;
    if (xpctl_namespace_set_grants(x, ns_selected, g) < 0)
        show_error(xpctl_error(x));
    else
        clear_error();
    refresh_namespaces();
}

static void
on_create(GtkEntry *entry, gpointer data)
{
    (void) data;
    const char *name = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (!*name)
        return;
    if (xpctl_namespace_create(x, name, 0, false) < 0) {
        show_error(xpctl_error(x));
        return;
    }
    clear_error();
    snprintf(ns_selected, sizeof(ns_selected), "%s", name);
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
    refresh_namespaces();
}

static void
on_create_button(GtkButton *b, gpointer entry)
{
    (void) b;
    on_create(GTK_ENTRY(entry), NULL);
}

static void
on_delete(GtkButton *b, gpointer data)
{
    (void) b;
    (void) data;
    if (!ns_selected[0])
        return;
    /* its apps are closed: the button says so */
    if (xpctl_namespace_delete(x, ns_selected, true) < 0) {
        show_error(xpctl_error(x));
        return;
    }
    clear_error();
    ns_selected[0] = '\0';
    refresh_namespaces();
}

static void
refresh_namespaces(void)
{
    updating = TRUE;
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(ns_list)))
        gtk_list_box_remove(GTK_LIST_BOX(ns_list), child);
    xpctl_namespace *all;
    int n;
    if (xpctl_namespaces(x, &all, &n) < 0) {
        updating = FALSE;
        show_error(xpctl_error(x));
        select_namespace(NULL);
        return;
    }
    const xpctl_namespace *sel = NULL;
    for (int i = 0; i < n; i++) {
        char *sub = g_strdup_printf("%u app%s%s", all[i].clients, all[i].clients == 1 ? "" : "s",
                                    all[i].builtin ? " · built in" :
                                    all[i].transient ? " · transient" : "");
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_margin_start(box, 8);
        gtk_widget_set_margin_end(box, 8);
        gtk_widget_set_margin_top(box, 6);
        gtk_widget_set_margin_bottom(box, 6);
        gtk_box_append(GTK_BOX(box), label(all[i].name, NULL));
        gtk_box_append(GTK_BOX(box), label(sub, "dim-label"));
        g_free(sub);
        GtkWidget *row = gtk_list_box_row_new();
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
        g_object_set_data_full(G_OBJECT(row), "name", g_strdup(all[i].name), g_free);
        gtk_list_box_append(GTK_LIST_BOX(ns_list), row);
        if (!strcmp(all[i].name, ns_selected)) {
            gtk_list_box_select_row(GTK_LIST_BOX(ns_list), GTK_LIST_BOX_ROW(row));
            sel = &all[i];
        }
    }
    updating = FALSE;
    select_namespace(sel);
    free(all);
}

static GtkWidget *
namespaces_page(void)
{
    GtkWidget *p = page("Namespaces",
        "Applications started in a namespace see only their own windows. Here you choose "
        "what else each namespace may do. Changes last until the server restarts; "
        "for good, put them in the namespace configuration file.");
    GtkWidget *cols = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    gtk_widget_set_vexpand(cols, TRUE);

    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_size_request(left, 220, -1);
    ns_list = gtk_list_box_new();
    gtk_widget_add_css_class(ns_list, "boxed-list");
    g_signal_connect(ns_list, "row-selected", G_CALLBACK(on_ns_row), NULL);
    gtk_box_append(GTK_BOX(left), scrolled(ns_list));
    GtkWidget *add = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "New namespace");
    gtk_widget_set_hexpand(entry, TRUE);
    g_signal_connect(entry, "activate", G_CALLBACK(on_create), NULL);
    GtkWidget *plus = gtk_button_new_from_icon_name("list-add-symbolic");
    gtk_widget_set_tooltip_text(plus, "Create");
    g_signal_connect(plus, "clicked", G_CALLBACK(on_create_button), entry);
    gtk_box_append(GTK_BOX(add), entry);
    gtk_box_append(GTK_BOX(add), plus);
    gtk_box_append(GTK_BOX(left), add);
    gtk_box_append(GTK_BOX(cols), left);

    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_hexpand(right, TRUE);
    ns_title = label("", "heading");
    gtk_box_append(GTK_BOX(right), ns_title);
    for (int i = 0; i < XPCTL_NUM_CAPS; i++) {
        char *text = g_strdup_printf("%c%s", g_ascii_toupper(xpctl_cap_what(i)[0]),
                                     xpctl_cap_what(i) + 1);
        ns_checks[i] = gtk_check_button_new_with_label(text);
        gtk_widget_set_tooltip_text(ns_checks[i], xpctl_cap_name(i));
        g_free(text);
        g_signal_connect(ns_checks[i], "toggled", G_CALLBACK(on_grant), NULL);
        gtk_box_append(GTK_BOX(right), ns_checks[i]);
    }
    ns_delete = gtk_button_new_with_label("Delete namespace and close its apps");
    gtk_widget_add_css_class(ns_delete, "destructive-action");
    gtk_widget_set_halign(ns_delete, GTK_ALIGN_START);
    gtk_widget_set_margin_top(ns_delete, 12);
    g_signal_connect(ns_delete, "clicked", G_CALLBACK(on_delete), NULL);
    gtk_box_append(GTK_BOX(right), ns_delete);
    gtk_box_append(GTK_BOX(cols), right);
    gtk_box_append(GTK_BOX(p), cols);
    return p;
}

/* ---- Apps ---------------------------------------------------------- */

static void
refresh_apps(void)
{
    clear_box(apps_box);
    xpctl_client *c;
    int n;
    if (xpctl_clients(x, &c, &n) < 0) {
        gtk_box_append(GTK_BOX(apps_box), label(xpctl_error(x), NULL));
        return;
    }
    int shown = 0;
    for (int i = 0; i < n; i++) {
        if (c[i].pid == getpid())
            continue;           /* this window's own connection */
        shown++;
        const char *name = c[i].app_id[0] ? c[i].app_id
                         : c[i].command[0] ? c[i].command : "Unknown application";
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_box_append(GTK_BOX(box), label(name, "heading"));
        bool session = !strcmp(c[i].ns, "root");
        char *g = grants_text(c[i].grants, session);
        char *pid = c[i].pid > 0 ? g_strdup_printf(" · pid %d", c[i].pid) : g_strdup("");
        char *sub = g_strdup_printf("%s%s%s · may do %s",
                                    session ? "Session (not confined)" : "Namespace ",
                                    session ? "" : c[i].ns, pid, g);
        gtk_box_append(GTK_BOX(box), label(sub, "dim-label"));
        g_free(g);
        g_free(pid);
        g_free(sub);
        gtk_widget_set_margin_bottom(box, 8);
        gtk_box_append(GTK_BOX(apps_box), box);
    }
    if (!shown)
        gtk_box_append(GTK_BOX(apps_box), label("No other applications are connected.", NULL));
    free(c);
}

/* ---- Prompts ------------------------------------------------------- */

static void
on_answer(GObject *src, GAsyncResult *res, gpointer data)
{
    int button = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, NULL);
    /* buttons: Deny, Allow once, Allow while it runs, Deny forever */
    static const enum xpctl_answer answers[] = {
        XPCTL_DENY_ONCE, XPCTL_ALLOW_ONCE, XPCTL_ALLOW_ALWAYS, XPCTL_DENY_ALWAYS };
    enum xpctl_answer a = button >= 0 && button < 4 ? answers[button] : XPCTL_DENY_ONCE;
    if (xpctl_agent_answer(x, GPOINTER_TO_UINT(data), a) < 0)
        show_error(xpctl_error(x));
}

static void
ask(const xpctl_prompt *p)
{
    const char *who = p->client.app_id[0] ? p->client.app_id
                    : p->client.command[0] ? p->client.command : "An application";
    GtkAlertDialog *d = gtk_alert_dialog_new("%s wants to %s", who, xpctl_cap_what(p->cap));
    char *detail = p->client.pid > 0
        ? g_strdup_printf("Namespace %s, process %d. Permission: %s.", p->client.ns,
                          p->client.pid, xpctl_cap_name(p->cap))
        : g_strdup_printf("Namespace %s. Permission: %s.", p->client.ns,
                          xpctl_cap_name(p->cap));
    gtk_alert_dialog_set_detail(d, detail);
    g_free(detail);
    const char *buttons[] = { "Deny", "Allow once", "Allow while it runs", "Deny forever",
                              NULL };
    gtk_alert_dialog_set_buttons(d, buttons);
    gtk_alert_dialog_set_cancel_button(d, 0);
    gtk_alert_dialog_set_default_button(d, 0);
    gtk_window_present(GTK_WINDOW(window));
    gtk_alert_dialog_choose(d, GTK_WINDOW(window), NULL, on_answer, GUINT_TO_POINTER(p->id));
    g_object_unref(d);
}

static gboolean
on_prompts(gpointer data)
{
    (void) data;
    xpctl_prompt p;
    int r;
    while ((r = xpctl_agent_next(x, &p, 0)) == 1)
        ask(&p);
    if (r < 0) {
        show_error(xpctl_error(x));
        agent_watch = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean
on_prompts_queued(gpointer data)
{
    if (agent_watch)
        on_prompts(data);
    return G_SOURCE_CONTINUE;
}

static gboolean
on_prompts_fd(int fd, GIOCondition cond, gpointer data)
{
    (void) fd;
    (void) cond;
    return on_prompts(data);
}

static gboolean
on_agent(GtkSwitch *sw, gboolean on, gpointer data)
{
    (void) data;
    if (xpctl_agent(x, on) < 0) {
        show_error(xpctl_error(x));
        return TRUE;
    }
    clear_error();
    gtk_switch_set_state(sw, on);
    if (on && !agent_watch)
        agent_watch = g_unix_fd_add(xpctl_fd(x), G_IO_IN, on_prompts_fd, NULL);
    else if (!on && agent_watch) {
        g_source_remove(agent_watch);
        agent_watch = 0;
    }
    gtk_label_set_text(GTK_LABEL(agent_label), on
        ? "This window asks you whenever a confined application needs a permission. "
          "Keep it open."
        : "Off: confined applications get only what their namespace allows.");
    return TRUE;
}

static GtkWidget *
prompts_page(void)
{
    GtkWidget *p = page("Permission prompts",
        "With the server in capability mode \"enforce\", a confined application that "
        "needs something its namespace does not allow can ask you. One program at a time "
        "answers these prompts.");
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *l = label("Ask me here", "heading");
    gtk_widget_set_hexpand(l, TRUE);
    gtk_box_append(GTK_BOX(row), l);
    agent_switch = gtk_switch_new();
    g_signal_connect(agent_switch, "state-set", G_CALLBACK(on_agent), NULL);
    gtk_box_append(GTK_BOX(row), agent_switch);
    gtk_box_append(GTK_BOX(p), row);
    agent_label = label("Off: confined applications get only what their namespace allows.",
                        "dim-label");
    gtk_box_append(GTK_BOX(p), agent_label);
    return p;
}

/* ---- Overview and window ------------------------------------------- */

static GtkWidget *
overview_page(void)
{
    xpctl_server s;
    xpctl_server_info(x, &s);
    GtkWidget *p = page("XPrism", NULL);
    char *ver = g_strdup_printf("%s %s", s.vendor, s.version);
    gtk_box_append(GTK_BOX(p), label(ver, "heading"));
    g_free(ver);
    const struct { bool on; const char *yes, *no; } facts[] = {
        { s.compositor, "Built-in compositor: on", "Built-in compositor: off" },
        { s.color, "Colour management: on", "Colour management: off" },
        { s.namespaces, "Namespaces: on — applications can be confined",
          "Namespaces: not available here" },
    };
    for (size_t i = 0; i < G_N_ELEMENTS(facts); i++)
        gtk_box_append(GTK_BOX(p), label(facts[i].on ? facts[i].yes : facts[i].no, NULL));
    if (!s.is_xprism)
        gtk_box_append(GTK_BOX(p), label("This X server is not XPrism: most settings here "
                                         "do nothing.", "error"));
    return p;
}

static gboolean
tick(gpointer stack)
{
    const char *name = gtk_stack_get_visible_child_name(GTK_STACK(stack));
    if (name && !strcmp(name, "apps"))
        refresh_apps();
    return G_SOURCE_CONTINUE;
}

static void
on_page(GObject *stack, GParamSpec *ps, gpointer data)
{
    (void) ps;
    (void) data;
    const char *name = gtk_stack_get_visible_child_name(GTK_STACK(stack));
    if (!name)
        return;
    if (!strcmp(name, "displays"))
        refresh_displays();
    else if (!strcmp(name, "namespaces"))
        refresh_namespaces();
    else if (!strcmp(name, "apps"))
        refresh_apps();
}

static void
activate(GtkApplication *app, gpointer data)
{
    const char *display = data;
    x = xpctl_open(display);
    window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "XPrism Control Panel");
    gtk_window_set_default_size(GTK_WINDOW(window), 860, 600);
    if (!x || !xpctl_connection(x)) {
        GtkAlertDialog *d = gtk_alert_dialog_new("Cannot reach the X server");
        gtk_alert_dialog_set_detail(d, x ? xpctl_error(x) : "out of memory");
        gtk_window_present(GTK_WINDOW(window));
        gtk_alert_dialog_show(d, GTK_WINDOW(window));
        g_object_unref(d);
        return;
    }

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    banner = label("", "error");
    gtk_widget_set_margin_start(banner, 24);
    gtk_widget_set_margin_top(banner, 8);
    gtk_widget_set_visible(banner, FALSE);
    gtk_box_append(GTK_BOX(outer), banner);

    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_vexpand(body, TRUE);
    GtkWidget *stack = gtk_stack_new();
    gtk_widget_set_hexpand(stack, TRUE);
    gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);

    gtk_stack_add_titled(GTK_STACK(stack), overview_page(), "overview", "Overview");

    GtkWidget *d = page("Displays",
        "Resolution, refresh rate and placement of each monitor. Changes you keep are "
        "saved and come back at the next login (xprismctl displays restore). HDR, EDR and "
        "wide gamut last until the server restarts; for good, set them in xorg.conf.");
    confirm_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    confirm_label = label("", "heading");
    gtk_widget_set_hexpand(confirm_label, TRUE);
    gtk_box_append(GTK_BOX(confirm_bar), confirm_label);
    GtkWidget *revert = gtk_button_new_with_label("Go back");
    g_signal_connect(revert, "clicked", G_CALLBACK(on_revert), NULL);
    gtk_box_append(GTK_BOX(confirm_bar), revert);
    GtkWidget *keep = gtk_button_new_with_label("Keep");
    gtk_widget_add_css_class(keep, "suggested-action");
    g_signal_connect(keep, "clicked", G_CALLBACK(on_keep), NULL);
    gtk_box_append(GTK_BOX(confirm_bar), keep);
    gtk_widget_set_visible(confirm_bar, FALSE);
    gtk_box_append(GTK_BOX(d), confirm_bar);
    displays_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_box_append(GTK_BOX(d), scrolled(displays_box));
    gtk_stack_add_titled(GTK_STACK(stack), d, "displays", "Displays");

    gtk_stack_add_titled(GTK_STACK(stack), namespaces_page(), "namespaces", "Namespaces");

    GtkWidget *a = page("Apps", "Applications connected to this display, and what each "
                        "may do beyond its own windows.");
    apps_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_append(GTK_BOX(a), scrolled(apps_box));
    gtk_stack_add_titled(GTK_STACK(stack), a, "apps", "Apps");

    gtk_stack_add_titled(GTK_STACK(stack), prompts_page(), "prompts", "Prompts");

    GtkWidget *side = gtk_stack_sidebar_new();
    gtk_stack_sidebar_set_stack(GTK_STACK_SIDEBAR(side), GTK_STACK(stack));
    gtk_widget_set_size_request(side, 170, -1);
    gtk_box_append(GTK_BOX(body), side);
    gtk_box_append(GTK_BOX(body), stack);
    gtk_box_append(GTK_BOX(outer), body);
    gtk_window_set_child(GTK_WINDOW(window), outer);

    g_signal_connect(stack, "notify::visible-child-name", G_CALLBACK(on_page), NULL);
    g_timeout_add_seconds(2, tick, stack);
    /* events read along with replies wait in xcb's queue, not on the fd */
    g_timeout_add(500, on_prompts_queued, NULL);

    const char *start = g_getenv("XPRISM_PANEL_PAGE");
    if (start && gtk_stack_get_child_by_name(GTK_STACK(stack), start))
        gtk_stack_set_visible_child_name(GTK_STACK(stack), start);
    gtk_window_present(GTK_WINDOW(window));
}

int
main(int argc, char **argv)
{
    /* --xdisplay=NAME: the X server to control (default $DISPLAY); the
       window itself opens wherever GTK opens it */
    const char *display = NULL;
    for (int i = 1; i < argc; i++)
        if (g_str_has_prefix(argv[i], "--xdisplay="))
            display = argv[i] + strlen("--xdisplay=");
    GtkApplication *app = gtk_application_new("org.xprism.ControlPanel",
                                              G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), (gpointer) display);
    char *args[] = { argv[0], NULL };
    int status = g_application_run(G_APPLICATION(app), 1, args);
    g_object_unref(app);
    xpctl_close(x);
    return status;
}
