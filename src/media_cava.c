// waybar CFFI plugin: combined MPRIS media + live cava audio visualiser — a
// compact top-bar media widget.
//
// Layout (left → right):
//   [6-bar visualiser] [title • artist] [prev] [play/pause] [next]
//
//  - Visualiser: spawns `cava` (raw ascii) and draws 6 rounded bars, vertically
//    centred/mirrored, height mapped with a sqrt curve.
//    The bar colour follows the drawing area's CSS `color` (matugen @primary).
//  - Media info + art URL come from `playerctl --follow`; controls dispatch
//    playerctl previous / play-pause / next. When no player exists the widget
//    hides and waybar collapses it.
//  - Optional album art (art-size > 0) is shown before the visualiser (off by
//    default — the bar shows none).
#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <glib/gstdio.h>
#include <math.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "waybar_cffi_module.h"
const size_t wbcffi_version = 1;

#define MAXBARS 64

// Nerd-Font (Material Design) media glyphs — same font the rest of the bar uses.
#define GLYPH_PREV  "\xf3\xb0\x92\xae"   // 󰒮 skip_previous
#define GLYPH_NEXT  "\xf3\xb0\x92\xad"   // 󰒭 skip_next
#define GLYPH_PLAY  "\xf3\xb0\x90\x8a"   // 󰐊 play
#define GLYPH_PAUSE "\xf3\xb0\x8f\xa4"   // 󰏤 pause
#define GLYPH_NOTE  "\xf3\xb0\x8e\x88"   // 󰎈 music note

typedef struct {
  GtkWidget *box;          // outer horizontal container (CSS #media-cava)
  GtkWidget *art;          // optional album-art GtkImage (CSS .media-art)
  GtkWidget *draw;         // cava visualiser GtkDrawingArea (CSS .cava)
  GtkWidget *label;        // "title • artist" (CSS .media-title)
  GtkWidget *btn_prev, *btn_play, *btn_next;   // controls (CSS .media-btn ...)
  GtkWidget *lbl_play;     // glyph inside btn_play (toggles play/pause)

  // visualiser geometry (px, already scaled for this bar)
  int bars, bar_width, bar_gap, bar_min, viz_height, mirror;
  double levels[MAXBARS];  // eased, 0..1 (drawn)
  double target[MAXBARS];  // latest from cava, 0..1
  double smooth;           // easing factor / frame
  guint anim_id;           // 60fps easing timer (0 = stopped)

  GPid cava_pid, pctl_pid;
  char *cava_cfg;
  GCancellable *cancel;          // cancels pending async reads before teardown

  gboolean have_player, playing;
  char *art_url;
  int art_size, max_length, show_controls;
} Instance;

// ─── rounded-rectangle cairo path ────────────────────────────────────────────
static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
  if (r > w / 2) r = w / 2;
  if (r > h / 2) r = h / 2;
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
  cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
  cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
  cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
  cairo_close_path(cr);
}

// ─── draw the visualiser ─────────────────────────────────────────────────────
static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer data) {
  Instance *self = data;
  GtkAllocation a;
  gtk_widget_get_allocation(w, &a);

  GtkStyleContext *ctx = gtk_widget_get_style_context(w);
  GdkRGBA col;
  gtk_style_context_get_color(ctx, gtk_style_context_get_state(ctx), &col);

  double bw = self->bar_width, gap = self->bar_gap;
  double total = self->bars * bw + (self->bars - 1) * gap;
  double x0 = (a.width - total) / 2.0;   // centre the bar row horizontally
  if (x0 < 0) x0 = 0;
  double cy = a.height / 2.0;
  double radius = bw / 2.0;

  for (int i = 0; i < self->bars; i++) {
    double lvl = self->levels[i];
    if (lvl < 0) lvl = 0; else if (lvl > 1) lvl = 1;
    double len = self->bar_min + lvl * (self->viz_height - self->bar_min);
    double x = x0 + i * (bw + gap);
    double y = self->mirror ? cy - len / 2.0 : a.height - len;
    cairo_set_source_rgba(cr, col.red, col.green, col.blue, col.alpha);
    rounded_rect(cr, x, y, bw, len, radius);
    cairo_fill(cr);
  }
  return FALSE;
}

