/*
 * binbows-installer.c
 * Michaelsoft Binbows 98 — Custom GTK3 installer
 *
 * Presents a Chicago95-themed install wizard.
 * Shells out to /usr/local/bin/binbows-install for disk operations.
 */

#include <gtk/gtk.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>

/* ──────────────────────────────────────────────────────────────
   CSS — Chicago95 flavoured, deliberately chunky and dated
   ────────────────────────────────────────────────────────────── */

static const char *INSTALLER_CSS =
    /* Main window */
    "window {"
    "  background-color: #c0c0c0;"
    "}"
    /* Title bar banner at the top of each page */
    ".wizard-banner {"
    "  background-color: #000080;"
    "  color: #ffffff;"
    "  font-family: 'MS Sans Serif', 'Sans', sans-serif;"
    "  font-size: 14px;"
    "  font-weight: bold;"
    "  padding: 8px 12px;"
    "}"
    ".wizard-banner-sub {"
    "  background-color: #000080;"
    "  color: #c0c0c0;"
    "  font-family: 'MS Sans Serif', 'Sans', sans-serif;"
    "  font-size: 11px;"
    "  padding: 0px 12px 8px 12px;"
    "}"
    /* Body area */
    ".wizard-body {"
    "  background-color: #c0c0c0;"
    "  padding: 16px;"
    "}"
    /* Standard labels */
    ".wizard-label {"
    "  font-family: 'MS Sans Serif', 'Sans', sans-serif;"
    "  font-size: 12px;"
    "  color: #000000;"
    "}"
    ".wizard-small {"
    "  font-family: 'MS Sans Serif', 'Sans', sans-serif;"
    "  font-size: 11px;"
    "  color: #444444;"
    "}"
    /* Warn text */
    ".wizard-warn {"
    "  font-family: 'MS Sans Serif', 'Sans', sans-serif;"
    "  font-size: 11px;"
    "  color: #aa0000;"
    "  font-weight: bold;"
    "}"
    /* Buttons — classic raised Win9x look */
    "button {"
    "  font-family: 'MS Sans Serif', 'Sans', sans-serif;"
    "  font-size: 12px;"
    "  background-color: #c0c0c0;"
    "  border: 2px outset #ffffff;"
    "  padding: 3px 14px;"
    "  min-width: 75px;"
    "}"
    "button:active {"
    "  border: 2px inset #808080;"
    "}"
    /* Progress bar */
    "progressbar trough {"
    "  background-color: #808080;"
    "  border: 2px inset #606060;"
    "  min-height: 18px;"
    "}"
    "progressbar progress {"
    "  background-color: #000080;"
    "  min-height: 18px;"
    "}"
    /* Combo box */
    "combobox {"
    "  font-family: 'MS Sans Serif', 'Sans', sans-serif;"
    "  font-size: 12px;"
    "}"
    /* Horizontal separator */
    "separator {"
    "  background-color: #808080;"
    "  min-height: 2px;"
    "  margin: 4px 0px;"
    "}";

/* ──────────────────────────────────────────────────────────────
   State
   ────────────────────────────────────────────────────────────── */

static GtkWidget   *window;
static GtkWidget   *stack;           /* GtkStack — one child per page */

/* Page: disk selection */
static GtkWidget   *disk_combo;

/* Page: progress */
static GtkWidget   *progress_bar;
static GtkWidget   *progress_label;
static GtkWidget   *progress_log;    /* GtkTextView for install log */

static gchar       *selected_disk = NULL;

/* ──────────────────────────────────────────────────────────────
   Helpers
   ────────────────────────────────────────────────────────────── */

/* Append text to the scrolling log view */
static void log_append(const gchar *text) {
    GtkTextBuffer *buf =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(progress_log));
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buf, &end);
    gtk_text_buffer_insert(buf, &end, text, -1);
    gtk_text_buffer_insert(buf, &end, "\n", -1);

    /* Auto-scroll to bottom */
    GtkTextMark *mark = gtk_text_buffer_get_mark(buf, "insert");
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(progress_log), mark);

    /* Let GTK redraw */
    while (gtk_events_pending()) gtk_main_iteration();
}

