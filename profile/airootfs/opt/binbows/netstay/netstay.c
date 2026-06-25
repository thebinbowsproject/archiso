#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define SITES_DIR "/opt/binbows/netstay/sites"

/* Global widget references */
static GtkEntry    *url_bar;
static WebKitWebView *web_view;
static GtkButton   *back_btn;

/* ─────────────────────────────────────────────
   Helpers
   ───────────────────────────────────────────── */

static int compare_strings(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    char *buf = malloc(size + 1);
    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);
    return buf;
}

/*
 * Looks for   <URL="sitp://whatever"></URL>
 * in HTML content and returns the URL string,
 * or NULL if the tag isn't present.
 * Caller must g_free() the result.
 */
static char *extract_declared_url(const char *html) {
    const char *tag = strstr(html, "<URL=\"");
    if (!tag) return NULL;
    tag += 6;                          /* skip past <URL=" */
    const char *end = strchr(tag, '"');
    if (!end) return NULL;
    return g_strndup(tag, end - tag);
}

/* ─────────────────────────────────────────────
   Page generators
   ───────────────────────────────────────────── */

static const char *HOMEPAGE_HTML =
    "<!DOCTYPE html><html><head><style>"
    "  body   { margin:0; background:#008080;"
    "           font-family:'MS Sans Serif',Arial,sans-serif;"
    "           text-align:center; }"
    "  h1     { color:#ffffff; margin-top:70px; font-size:54px;"
    "           text-shadow:3px 3px #003030; letter-spacing:2px; }"
    "  .splash{ color:#ffff00; font-size:13px; font-style:italic;"
    "           margin-bottom:36px; }"
    "  .box   { display:inline-block; background:#c0c0c0;"
    "           border:3px outset #ffffff; padding:18px 28px; }"
    "  input[type=text]  { width:260px; padding:3px 5px; font-size:13px;"
    "                      border:2px inset #808080; }"
    "  input[type=submit]{ margin-left:6px; padding:3px 10px; font-size:13px;"
    "                      border:2px outset #ffffff; background:#c0c0c0;"
    "                      cursor:pointer; }"
    "</style></head><body>"
    "<h1>Netstay</h1>"
    "<p class='splash'>Now with SuperText Image Transfer Protocol</p>"
    "<div class='box'>"
    "  <form onsubmit=\""
    "    window.location='sitp://search?q='"
    "    +encodeURIComponent(document.getElementById('q').value);"
    "    return false;\">"
    "    <input type='text' id='q' autofocus placeholder='Search the web...' />"
    "    <input type='submit' value='Go' />"
    "  </form>"
    "</div>"
    "</body></html>";

static char *generate_results_page(void) {
    DIR *dir = opendir(SITES_DIR);
    if (!dir) {
        return g_strdup(
            "<html><body style='font-family:sans-serif;padding:20px'>"
            "<b>No sites folder found.</b><br>"
            "Expected: " SITES_DIR
            "</body></html>");
    }

    /* Collect .html / .htm files */
    char  **files = NULL;
    int     count = 0;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        const char *n = entry->d_name;
        if (strstr(n, ".html") || strstr(n, ".htm")) {
            files = realloc(files, (count + 1) * sizeof(char *));
            files[count++] = strdup(n);
        }
    }
    closedir(dir);

    /* Sort alphabetically */
    if (count > 0)
        qsort(files, count, sizeof(char *), compare_strings);

    /* Build HTML */
    GString *html = g_string_new(
        "<!DOCTYPE html><html><head><style>"
        "  body { background:#c0c0c0; font-family:'MS Sans Serif',sans-serif;"
        "         padding:16px; }"
        "  h2   { color:#000080; margin-top:0; }"
        "  ul   { list-style:none; padding:0; margin:0; }"
        "  li a { display:block; padding:5px 8px; color:#000080;"
        "         text-decoration:underline; font-size:13px; }"
        "  li a:hover { background:#000080; color:#ffffff; }"
        "</style></head><body>"
        "<h2>Search Results</h2><ul>");

    if (count == 0) {
        g_string_append(html, "<li>No pages found in " SITES_DIR "</li>");
    } else {
        for (int i = 0; i < count; i++) {
            /* Strip .html extension for display name */
            char display[256];
            strncpy(display, files[i], sizeof(display) - 1);
            display[sizeof(display) - 1] = '\0';
            char *dot = strrchr(display, '.');
            if (dot) *dot = '\0';

            g_string_append_printf(html,
                "<li><a href='sitp://site/%s'>%s</a></li>",
                files[i], display);
            free(files[i]);
        }
    }
    free(files);

    g_string_append(html, "</ul></body></html>");
    return g_string_free(html, FALSE);
}

/* ─────────────────────────────────────────────
   SITP URI scheme handler
   sitp://home          → homepage
   sitp://search?q=...  → results listing
   sitp://site/foo.html → load from SITES_DIR
   ───────────────────────────────────────────── */

