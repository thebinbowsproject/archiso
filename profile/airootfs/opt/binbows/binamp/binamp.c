/*
 * binamp.c — Michaelsoft Binbows 98 music player
 * GTK3 + GStreamer
 *
 * Scans /opt/binbows/binamp/audio/ for MP3 files.
 * Reads metadata via GstDiscoverer, plays via GStreamer pipeline.
 */

#include <gtk/gtk.h>
#include <gst/gst.h>
#include <gst/pbutils/pbutils.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>

#define AUDIO_DIR    "/opt/binbows/binamp/audio"
#define DISPLAY_COLS 26          /* characters visible in the LED ticker */
#define MAX_TRACKS   256

/* ──────────────────────────────────────────────────────────────
   Data
   ────────────────────────────────────────────────────────────── */

typedef struct {
    gchar *filepath;
    gchar *title;
    gchar *subtitle;
    gchar *artist;
    gchar *album;
    gchar *genre;
    guint  year;
    guint  track_number;
    guint  rating;           /* 0–100 from GST_TAG_USER_RATING */
    gint64 duration_ns;
} TrackInfo;

static TrackInfo  tracks[MAX_TRACKS];
static int        track_count   = 0;
static int        current_track = -1;

/* ──────────────────────────────────────────────────────────────
   GStreamer state
   ────────────────────────────────────────────────────────────── */

static GstElement *pipeline     = NULL;
static gboolean    is_playing   = FALSE;
static guint       bus_watch_id = 0;

/* ──────────────────────────────────────────────────────────────
   GTK widgets we need to update later
   ────────────────────────────────────────────────────────────── */

static GtkWidget *win_main;
static GtkWidget *display_label;      /* LED ticker */
static GtkWidget *time_label;         /* current position */
static GtkWidget *seek_bar;
static GtkWidget *volume_slider;
static GtkWidget *btn_play;
static GtkWidget *playlist_box;       /* VBox of track-name buttons */

/* Metadata value labels */
static GtkWidget *val_title;
static GtkWidget *val_subtitle;
static GtkWidget *val_artist;
static GtkWidget *val_album;
static GtkWidget *val_year;
static GtkWidget *val_track;
static GtkWidget *val_genre;
static GtkWidget *val_length;
static GtkWidget *val_rating;

/* Ticker state */
static gchar    *ticker_string  = NULL;
static int       ticker_offset  = 0;
static guint     ticker_timer   = 0;

/* Seek-bar update timer */
static guint     pos_timer      = 0;

/* ──────────────────────────────────────────────────────────────
   CSS
   ────────────────────────────────────────────────────────────── */

static const char *BINAMP_CSS =
    /* overall window background */
    "window.binamp {"
    "  background-color: #2a2a2a;"
    "}"

    /* LED display area — green on black */
    ".led-display {"
    "  background-color: #080808;"
    "  color: #00dd00;"
    "  font-family: 'Courier New', Courier, monospace;"
    "  font-size: 13px;"
    "  font-weight: bold;"
    "  padding: 6px 8px;"
    "  border: 2px inset #111111;"
    "}"

    /* transport buttons */
    ".transport-btn {"
    "  background-color: #4a4a4a;"
    "  color: #dddddd;"
    "  border: 2px outset #666666;"
    "  font-family: monospace;"
    "  font-size: 11px;"
    "  min-width: 32px;"
    "  min-height: 22px;"
    "  padding: 0 4px;"
    "}"
    ".transport-btn:active {"
    "  border: 2px inset #333333;"
    "  background-color: #3a3a3a;"
    "}"

    /* seek / volume sliders */
    ".binamp-scale trough {"
    "  background-color: #111111;"
    "  min-height: 6px;"
    "}"
    ".binamp-scale slider {"
    "  background-color: #00bb00;"
    "  min-width: 10px; min-height: 10px;"
    "}"

    /* metadata panel */
    ".meta-panel {"
    "  background-color: #181818;"
    "  padding: 6px 8px;"
    "  border-top: 2px inset #111111;"
    "}"
    ".meta-key {"
    "  color: #007700;"
    "  font-family: monospace;"
    "  font-size: 11px;"
    "  font-weight: bold;"
    "  min-width: 90px;"
    "}"
    ".meta-val {"
    "  color: #00cc00;"
    "  font-family: monospace;"
    "  font-size: 11px;"
    "}"

    /* playlist */
    ".playlist {"
    "  background-color: #111111;"
    "  border-top: 2px inset #080808;"
    "}"
    ".playlist-item {"
    "  background-color: transparent;"
    "  color: #009900;"
    "  font-family: monospace;"
    "  font-size: 11px;"
    "  border: none;"
    "  box-shadow: none;"
    "  text-align: left;"
    "  padding: 2px 6px;"
    "}"
    ".playlist-item:hover {"
    "  background-color: #003300;"
    "  color: #00ff00;"
    "}"
    ".playlist-item.playing {"
    "  color: #ffff00;"
    "  background-color: #002200;"
    "}"

    /* section labels */
    ".section-label {"
    "  background-color: #333333;"
    "  color: #888888;"
    "  font-family: monospace;"
    "  font-size: 10px;"
    "  padding: 2px 6px;"
    "}";