/* Create a label with a given CSS class */
static GtkWidget *styled_label(const gchar *text, const gchar *css_class) {
    GtkWidget *lbl = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(lbl), TRUE);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(lbl), css_class);
    return lbl;
}

/* ──────────────────────────────────────────────────────────────
   Page builder helpers
   ────────────────────────────────────────────────────────────── */

/*
 * Every page has the same structure:
 *   [blue banner]
 *   [body vbox]
 *   [separator]
 *   [button row]
 */
static GtkWidget *make_page(const gchar *title,
                             const gchar *subtitle,
                             GtkWidget  **body_out,
                             GtkWidget  **btn_row_out)
{
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* Banner — horizontal box so logo sits on the right */
    GtkWidget *banner_outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(banner_outer), "wizard-banner");

    /* Left side: title + subtitle stacked vertically */
    GtkWidget *banner_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(banner_box), "wizard-banner");
    gtk_box_pack_start(GTK_BOX(banner_outer), banner_box, TRUE, TRUE, 0);

    GtkWidget *title_lbl = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(title_lbl), 0.0);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(title_lbl), "wizard-banner");
    gtk_box_pack_start(GTK_BOX(banner_box), title_lbl, FALSE, FALSE, 0);

    if (subtitle && *subtitle) {
        GtkWidget *sub_lbl = gtk_label_new(subtitle);
        gtk_label_set_xalign(GTK_LABEL(sub_lbl), 0.0);
        gtk_style_context_add_class(
            gtk_widget_get_style_context(sub_lbl), "wizard-banner-sub");
        gtk_box_pack_start(GTK_BOX(banner_box), sub_lbl, FALSE, FALSE, 0);
    }

    /* Right side: small logo (32x32 scaled from 128x128 source) */
    GdkPixbuf *logo_full = gdk_pixbuf_new_from_file(
        "/usr/local/bin/logo.png", NULL);
    if (logo_full) {
        GdkPixbuf *logo_small = gdk_pixbuf_scale_simple(
            logo_full, 32, 32, GDK_INTERP_NEAREST);
        gdk_pixbuf_unref(logo_full);
        GtkWidget *logo_img = gtk_image_new_from_pixbuf(logo_small);
        gdk_pixbuf_unref(logo_small);
        gtk_widget_set_margin_end(logo_img, 8);
        gtk_box_pack_end(GTK_BOX(banner_outer), logo_img, FALSE, FALSE, 0);
    }

    gtk_box_pack_start(GTK_BOX(page), banner_outer, FALSE, FALSE, 0);

    /* Body */
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(body), "wizard-body");
    gtk_box_pack_start(GTK_BOX(page), body, TRUE, TRUE, 0);
    if (body_out) *body_out = body;

    /* Separator + button row */
    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(page), sep, FALSE, FALSE, 0);

    GtkWidget *btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start (btn_row, 8);
    gtk_widget_set_margin_end   (btn_row, 8);
    gtk_widget_set_margin_top   (btn_row, 6);
    gtk_widget_set_margin_bottom(btn_row, 8);
    gtk_box_pack_start(GTK_BOX(page), btn_row, FALSE, FALSE, 0);
    if (btn_row_out) *btn_row_out = btn_row;

    return page;
}

/* ──────────────────────────────────────────────────────────────
   Install script runner
   ────────────────────────────────────────────────────────────── */

typedef struct {
    gdouble  progress;
    gchar   *log_line;
    gboolean done;
    gboolean success;
} InstallUpdate;