static void sitp_uri_scheme_cb(WebKitURISchemeRequest *request,
                               gpointer               user_data)
{
    const char *uri  = webkit_uri_scheme_request_get_uri(request);
    char       *content = NULL;
    gsize       length;

    if (g_str_has_prefix(uri, "sitp://home")) {
        /* ── Homepage ── */
        content = g_strdup(HOMEPAGE_HTML);
        gtk_entry_set_text(url_bar, "sitp://home");

    } else if (g_str_has_prefix(uri, "sitp://search")) {
        /* ── Search results ── */
        content = generate_results_page();
        gtk_entry_set_text(url_bar, "sitp://search");

    } else if (g_str_has_prefix(uri, "sitp://site/")) {
        /* ── Load a local site file ── */
        const char *filename = uri + strlen("sitp://site/");

        /* Guard against path traversal */
        if (strstr(filename, "..")) {
            content = g_strdup("<html><body>Nice try.</body></html>");
            gtk_entry_set_text(url_bar, "sitp://error");
        } else {
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", SITES_DIR, filename);
            content = read_file(path);

            if (!content) {
                content = g_strdup(
                    "<html><body style='font-family:sans-serif;padding:20px'>"
                    "<b>404 – Page Not Found</b></body></html>");
                gtk_entry_set_text(url_bar, uri);
            } else {
                /* Let the page declare its own URL */
                char *declared = extract_declared_url(content);
                gtk_entry_set_text(url_bar, declared ? declared : uri);
                g_free(declared);
            }
        }

    } else {
        content = g_strdup(
            "<html><body style='font-family:sans-serif;padding:20px'>"
            "<b>Unknown SITP address.</b></body></html>");
        gtk_entry_set_text(url_bar, uri);
    }

    length = strlen(content);
    GInputStream *stream =
        g_memory_input_stream_new_from_data(content, length, g_free);
    webkit_uri_scheme_request_finish(request, stream, length, "text/html");
    g_object_unref(stream);

    /* Update back-button sensitivity */
    gtk_widget_set_sensitive(GTK_WIDGET(back_btn),
                             webkit_web_view_can_go_back(web_view));
}

/* ─────────────────────────────────────────────
   Signal callbacks
   ───────────────────────────────────────────── */

static void load_homepage(void) {
    webkit_web_view_load_uri(web_view, "sitp://home");
}

static void on_back_clicked(GtkButton *btn, gpointer data) {
    (void)btn; (void)data;
    webkit_web_view_go_back(web_view);
}

static void on_home_clicked(GtkButton *btn, gpointer data) {
    (void)btn; (void)data;
    load_homepage();
}

static void on_url_bar_activate(GtkEntry *entry, gpointer data) {
    (void)data;
    const char *uri = gtk_entry_get_text(entry);
    /* Only navigate SITP URIs; ignore anything else */
    if (g_str_has_prefix(uri, "sitp://")) {
        webkit_web_view_load_uri(web_view, uri);
    }
}

/* ─────────────────────────────────────────────
   main
   ───────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    /* Register the sitp:// scheme before creating any web view */
    WebKitWebContext *ctx = webkit_web_context_get_default();
    webkit_web_context_register_uri_scheme(ctx, "sitp",
                                           sitp_uri_scheme_cb,
                                           NULL, NULL);

    /* ── Window ── */
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Netstay");
    gtk_window_set_default_size(GTK_WINDOW(window), 900, 640);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    /* ── Root layout ── */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    /* ── Toolbar ── */
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start (toolbar, 4);
    gtk_widget_set_margin_end   (toolbar, 4);
    gtk_widget_set_margin_top   (toolbar, 4);
    gtk_widget_set_margin_bottom(toolbar, 4);
    gtk_box_pack_start(GTK_BOX(vbox), toolbar, FALSE, FALSE, 0);

    GtkWidget *b_back = gtk_button_new_with_label("◀ Back");
    back_btn = GTK_BUTTON(b_back);
    gtk_widget_set_sensitive(b_back, FALSE);
    g_signal_connect(b_back, "clicked", G_CALLBACK(on_back_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(toolbar), b_back, FALSE, FALSE, 0);

    GtkWidget *b_home = gtk_button_new_with_label("⌂ Home");
    g_signal_connect(b_home, "clicked", G_CALLBACK(on_home_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(toolbar), b_home, FALSE, FALSE, 0);

    GtkWidget *url_entry = gtk_entry_new();
    url_bar = GTK_ENTRY(url_entry);
    g_signal_connect(url_entry, "activate",
                     G_CALLBACK(on_url_bar_activate), NULL);
    gtk_box_pack_start(GTK_BOX(toolbar), url_entry, TRUE, TRUE, 0);

    /* ── Separator ── */
    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(vbox), sep, FALSE, FALSE, 0);

    /* ── WebKit view ── */
    GtkWidget *wv = webkit_web_view_new();
    web_view = WEBKIT_WEB_VIEW(wv);
    gtk_box_pack_start(GTK_BOX(vbox), wv, TRUE, TRUE, 0);

    /* ── Go ── */
    load_homepage();
    gtk_widget_show_all(window);
    gtk_main();

    return 0;
}