/* ──────────────────────────────────────────────────────────────
   Utility helpers
   ────────────────────────────────────────────────────────────── */

/* Format nanoseconds → "M:SS" */
static gchar *format_duration(gint64 ns) {
    if (ns <= 0) return g_strdup("?:??");
    gint64 total_sec = ns / GST_SECOND;
    int min = (int)(total_sec / 60);
    int sec = (int)(total_sec % 60);
    return g_strdup_printf("%d:%02d", min, sec);
}

/* Rating 0–100 → crude star string */
static gchar *format_rating(guint r) {
    if (r == 0) return g_strdup("N/A");
    int stars = (int)((r / 100.0) * 5.0 + 0.5);
    GString *s = g_string_new("");
    for (int i = 0; i < 5; i++)
        g_string_append(s, i < stars ? "*" : "-");
    g_string_append_printf(s, " (%u/100)", r);
    return g_string_free(s, FALSE);
}

/* Safe fallback for NULL strings */
static const gchar *safe(const gchar *s) {
    return (s && *s) ? s : "N/A";
}

/* ──────────────────────────────────────────────────────────────
   LED ticker
   ────────────────────────────────────────────────────────────── */

static void ticker_set(const gchar *text) {
    g_free(ticker_string);
    /* pad so the text scrolls fully off each side */
    ticker_string = g_strdup_printf("%*s%s   ", DISPLAY_COLS, " ", text);
    ticker_offset = 0;
}

static gboolean ticker_tick(gpointer data) {
    (void)data;
    if (!ticker_string) return G_SOURCE_CONTINUE;

    int len = (int)strlen(ticker_string);
    if (len <= DISPLAY_COLS) {
        gtk_label_set_text(GTK_LABEL(display_label), ticker_string);
        return G_SOURCE_CONTINUE;
    }

    /* Extract a DISPLAY_COLS-wide window */
    gchar buf[DISPLAY_COLS + 1];
    for (int i = 0; i < DISPLAY_COLS; i++)
        buf[i] = ticker_string[(ticker_offset + i) % len];
    buf[DISPLAY_COLS] = '\0';
    gtk_label_set_text(GTK_LABEL(display_label), buf);

    ticker_offset = (ticker_offset + 1) % len;
    return G_SOURCE_CONTINUE;
}

/* ──────────────────────────────────────────────────────────────
   Metadata panel update
   ────────────────────────────────────────────────────────────── */