/* Called on main thread to apply updates from the install thread */
static gboolean apply_install_update(gpointer data) {
    InstallUpdate *u = (InstallUpdate *)data;

    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar),
                                  u->progress);

    if (u->log_line && *u->log_line) {
        log_append(u->log_line);
        gtk_label_set_text(GTK_LABEL(progress_label), u->log_line);
    }

    if (u->done) {
        if (u->success)
            gtk_stack_set_visible_child_name(GTK_STACK(stack), "done");
        else
            gtk_stack_set_visible_child_name(GTK_STACK(stack), "error");
    }

    g_free(u->log_line);
    g_free(u);
    return G_SOURCE_REMOVE;
}

static gpointer run_install_thread(gpointer data) {
    const gchar *disk = (const gchar *)data;

    gchar *cmd = g_strdup_printf(
        "/usr/local/bin/binbows-install %s 2>&1", disk);
    FILE *fp = popen(cmd, "r");
    g_free(cmd);

    if (!fp) {
        InstallUpdate *u = g_new0(InstallUpdate, 1);
        u->log_line = g_strdup("ERROR: Could not launch install script.");
        u->done = TRUE;
        u->success = FALSE;
        g_idle_add(apply_install_update, u);
        return NULL;
    }

    /*
     * The install script outputs lines of the form:
     *   PROGRESS:0.15
     *   LOG:Formatting partitions...
     * This keeps the UI responsive without blocking the main thread.
     */
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        /* Strip trailing newline */
        line[strcspn(line, "\n")] = '\0';

        InstallUpdate *u = g_new0(InstallUpdate, 1);

        if (g_str_has_prefix(line, "PROGRESS:")) {
            u->progress = g_ascii_strtod(line + 9, NULL);
            u->log_line = g_strdup("");
        } else if (g_str_has_prefix(line, "LOG:")) {
            u->log_line = g_strdup(line + 4);
        } else {
            u->log_line = g_strdup(line);
        }

        g_idle_add(apply_install_update, u);
    }

    int exit_code = pclose(fp);

    InstallUpdate *final = g_new0(InstallUpdate, 1);
    final->progress = 1.0;
    final->log_line = g_strdup(exit_code == 0
                                ? "Installation complete."
                                : "Installation failed. See log above.");
    final->done    = TRUE;
    final->success = (exit_code == 0);
    g_idle_add(apply_install_update, final);

    return NULL;
}

/* ──────────────────────────────────────────────────────────────
   Disk scanner
   ────────────────────────────────────────────────────────────── */

static void populate_disk_combo(GtkComboBoxText *combo) {
    const char *prefixes[] = { "sd", "nvme", "vd", "hd", NULL };
    DIR *dir = opendir("/dev");
    if (!dir) return;

    struct dirent *entry;
    /* Collect matching devices */
    GPtrArray *devs = g_ptr_array_new_with_free_func(g_free);

    while ((entry = readdir(dir)) != NULL) {
        for (int i = 0; prefixes[i]; i++) {
            if (g_str_has_prefix(entry->d_name, prefixes[i])) {
                /* Only whole disks, not partitions (no trailing digit for sd*) */
                const char *n = entry->d_name;
                gboolean is_whole = TRUE;
                if (g_str_has_prefix(n, "sd") || g_str_has_prefix(n, "hd") || g_str_has_prefix(n, "vd")) {
                    /* whole disk = exactly 3 chars like sda, sdb */
                    if (strlen(n) != 3) is_whole = FALSE;
                } else if (g_str_has_prefix(n, "nvme")) {
                    /* whole disk = nvme0n1, not nvme0n1p1 */
                    if (strstr(n, "p") != NULL) is_whole = FALSE;
                }
                if (is_whole)
                    g_ptr_array_add(devs, g_strdup_printf("/dev/%s", n));
            }
        }
    }
    closedir(dir);

    /* Sort for consistent ordering */
    g_ptr_array_sort(devs, (GCompareFunc)g_strcmp0);

    for (guint i = 0; i < devs->len; i++)
        gtk_combo_box_text_append_text(combo, devs->pdata[i]);

    if (devs->len > 0)
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);

    g_ptr_array_free(devs, TRUE);
}