// ─── 60fps easing toward the latest cava frame ───────────────────────────────
static gboolean tick(gpointer data) {
  Instance *self = data;
  double maxlvl = 0;
  for (int i = 0; i < self->bars; i++) {
    self->levels[i] += (self->target[i] - self->levels[i]) * self->smooth;
    if (self->levels[i] > maxlvl) maxlvl = self->levels[i];
  }
  gtk_widget_queue_draw(self->draw);
  if (!self->playing && maxlvl < 0.01) {        // idle + settled → stop animating
    for (int i = 0; i < self->bars; i++) self->levels[i] = self->target[i] = 0;
    gtk_widget_queue_draw(self->draw);
    self->anim_id = 0;
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static void ensure_anim(Instance *self) {
  if (!self->anim_id) self->anim_id = g_timeout_add(16, tick, self);
}

// ─── cava: parse a raw ascii line "v0;v1;...;" (0..1000), sqrt curve ──────────
static void cava_line(Instance *self, const char *line) {
  const char *p = line;
  int i = 0;
  while (*p && i < self->bars) {
    char *end;
    long v = strtol(p, &end, 10);
    if (end == p) break;
    double n = v / 1000.0;
    if (n < 0) n = 0; else if (n > 1) n = 1;
    n = sqrt(n);                                 // emphasise low levels
    self->target[i++] = self->playing ? n : 0.0;
    p = end;
    while (*p == ';' || *p == ' ') p++;
  }
  ensure_anim(self);
}

// ─── async line readers (cava + playerctl), teardown-safe ────────────────────
// Each reader owns a ref to the instance's GCancellable, so it stays valid even
// after `self` is freed on teardown. The callback checks cancellation BEFORE ever
// touching `self` — this is what fixes the use-after-free crash on waybar reload
// (matugen's SIGUSR2 on wallpaper change tears every module down and back up).
static void pctl_handle(Instance *self, char *line);   // defined below

typedef struct {
  Instance *self;
  GCancellable *cancel;
  GDataInputStream *in;
  gboolean pctl;
} Reader;

static void reader_done(Reader *r) {
  g_object_unref(r->cancel);
  g_object_unref(r->in);
  g_free(r);
}
static void reader_cb(GObject *src, GAsyncResult *res, gpointer data);
static void reader_next(Reader *r) {
  g_data_input_stream_read_line_async(r->in,
      r->pctl ? G_PRIORITY_DEFAULT : G_PRIORITY_DEFAULT_IDLE, r->cancel, reader_cb, r);
}
static void reader_cb(GObject *src, GAsyncResult *res, gpointer data) {
  Reader *r = data;
  gsize len = 0;
  char *line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(src), res, &len, NULL);
  // Bail before touching self if we're being torn down (self may be freed) or the
  // pipe closed. NB: a frame may have arrived just before cancel, so check the
  // cancellable explicitly rather than trusting a non-NULL line.
  if (g_cancellable_is_cancelled(r->cancel) || !line) { g_free(line); reader_done(r); return; }
  if (r->pctl) pctl_handle(r->self, line);       // handles empty line = no player
  else if (len > 0) cava_line(r->self, line);
  g_free(line);
  reader_next(r);
}
static void reader_start(Instance *self, GDataInputStream *in, gboolean pctl) {
  if (!in) return;
  Reader *r = g_new0(Reader, 1);
  r->self = self;
  r->cancel = g_object_ref(self->cancel);
  r->in = in;                                    // takes ownership
  r->pctl = pctl;
  reader_next(r);
}

// ─── album art (optional) ────────────────────────────────────────────────────
static GdkPixbuf *round_pixbuf(GdkPixbuf *src, int size, double radius) {
  cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
  cairo_t *cr = cairo_create(surf);
  rounded_rect(cr, 0, 0, size, size, radius);
  cairo_clip(cr);
  gdk_cairo_set_source_pixbuf(cr, src, 0, 0);
  cairo_paint(cr);
  cairo_destroy(cr);
  GdkPixbuf *out = gdk_pixbuf_get_from_surface(surf, 0, 0, size, size);
  cairo_surface_destroy(surf);
  return out;
}

static void set_art(Instance *self) {
  if (!self->art) return;
  char *path = NULL;
  const char *url = self->art_url;
  if (url && g_str_has_prefix(url, "file://")) path = g_filename_from_uri(url, NULL, NULL);
  else if (url && *url == '/') path = g_strdup(url);
  if (path) {
    GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(path, self->art_size, self->art_size, TRUE, NULL);
    g_free(path);
    if (pb) {
      GdkPixbuf *rp = round_pixbuf(pb, self->art_size, self->art_size * 0.22);
      gtk_image_set_from_pixbuf(GTK_IMAGE(self->art), rp);
      g_object_unref(rp); g_object_unref(pb);
      gtk_widget_show(self->art);
      return;
    }
  }
  gtk_widget_hide(self->art);
}

// ─── media state from playerctl ──────────────────────────────────────────────
static void update_media(Instance *self, const char *status, const char *title,
                         const char *artist, const char *art) {
  self->have_player = (status && *status);
  self->playing = (status && strcmp(status, "Playing") == 0);

  if (!self->have_player) { gtk_widget_hide(self->box); return; }
  gtk_widget_show(self->box);

  // title • artist; fall back to title / placeholder
  char *label;
  if (title && *title && artist && *artist)
    label = g_strdup_printf("%s \xe2\x80\xa2 %s", title, artist);
  else
    label = g_strdup((title && *title) ? title : "\xe2\x80\x94");
  gtk_label_set_text(GTK_LABEL(self->label), label);
  g_free(label);

  // play/pause glyph + a .playing class so CSS can fill the button with @primary
  if (self->lbl_play)
    gtk_label_set_text(GTK_LABEL(self->lbl_play), self->playing ? GLYPH_PAUSE : GLYPH_PLAY);
  if (self->btn_play) {
    GtkStyleContext *pc = gtk_widget_get_style_context(self->btn_play);
    if (self->playing) gtk_style_context_add_class(pc, "playing");
    else gtk_style_context_remove_class(pc, "playing");
  }

  g_free(self->art_url);
  self->art_url = g_strdup(art ? art : "");
  set_art(self);

  if (self->playing) ensure_anim(self);
}

// playerctl line: "status \t title \t artist \t artUrl"; empty line = no player.
static void pctl_handle(Instance *self, char *line) {
  if (!*line) { update_media(self, "", "", "", ""); return; }
  char *f[4] = {0}, *save = NULL, *tok = strtok_r(line, "\t", &save);
  for (int i = 0; i < 4 && tok; i++) { f[i] = tok; tok = strtok_r(NULL, "\t", &save); }
  update_media(self, f[0] ? f[0] : "", f[1] ? f[1] : "", f[2] ? f[2] : "", f[3] ? f[3] : "");
}

// ─── control clicks → playerctl ──────────────────────────────────────────────
static gboolean on_ctrl(GtkWidget *w, GdkEventButton *ev, gpointer data) {
  (void)w;
  if (ev->button != 1) return FALSE;
  const char *cmd = data;
  char *argv[] = {"playerctl", (char *)cmd, NULL};
  g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
  return TRUE;
}

// A circular control button: GtkEventBox (keeps the compositor cursor; ignores CSS
// box-model, so the round shape is sized here) + a centred glyph label.
static GtkWidget *make_ctrl(const char *glyph, const char *cls, const char *cmd,
                            int size, GtkWidget **glyph_out) {
  GtkWidget *b = gtk_event_box_new();
  gtk_widget_add_events(b, GDK_BUTTON_PRESS_MASK);
  gtk_widget_set_size_request(b, size, size);
  gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
  GtkStyleContext *c = gtk_widget_get_style_context(b);
  gtk_style_context_add_class(c, "media-btn");
  gtk_style_context_add_class(c, cls);
  GtkWidget *l = gtk_label_new(glyph);
  gtk_widget_set_halign(l, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(l, GTK_ALIGN_CENTER);
  gtk_container_add(GTK_CONTAINER(b), l);
  g_signal_connect(b, "button-press-event", G_CALLBACK(on_ctrl), (gpointer)cmd);
  if (glyph_out) *glyph_out = l;
  return b;
}

// ─── cava config ─────────────────────────────────────────────────────────────
static void write_cava_cfg(Instance *self) {
  const char *rt = g_getenv("XDG_RUNTIME_DIR");
  if (!rt || !*rt) rt = "/tmp";
  self->cava_cfg = g_strdup_printf("%s/waybar-media-cava.conf", rt);

  char *sink = NULL;
  g_spawn_command_line_sync("pactl get-default-sink", &sink, NULL, NULL, NULL);
  if (sink) g_strchomp(sink);
  char *source = (sink && *sink) ? g_strdup_printf("%s.monitor", sink) : g_strdup("auto");
  g_free(sink);

  // autosens=1 auto-gains so quiet sources
  // (e.g. speech podcasts) still fill the bars instead of sitting near the floor.
  char *cfg = g_strdup_printf(
    "[general]\nbars = %d\nframerate = 30\nautosens = 1\nsensitivity = 100\n"
    "lower_cutoff_freq = 50\nhigher_cutoff_freq = 12000\n"
    "[input]\nmethod = pipewire\nsource = %s\n"
    "[output]\nmethod = raw\nraw_target = /dev/stdout\ndata_format = ascii\n"
    "ascii_max_range = 1000\nchannels = mono\n"
    "[smoothing]\nnoise_reduction = 35\n",
    self->bars, source);
  g_file_set_contents(self->cava_cfg, cfg, -1, NULL);
  g_free(cfg); g_free(source);
}

static GDataInputStream *spawn_reader(char **argv, GPid *pid_out) {
  int out_fd = -1;
  GError *err = NULL;
  if (!g_spawn_async_with_pipes(NULL, argv, NULL,
        G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL,
        pid_out, NULL, &out_fd, NULL, &err)) {
    if (err) { g_warning("media-cava: spawn failed: %s", err->message); g_error_free(err); }
    return NULL;
  }
  GInputStream *is = g_unix_input_stream_new(out_fd, TRUE);
  return g_data_input_stream_new(is);
}

// ─── CFFI entry points ───────────────────────────────────────────────────────
void *wbcffi_init(const wbcffi_init_info *info,
                  const wbcffi_config_entry *entries, size_t entries_len) {
  Instance *self = g_new0(Instance, 1);
  // Default geometry (20x20 slot, 6 bars) pre-scaled ~1.75x for this bar.
  self->bars = 6;
  self->bar_width = 4;
  self->bar_gap = 3;
  self->bar_min = 5;
  self->viz_height = 34;
  self->mirror = 1;
  self->smooth = 0.35;
  self->art_size = 0;          // no album art in the bar
  self->max_length = 40;
  self->show_controls = 1;

  for (size_t i = 0; i < entries_len; i++) {
    const char *k = entries[i].key, *v = entries[i].value;
    if (!strcmp(k, "bars")) self->bars = CLAMP(atoi(v), 1, MAXBARS);
    else if (!strcmp(k, "bar-width")) self->bar_width = atoi(v);
    else if (!strcmp(k, "bar-gap")) self->bar_gap = atoi(v);
    else if (!strcmp(k, "bar-min")) self->bar_min = atoi(v);
    else if (!strcmp(k, "viz-height")) self->viz_height = atoi(v);
    else if (!strcmp(k, "mirror")) self->mirror = atoi(v);
    else if (!strcmp(k, "smoothing")) self->smooth = atof(v);
    else if (!strcmp(k, "art-size")) self->art_size = atoi(v);
    else if (!strcmp(k, "max-length")) self->max_length = atoi(v);
    else if (!strcmp(k, "controls")) self->show_controls = atoi(v);
  }
  if (self->smooth <= 0 || self->smooth > 1) self->smooth = 0.35;
  self->cancel = g_cancellable_new();

  GtkContainer *root = info->get_root_widget(info->obj);
  self->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_name(self->box, "media-cava");
  gtk_widget_set_valign(self->box, GTK_ALIGN_CENTER);

  if (self->art_size > 0) {
    self->art = gtk_image_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(self->art), "media-art");
    gtk_widget_set_valign(self->art, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(self->box), self->art, FALSE, FALSE, 0);
  }

  self->draw = gtk_drawing_area_new();
  gtk_style_context_add_class(gtk_widget_get_style_context(self->draw), "cava");
  int vw = self->bars * self->bar_width + (self->bars - 1) * self->bar_gap;
  gtk_widget_set_size_request(self->draw, vw, self->viz_height);
  gtk_widget_set_valign(self->draw, GTK_ALIGN_CENTER);
  g_signal_connect(self->draw, "draw", G_CALLBACK(on_draw), self);
  gtk_box_pack_start(GTK_BOX(self->box), self->draw, FALSE, FALSE, 0);

  self->label = gtk_label_new("");
  gtk_style_context_add_class(gtk_widget_get_style_context(self->label), "media-title");
  gtk_label_set_ellipsize(GTK_LABEL(self->label), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars(GTK_LABEL(self->label), self->max_length);
  gtk_widget_set_valign(self->label, GTK_ALIGN_CENTER);
  gtk_box_pack_start(GTK_BOX(self->box), self->label, FALSE, FALSE, 0);

  if (self->show_controls) {
    // prev/next 20px circles, play/pause 24px — pre-scaled ~1.75x for this bar.
    self->btn_prev = make_ctrl(GLYPH_PREV, "media-prev", "previous", 34, NULL);
    self->btn_play = make_ctrl(GLYPH_PLAY, "media-play", "play-pause", 42, &self->lbl_play);
    self->btn_next = make_ctrl(GLYPH_NEXT, "media-next", "next", 34, NULL);
    gtk_box_pack_start(GTK_BOX(self->box), self->btn_prev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(self->box), self->btn_play, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(self->box), self->btn_next, FALSE, FALSE, 0);
  }

  gtk_container_add(root, self->box);
  gtk_widget_show_all(GTK_WIDGET(root));
  gtk_widget_hide(self->box);           // until a player appears

  write_cava_cfg(self);
  char *cava_argv[] = {"cava", "-p", self->cava_cfg, NULL};
  reader_start(self, spawn_reader(cava_argv, &self->cava_pid), FALSE);

  char *pctl_argv[] = {"playerctl", "--follow", "--format",
    "{{status}}\t{{title}}\t{{artist}}\t{{mpris:artUrl}}", "metadata", NULL};
  reader_start(self, spawn_reader(pctl_argv, &self->pctl_pid), TRUE);

  return self;
}

void wbcffi_deinit(void *instance) {
  Instance *self = instance;
  // Cancel pending async reads FIRST: their callbacks check the NULL result and
  // bail before touching `self`. Order matters — killing the children closes
  // their pipes, which would otherwise complete a still-armed read into freed
  // memory (the wallpaper-change / reload crash).
  if (self->cancel) g_cancellable_cancel(self->cancel);   // readers bail + self-free
  if (self->anim_id) g_source_remove(self->anim_id);
  // SIGTERM then reap — spawned with DO_NOT_REAP_CHILD, so we must waitpid or the
  // killed child lingers as a zombie (one per reload otherwise).
  if (self->cava_pid) { kill(self->cava_pid, SIGTERM); waitpid(self->cava_pid, NULL, 0); g_spawn_close_pid(self->cava_pid); }
  if (self->pctl_pid) { kill(self->pctl_pid, SIGTERM); waitpid(self->pctl_pid, NULL, 0); g_spawn_close_pid(self->pctl_pid); }
  g_clear_object(&self->cancel);
  if (self->cava_cfg) { g_unlink(self->cava_cfg); g_free(self->cava_cfg); }
  g_free(self->art_url);
  g_free(self);
}