static void update_metadata_panel(int idx) {
    if (idx < 0 || idx >= track_count) {
        gtk_label_set_text(GTK_LABEL(val_title),    "N/A");
        gtk_label_set_text(GTK_LABEL(val_subtitle), "N/A");
        gtk_label_set_text(GTK_LABEL(val_artist),   "N/A");
        gtk_label_set_text(GTK_LABEL(val_album),    "N/A");
        gtk_label_set_text(GTK_LABEL(val_year),     "N/A");
        gtk_label_set_text(GTK_LABEL(val_track),    "N/A");
        gtk_label_set_text(GTK_LABEL(val_genre),    "N/A");
        gtk_label_set_text(GTK_LABEL(val_length),   "N/A");
        gtk_label_set_text(GTK_LABEL(val_rating),   "N/A");
        return;
    }

    TrackInfo *t = &tracks[idx];

    gtk_label_set_text(GTK_LABEL(val_title),    safe(t->title));
    gtk_label_set_text(GTK_LABEL(val_subtitle), safe(t->subtitle));
    gtk_label_set_text(GTK_LABEL(val_artist),   safe(t->artist));
    gtk_label_set_text(GTK_LABEL(val_album),    safe(t->album));

    if (t->year > 0) {
        gchar *y = g_strdup_printf("%u", t->year);
        gtk_label_set_text(GTK_LABEL(val_year), y);
        g_free(y);
    } else {
        gtk_label_set_text(GTK_LABEL(val_year), "N/A");
    }

    if (t->track_number > 0) {
        gchar *tn = g_strdup_printf("%u", t->track_number);
        gtk_label_set_text(GTK_LABEL(val_track), tn);
        g_free(tn);
    } else {
        gtk_label_set_text(GTK_LABEL(val_track), "N/A");
    }

    gtk_label_set_text(GTK_LABEL(val_genre), safe(t->genre));

    gchar *dur = format_duration(t->duration_ns);
    gtk_label_set_text(GTK_LABEL(val_length), dur);
    g_free(dur);

    gchar *rat = format_rating(t->rating);
    gtk_label_set_text(GTK_LABEL(val_rating), rat);
    g_free(rat);
}

/* ──────────────────────────────────────────────────────────────
   Metadata reader (GstDiscoverer)
   ────────────────────────────────────────────────────────────── */

static void read_metadata(TrackInfo *t) {
    GError *err = NULL;
    GstDiscoverer *disc = gst_discoverer_new(5 * GST_SECOND, &err);
    if (!disc) {
        g_clear_error(&err);
        return;
    }

    gchar *uri = gst_filename_to_uri(t->filepath, &err);
    if (!uri) { g_clear_error(&err); g_object_unref(disc); return; }

    GstDiscovererInfo *info = gst_discoverer_discover_uri(disc, uri, &err);
    g_free(uri);
    if (!info) { g_clear_error(&err); g_object_unref(disc); return; }

    /* Duration */
    t->duration_ns = (gint64)gst_discoverer_info_get_duration(info);

    /* Tags */
    GstTagList *tags = gst_discoverer_info_get_tags(info);
    if (tags) {
        gchar *s = NULL;
        guint  u = 0;
        GDate *date = NULL;
        GstDateTime *dt = NULL;

        if (gst_tag_list_get_string(tags, GST_TAG_TITLE, &s))
            t->title = s;
        /* TIT3 / subtitle — GStreamer exposes it as "subtitle" */
        if (gst_tag_list_get_string(tags, "subtitle", &s))
            t->subtitle = s;
        if (gst_tag_list_get_string(tags, GST_TAG_ARTIST, &s))
            t->artist = s;
        if (gst_tag_list_get_string(tags, GST_TAG_ALBUM, &s))
            t->album = s;
        if (gst_tag_list_get_string(tags, GST_TAG_GENRE, &s))
            t->genre = s;
        if (gst_tag_list_get_uint(tags, GST_TAG_TRACK_NUMBER, &u))
            t->track_number = u;
        if (gst_tag_list_get_uint(tags, GST_TAG_USER_RATING, &u))
            t->rating = u;

        /* Year — try datetime first, then plain date */
        if (gst_tag_list_get_date_time(tags, GST_TAG_DATE_TIME, &dt)) {
            if (gst_date_time_has_year(dt))
                t->year = (guint)gst_date_time_get_year(dt);
            gst_date_time_unref(dt);
        } else if (gst_tag_list_get_date(tags, GST_TAG_DATE, &date)) {
            t->year = (guint)g_date_get_year(date);
            g_date_free(date);
        }

        gst_tag_list_unref(tags);
    }

    gst_discoverer_info_unref(info);
    g_object_unref(disc);
}

/* ──────────────────────────────────────────────────────────────
   Directory scanner
   ────────────────────────────────────────────────────────────── */

static int compare_track_info(const void *a, const void *b) {
    return strcmp(((TrackInfo *)a)->filepath, ((TrackInfo *)b)->filepath);
}