/* ──────────────────────────────────────────────────────────────
   Page callbacks
   ────────────────────────────────────────────────────────────── */

static void on_welcome_next(GtkButton *btn, gpointer data) {
    (void)btn; (void)data;
    gtk_stack_set_visible_child_name(GTK_STACK(stack), "disk");
}

static void on_disk_next(GtkButton *btn, gpointer data) {
    (void)btn; (void)data;
    g_free(selected_disk);
    selected_disk = gtk_combo_box_text_get_active_text(
                        GTK_COMBO_BOX_TEXT(disk_combo));
    if (!selected_disk || !*selected_disk) return;
    gtk_stack_set_visible_child_name(GTK_STACK(stack), "confirm");
}

static void on_disk_back(GtkButton *btn, gpointer data) {
    (void)btn; (void)data;
    gtk_stack_set_visible_child_name(GTK_STACK(stack), "welcome");
}

static void on_confirm_install(GtkButton *btn, gpointer data) {
    (void)btn; (void)data;
    gtk_stack_set_visible_child_name(GTK_STACK(stack), "progress");
    /* Launch install on a background thread so the UI stays alive */
    GThread *t = g_thread_new("install",
                               run_install_thread,
                               selected_disk);
    g_thread_unref(t);
}

static void on_confirm_back(GtkButton *btn, gpointer data) {
    (void)btn; (void)data;
    gtk_stack_set_visible_child_name(GTK_STACK(stack), "disk");
}

static void on_reboot(GtkButton *btn, gpointer data) {
    (void)btn; (void)data;
    g_spawn_command_line_async("reboot", NULL);
}

static void on_quit(GtkButton *btn, gpointer data) {
    (void)btn; (void)data;
    gtk_main_quit();
}

/* ──────────────────────────────────────────────────────────────
   Page builders
   ────────────────────────────────────────────────────────────── */

static GtkWidget *build_welcome_page(void) {
    GtkWidget *body, *btn_row;
    GtkWidget *page = make_page(
        "Welcome to Binbows 98 Setup",
        "Install Michaelsoft Binbows 98 on your computer",
        &body, &btn_row);

    /* Large splash logo — 128x128, centered */
    GdkPixbuf *splash_buf = gdk_pixbuf_new_from_file(
        "/usr/local/bin/logo.png", NULL);
    if (splash_buf) {
        GtkWidget *splash_img = gtk_image_new_from_pixbuf(splash_buf);
        gdk_pixbuf_unref(splash_buf);
        gtk_widget_set_halign(splash_img, GTK_ALIGN_CENTER);
        gtk_widget_set_margin_bottom(splash_img, 8);
        gtk_box_pack_start(GTK_BOX(body), splash_img, FALSE, FALSE, 0);
    }

    gtk_box_pack_start(GTK_BOX(body),
        styled_label(
            "Welcome to Binbows 98 Setup.\n\n"
            "This wizard will install Michaelsoft Binbows 98 on your "
            "computer. The installation process will erase all data on "
            "the selected disk.\n\n"
            "Click Next to continue.",
            "wizard-label"),
        FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(body),
        styled_label(
            "WARNING: Do not install this on a computer you care about.",
            "wizard-warn"),
        FALSE, FALSE, 0);

    /* Spacer */
    gtk_box_pack_start(GTK_BOX(btn_row),
        gtk_label_new(""), TRUE, TRUE, 0);
    GtkWidget *btn_next = gtk_button_new_with_label("Next >");
    g_signal_connect(btn_next, "clicked",
                     G_CALLBACK(on_welcome_next), NULL);
    gtk_box_pack_start(GTK_BOX(btn_row), btn_next, FALSE, FALSE, 0);

    GtkWidget *btn_quit = gtk_button_new_with_label("Cancel");
    g_signal_connect(btn_quit, "clicked", G_CALLBACK(on_quit), NULL);
    gtk_box_pack_start(GTK_BOX(btn_row), btn_quit, FALSE, FALSE, 0);

    return page;
}