static void scan_audio_dir(void) {
    DIR *dir = opendir(AUDIO_DIR);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && track_count < MAX_TRACKS) {
        const char *n = entry->d_name;
        /* Accept .mp3, .flac, .ogg, .wav for flexibility */
        if (!strstr(n, ".mp3") && !strstr(n, ".flac")
            && !strstr(n, ".ogg") && !strstr(n, ".wav"))
            continue;

        TrackInfo *t = &tracks[track_count++];
        memset(t, 0, sizeof(*t));
        t->filepath = g_strdup_printf("%s/%s", AUDIO_DIR, n);
        /* Pre-fill title with filename (fallback if tags missing) */
        t->title    = g_strdup(n);

        read_metadata(t);
    }
    closedir(dir);

    qsort(tracks, track_count, sizeof(TrackInfo), compare_track_info);
}

/* ──────────────────────────────────────────────────────────────
   GStreamer playback
   ────────────────────────────────────────────────────────────── */

static gboolean update_position(gpointer data) {
    (void)data;
    if (!pipeline || !is_playing) return G_SOURCE_CONTINUE;

    gint64 pos = 0, dur = 0;
    gst_element_query_position(pipeline, GST_FORMAT_TIME, &pos);
    gst_element_query_duration(pipeline, GST_FORMAT_TIME, &dur);

    /* Time label */
    gchar *tp = format_duration(pos);
    gchar *td = format_duration(dur > 0 ? dur
                                        : (current_track >= 0
                                           ? tracks[current_track].duration_ns
                                           : 0));
    gchar *tl = g_strdup_printf("%s / %s", tp, td);
    gtk_label_set_text(GTK_LABEL(time_label), tl);
    g_free(tp); g_free(td); g_free(tl);

    /* Seek bar */
    if (dur > 0) {
        g_signal_handlers_block_by_func(seek_bar,
            (gpointer)gtk_range_get_value, NULL);
        gtk_range_set_value(GTK_RANGE(seek_bar),
            (double)pos / (double)dur * 100.0);
        g_signal_handlers_unblock_by_func(seek_bar,
            (gpointer)gtk_range_get_value, NULL);
    }

    return G_SOURCE_CONTINUE;
}

static gboolean bus_callback(GstBus *bus, GstMessage *msg, gpointer data) {
    (void)bus; (void)data;
    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_EOS:
        /* Auto-advance */
        if (current_track + 1 < track_count) {
            current_track++;
            /* trigger load — handled outside this callback */
            g_idle_add((GSourceFunc)gtk_widget_queue_draw, win_main);
        } else {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            is_playing = FALSE;
            gtk_button_set_label(GTK_BUTTON(btn_play), "▶ Play");
        }
        break;
    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        gst_message_parse_error(msg, &err, NULL);
        g_printerr("GStreamer error: %s\n", err->message);
        g_error_free(err);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        is_playing = FALSE;
        gtk_button_set_label(GTK_BUTTON(btn_play), "▶ Play");
        break;
    }
    default: break;
    }
    return TRUE;
}

static void stop_pipeline(void) {
    if (pipeline) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        if (bus_watch_id) {
            g_source_remove(bus_watch_id);
            bus_watch_id = 0;
        }
        gst_object_unref(pipeline);
        pipeline = NULL;
    }
    is_playing = FALSE;
}

static void play_track(int idx) {
    if (idx < 0 || idx >= track_count) return;

    stop_pipeline();
    current_track = idx;

    /* Build pipeline */
    pipeline = gst_element_factory_make("playbin", "player");
    if (!pipeline) {
        g_printerr("Could not create playbin element\n");
        return;
    }

    GError *err = NULL;
    gchar *uri = gst_filename_to_uri(tracks[idx].filepath, &err);
    if (!uri) { g_clear_error(&err); stop_pipeline(); return; }
    g_object_set(pipeline, "uri", uri, NULL);
    g_free(uri);

    /* Volume */
    double vol = gtk_range_get_value(GTK_RANGE(volume_slider)) / 100.0;
    g_object_set(pipeline, "volume", vol, NULL);

    /* Bus */
    GstBus *bus = gst_element_get_bus(pipeline);
    bus_watch_id = gst_bus_add_watch(bus, bus_callback, NULL);
    gst_object_unref(bus);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    is_playing = TRUE;
    gtk_button_set_label(GTK_BUTTON(btn_play), "⏸ Pause");

    /* Update UI */
    TrackInfo *t = &tracks[idx];
    gchar *ticker = g_strdup_printf("%s  —  %s",
                                    safe(t->artist), safe(t->title));
    ticker_set(ticker);
    g_free(ticker);

    update_metadata_panel(idx);
    gtk_range_set_value(GTK_RANGE(seek_bar), 0.0);
}

/* ──────────────────────────────────────────────────────────────
   Button callbacks
   ────────────────────────────────────────────────────────────── */

static void on_play_pause(GtkButton *btn, gpointer data) {
    (void)btn; (void)data;

    if (!pipeline) {
        /* Nothing loaded — start first track */
        if (track_count > 0) play_track(0);
        return;
    }

    if (is_playing) {
        gst_element_set_state(pipeline, GST_STATE_PAUSED);
        is_playing = FALSE;
        gtk_button_set_label(GTK_BUTTON(btn_play), "▶ Play");
    } else {
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        is_playing = TRUE;
        gtk_button_set_label(GTK_BUTTON(btn_play), "⏸ Pause");
    }
}

static void on_stop(GtkButton *btn, gpointer data) {
    (void)btn; (void)data;
    stop_pipeline();
    gtk_button_set_label(GTK_BUTTON(btn_play), "▶ Play");
    gtk_label_set_text(GTK_LABEL(time_label), "0:00 / 0:00");
    gtk_range_set_value(GTK_RANGE(seek_bar), 0.0);
    ticker_set("BINAMP 98  —  READY");
}

static void on_prev(GtkButton *btn, gpointer data) {
    (void)btn; (void)data;
    if (current_track > 0) play_track(current_track - 1);
}

static void on_next(GtkButton *btn, gpointer data) {
    (void)btn; (void)data;
    if (current_track + 1 < track_count) play_track(current_track + 1);
}

static void on_seek(GtkRange *range, gpointer data) {
    (void)data;
    if (!pipeline) return;
    double pct = gtk_range_get_value(range) / 100.0;
    gint64 dur = 0;
    if (gst_element_query_duration(pipeline, GST_FORMAT_TIME, &dur) && dur > 0)
        gst_element_seek_simple(pipeline, GST_FORMAT_TIME,
            GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
            (gint64)(pct * dur));
}

static void on_volume(GtkRange *range, gpointer data) {
    (void)data;
    if (pipeline)
        g_object_set(pipeline, "volume",
                     gtk_range_get_value(range) / 100.0, NULL);
}

/* ──────────────────────────────────────────────────────────────
   UI construction helpers
   ────────────────────────────────────────────────────────────── */

/* Create a styled GTK label pair (key + value) and add to grid */
static GtkWidget *meta_val_label(void) {
    GtkWidget *lbl = gtk_label_new("N/A");
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
    gtk_widget_get_style_context(lbl);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(lbl), "meta-val");
    gtk_label_set_max_width_chars(GTK_LABEL(lbl), 30);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    return lbl;
}

static GtkWidget *meta_key_label(const char *text) {
    GtkWidget *lbl = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(lbl), "meta-key");
    return lbl;
}

static GtkWidget *transport_button(const char *label) {
    GtkWidget *btn = gtk_button_new_with_label(label);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(btn), "transport-btn");
    return btn;
}

static void add_playlist_entries(GtkWidget *box) {
    for (int i = 0; i < track_count; i++) {
        const gchar *name = tracks[i].title ? tracks[i].title
                                            : tracks[i].filepath;
        /* Truncate long names */
        gchar *label = g_strndup(name, 40);
        GtkWidget *btn = gtk_button_new_with_label(label);
        g_free(label);
        gtk_style_context_add_class(
            gtk_widget_get_style_context(btn), "playlist-item");
        gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
        g_signal_connect(btn, "clicked",
                         G_CALLBACK(play_track),
                         GINT_TO_POINTER(i));
        gtk_box_pack_start(GTK_BOX(box), btn, FALSE, FALSE, 0);
    }
}