static GtkWidget *build_disk_page(void) {
    GtkWidget *body, *btn_row;
    GtkWidget *page = make_page(
        "Select Installation Disk",
        "Choose the disk to install Binbows 98 on",
        &body, &btn_row);

    gtk_box_pack_start(GTK_BOX(body),
        styled_label("Select the disk to install Binbows 98 on.\n"
                     "ALL DATA ON THE SELECTED DISK WILL BE ERASED.",
                     "wizard-label"),
        FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(body),
        styled_label("Available disks:", "wizard-small"),
        FALSE, FALSE, 0);

    disk_combo = GTK_WIDGET(gtk_combo_box_text_new());
    populate_disk_combo(GTK_COMBO_BOX_TEXT(disk_combo));
    gtk_box_pack_start(GTK_BOX(body), disk_combo, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(body),
        styled_label("If your disk does not appear, it may not be "
                     "detected. Check connections and try again.",
                     "wizard-small"),
        FALSE, FALSE, 0);

    /* Buttons */
    gtk_box_pack_start(GTK_BOX(btn_row),
        gtk_label_new(""), TRUE, TRUE, 0);

    GtkWidget *btn_next = gtk_button_new_with_label("Next >");
    g_signal_connect(btn_next, "clicked",
                     G_CALLBACK(on_disk_next), NULL);
    gtk_box_pack_start(GTK_BOX(btn_row), btn_next, FALSE, FALSE, 0);

    GtkWidget *btn_back = gtk_button_new_with_label("< Back");
    g_signal_connect(btn_back, "clicked",
                     G_CALLBACK(on_disk_back), NULL);
    gtk_box_pack_start(GTK_BOX(btn_row), btn_back, FALSE, FALSE, 0);

    GtkWidget *btn_quit = gtk_button_new_with_label("Cancel");
    g_signal_connect(btn_quit, "clicked", G_CALLBACK(on_quit), NULL);
    gtk_box_pack_start(GTK_BOX(btn_row), btn_quit, FALSE, FALSE, 0);

    return page;
}

static GtkWidget *build_confirm_page(void) {
    GtkWidget *body, *btn_row;
    GtkWidget *page = make_page(
        "Confirm Installation",
        "Last chance to turn back",
        &body, &btn_row);

    gtk_box_pack_start(GTK_BOX(body),
        styled_label(
            "You are about to install Binbows 98.\n\n"
            "THIS WILL COMPLETELY ERASE THE SELECTED DISK.\n\n"
            "There is no undo. Are you absolutely sure?",
            "wizard-warn"),
        FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(body),
        styled_label(
            "Click Install to begin. Click Back to go back. "
            "Click Cancel to exit without installing.",
            "wizard-small"),
        FALSE, FALSE, 0);

    /* Buttons */
    gtk_box_pack_start(GTK_BOX(btn_row),
        gtk_label_new(""), TRUE, TRUE, 0);

    GtkWidget *btn_install = gtk_button_new_with_label("Install");
    g_signal_connect(btn_install, "clicked",
                     G_CALLBACK(on_confirm_install), NULL);
    gtk_box_pack_start(GTK_BOX(btn_row), btn_install, FALSE, FALSE, 0);

    GtkWidget *btn_back = gtk_button_new_with_label("< Back");
    g_signal_connect(btn_back, "clicked",
                     G_CALLBACK(on_confirm_back), NULL);
    gtk_box_pack_start(GTK_BOX(btn_row), btn_back, FALSE, FALSE, 0);

    GtkWidget *btn_quit = gtk_button_new_with_label("Cancel");
    g_signal_connect(btn_quit, "clicked", G_CALLBACK(on_quit), NULL);
    gtk_box_pack_start(GTK_BOX(btn_row), btn_quit, FALSE, FALSE, 0);

    return page;
}

static GtkWidget *build_progress_page(void) {
    GtkWidget *body, *btn_row;
    GtkWidget *page = make_page(
        "Installing Binbows 98",
        "Please wait. Do not turn off your computer.",
        &body, &btn_row);

    progress_label = styled_label("Preparing...", "wizard-small");
    gtk_box_pack_start(GTK_BOX(body), progress_label, FALSE, FALSE, 0);

    progress_bar = gtk_progress_bar_new();
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 0.0);
    gtk_box_pack_start(GTK_BOX(body), progress_bar, FALSE, FALSE, 4);

    /* Scrollable log */
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, 160);

    progress_log = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(progress_log), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(progress_log), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(progress_log),
                                GTK_WRAP_WORD_CHAR);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(progress_log), "wizard-small");
    gtk_container_add(GTK_CONTAINER(scroll), progress_log);
    gtk_box_pack_start(GTK_BOX(body), scroll, TRUE, TRUE, 0);

    /* No buttons on progress page — can't go back mid-install */
    (void)btn_row;

    return page;
}

static GtkWidget *build_done_page(void) {
    GtkWidget *body, *btn_row;
    GtkWidget *page = make_page(
        "Installation Complete",
        "Binbows 98 has been installed successfully",
        &body, &btn_row);

    gtk_box_pack_start(GTK_BOX(body),
        styled_label(
            "Binbows 98 has been installed on your computer.\n\n"
            "Remove the installation media and click Restart to "
            "boot into your new Binbows 98 installation.",
            "wizard-label"),
        FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(btn_row),
        gtk_label_new(""), TRUE, TRUE, 0);

    GtkWidget *btn_reboot = gtk_button_new_with_label("Restart");
    g_signal_connect(btn_reboot, "clicked",
                     G_CALLBACK(on_reboot), NULL);
    gtk_box_pack_start(GTK_BOX(btn_row), btn_reboot, FALSE, FALSE, 0);

    return page;
}

static GtkWidget *build_error_page(void) {
    GtkWidget *body, *btn_row;
    GtkWidget *page = make_page(
        "Installation Failed",
        "Something went wrong",
        &body, &btn_row);

    gtk_box_pack_start(GTK_BOX(body),
        styled_label(
            "The installation did not complete successfully.\n\n"
            "Check the log above for details. You may close this "
            "window and try again.",
            "wizard-warn"),
        FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(btn_row),
        gtk_label_new(""), TRUE, TRUE, 0);

    GtkWidget *btn_quit = gtk_button_new_with_label("Close");
    g_signal_connect(btn_quit, "clicked", G_CALLBACK(on_quit), NULL);
    gtk_box_pack_start(GTK_BOX(btn_row), btn_quit, FALSE, FALSE, 0);

    return page;
}

/* ──────────────────────────────────────────────────────────────
   main
   ────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    /* CSS */
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, INSTALLER_CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    /* Window */
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Binbows 98 Setup");
    gtk_window_set_default_size(GTK_WINDOW(window), 500, 380);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    /* Stack */
    stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack),
                                  GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_container_add(GTK_CONTAINER(window), stack);

    gtk_stack_add_named(GTK_STACK(stack), build_welcome_page(),  "welcome");
    gtk_stack_add_named(GTK_STACK(stack), build_disk_page(),     "disk");
    gtk_stack_add_named(GTK_STACK(stack), build_confirm_page(),  "confirm");
    gtk_stack_add_named(GTK_STACK(stack), build_progress_page(), "progress");
    gtk_stack_add_named(GTK_STACK(stack), build_done_page(),     "done");
    gtk_stack_add_named(GTK_STACK(stack), build_error_page(),    "error");

    gtk_stack_set_visible_child_name(GTK_STACK(stack), "welcome");

    gtk_widget_show_all(window);
    gtk_main();

    g_free(selected_disk);
    return 0;
}