/* ──────────────────────────────────────────────────────────────
   main
   ────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    gst_init(&argc, &argv);
    gtk_init(&argc, &argv);

    /* Apply CSS */
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, BINAMP_CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    /* Scan audio */
    scan_audio_dir();

    /* ── Main window ── */
    win_main = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win_main), "Binamp 98");
    gtk_window_set_default_size(GTK_WINDOW(win_main), 340, 580);
    gtk_window_set_resizable(GTK_WINDOW(win_main), FALSE);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(win_main), "binamp");
    g_signal_connect(win_main, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(win_main), root);

    /* ── Branding strip ── */
    GtkWidget *brand = gtk_label_new("BINAMP 98");
    gtk_style_context_add_class(
        gtk_widget_get_style_context(brand), "led-display");
    gtk_label_set_xalign(GTK_LABEL(brand), 0.5);
    gtk_box_pack_start(GTK_BOX(root), brand, FALSE, FALSE, 0);

    /* ── LED ticker display ── */
    display_label = gtk_label_new("BINAMP 98  —  READY");
    gtk_style_context_add_class(
        gtk_widget_get_style_context(display_label), "led-display");
    gtk_label_set_xalign(GTK_LABEL(display_label), 0.0);
    gtk_label_set_single_line_mode(GTK_LABEL(display_label), TRUE);
    gtk_box_pack_start(GTK_BOX(root), display_label, FALSE, FALSE, 0);

    /* ── Time label ── */
    time_label = gtk_label_new("0:00 / 0:00");
    gtk_style_context_add_class(
        gtk_widget_get_style_context(time_label), "led-display");
    gtk_label_set_xalign(GTK_LABEL(time_label), 1.0);
    gtk_widget_set_margin_end(time_label, 6);
    gtk_box_pack_start(GTK_BOX(root), time_label, FALSE, FALSE, 0);

    /* ── Seek bar ── */
    seek_bar = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
                                        0.0, 100.0, 0.1);
    gtk_scale_set_draw_value(GTK_SCALE(seek_bar), FALSE);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(seek_bar), "binamp-scale");
    g_signal_connect(seek_bar, "value-changed",
                     G_CALLBACK(on_seek), NULL);
    gtk_box_pack_start(GTK_BOX(root), seek_bar, FALSE, FALSE, 0);

    /* ── Transport controls ── */
    GtkWidget *transport = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start (transport, 6);
    gtk_widget_set_margin_end   (transport, 6);
    gtk_widget_set_margin_top   (transport, 4);
    gtk_widget_set_margin_bottom(transport, 4);
    gtk_box_pack_start(GTK_BOX(root), transport, FALSE, FALSE, 0);

    GtkWidget *b_prev = transport_button("|◀ Prev");
    g_signal_connect(b_prev, "clicked", G_CALLBACK(on_prev), NULL);
    gtk_box_pack_start(GTK_BOX(transport), b_prev, TRUE, TRUE, 0);

    btn_play = transport_button("▶ Play");
    g_signal_connect(btn_play, "clicked", G_CALLBACK(on_play_pause), NULL);
    gtk_box_pack_start(GTK_BOX(transport), btn_play, TRUE, TRUE, 0);

    GtkWidget *b_stop = transport_button("■ Stop");
    g_signal_connect(b_stop, "clicked", G_CALLBACK(on_stop), NULL);
    gtk_box_pack_start(GTK_BOX(transport), b_stop, TRUE, TRUE, 0);

    GtkWidget *b_next = transport_button("Next ▶|");
    g_signal_connect(b_next, "clicked", G_CALLBACK(on_next), NULL);
    gtk_box_pack_start(GTK_BOX(transport), b_next, TRUE, TRUE, 0);

    /* ── Volume ── */
    GtkWidget *vol_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start(vol_box, 6);
    gtk_widget_set_margin_end  (vol_box, 6);
    gtk_box_pack_start(GTK_BOX(root), vol_box, FALSE, FALSE, 0);

    GtkWidget *vol_lbl = gtk_label_new("Vol");
    gtk_style_context_add_class(
        gtk_widget_get_style_context(vol_lbl), "meta-key");
    gtk_box_pack_start(GTK_BOX(vol_box), vol_lbl, FALSE, FALSE, 0);

    volume_slider = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
                                              0.0, 100.0, 1.0);
    gtk_scale_set_draw_value(GTK_SCALE(volume_slider), FALSE);
    gtk_range_set_value(GTK_RANGE(volume_slider), 80.0);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(volume_slider), "binamp-scale");
    g_signal_connect(volume_slider, "value-changed",
                     G_CALLBACK(on_volume), NULL);
    gtk_box_pack_start(GTK_BOX(vol_box), volume_slider, TRUE, TRUE, 0);

    /* ── Metadata panel ── */
    GtkWidget *meta_lbl = gtk_label_new("FILE INFO");
    gtk_style_context_add_class(
        gtk_widget_get_style_context(meta_lbl), "section-label");
    gtk_label_set_xalign(GTK_LABEL(meta_lbl), 0.0);
    gtk_box_pack_start(GTK_BOX(root), meta_lbl, FALSE, FALSE, 0);

    GtkWidget *meta_grid = gtk_grid_new();
    gtk_grid_set_row_spacing   (GTK_GRID(meta_grid), 2);
    gtk_grid_set_column_spacing(GTK_GRID(meta_grid), 8);
    gtk_widget_set_margin_start (meta_grid, 8);
    gtk_widget_set_margin_end   (meta_grid, 8);
    gtk_widget_set_margin_top   (meta_grid, 4);
    gtk_widget_set_margin_bottom(meta_grid, 4);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(meta_grid), "meta-panel");
    gtk_box_pack_start(GTK_BOX(root), meta_grid, FALSE, FALSE, 0);

    /* Row: key, val */
#define META_ROW(row, key_text, val_ptr) \
    gtk_grid_attach(GTK_GRID(meta_grid), meta_key_label(key_text), 0, row, 1, 1); \
    (val_ptr) = meta_val_label(); \
    gtk_grid_attach(GTK_GRID(meta_grid), (val_ptr), 1, row, 1, 1);

    META_ROW(0, "Title:",    val_title)
    META_ROW(1, "Subtitle:", val_subtitle)
    META_ROW(2, "Artist:",   val_artist)
    META_ROW(3, "Album:",    val_album)
    META_ROW(4, "Year:",     val_year)
    META_ROW(5, "Track #:",  val_track)
    META_ROW(6, "Genre:",    val_genre)
    META_ROW(7, "Length:",   val_length)
    META_ROW(8, "Rating:",   val_rating)
#undef META_ROW

    /* ── Playlist ── */
    GtkWidget *pl_lbl = gtk_label_new("PLAYLIST");
    gtk_style_context_add_class(
        gtk_widget_get_style_context(pl_lbl), "section-label");
    gtk_label_set_xalign(GTK_LABEL(pl_lbl), 0.0);
    gtk_box_pack_start(GTK_BOX(root), pl_lbl, FALSE, FALSE, 0);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, 110);
    gtk_box_pack_start(GTK_BOX(root), scroll, TRUE, TRUE, 0);

    GtkWidget *pl_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(pl_box), "playlist");
    gtk_container_add(GTK_CONTAINER(scroll), pl_box);

    if (track_count > 0)
        add_playlist_entries(pl_box);
    else {
        GtkWidget *empty = gtk_label_new("  No files in " AUDIO_DIR);
        gtk_style_context_add_class(
            gtk_widget_get_style_context(empty), "meta-val");
        gtk_box_pack_start(GTK_BOX(pl_box), empty, FALSE, FALSE, 4);
    }

    /* ── Timers ── */
    ticker_timer = g_timeout_add(280, ticker_tick, NULL);
    pos_timer    = g_timeout_add(500, update_position, NULL);

    ticker_set("BINAMP 98  —  READY  —  MICHAELSOFT BINBOWS");

    gtk_widget_show_all(win_main);
    gtk_main();

    /* Cleanup */
    if (ticker_timer) g_source_remove(ticker_timer);
    if (pos_timer)    g_source_remove(pos_timer);
    stop_pipeline();

    for (int i = 0; i < track_count; i++) {
        g_free(tracks[i].filepath);
        g_free(tracks[i].title);
        g_free(tracks[i].subtitle);
        g_free(tracks[i].artist);
        g_free(tracks[i].album);
        g_free(tracks[i].genre);
    }

    return 0;
}
