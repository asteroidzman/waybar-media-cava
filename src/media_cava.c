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
#include "wbcommon.h"
#include <glib/gstdio.h>
#include <math.h>
#include <signal.h>
#include <sys/prctl.h>
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
  int framerate;           // redraw + cava output rate (fps); caps GPU cost
  double levels[MAXBARS];  // eased, 0..1 (drawn)
  double target[MAXBARS];  // latest from cava, 0..1
  double smooth;           // easing factor / frame
  guint anim_id;           // easing/redraw timer at `framerate` fps (0 = stopped)

  WbReader *cava_rdr, *pctl_rdr;
  char *cava_cfg;

  gboolean have_player, playing;
  char *art_url;
  int art_size, max_length, show_controls;
  char *icon_dir; int icon_size;
} Instance;

// Load a control icon SVG and recolour the silhouette to the widget's theme colour.
static GdkPixbuf *themed_pixbuf(GtkWidget *w, const char *dir, int size, const char *name) {
  char *p = g_build_filename(dir, name, NULL);
  GdkPixbuf *src = gdk_pixbuf_new_from_file_at_size(p, size, size, NULL);
  g_free(p);
  if (!src) return NULL;
  GdkPixbuf *d = gdk_pixbuf_get_has_alpha(src) ? gdk_pixbuf_copy(src)
                                               : gdk_pixbuf_add_alpha(src, FALSE, 0, 0, 0);
  g_object_unref(src);
  GdkRGBA c; GtkStyleContext *sc = gtk_widget_get_style_context(w);
  gtk_style_context_get_color(sc, gtk_style_context_get_state(sc), &c);
  guchar R = (guchar)(c.red*255), G = (guchar)(c.green*255), B = (guchar)(c.blue*255);
  int wd = gdk_pixbuf_get_width(d), h = gdk_pixbuf_get_height(d);
  int rs = gdk_pixbuf_get_rowstride(d), nc = gdk_pixbuf_get_n_channels(d);
  guchar *px = gdk_pixbuf_get_pixels(d);
  for (int y = 0; y < h; y++) for (int x = 0; x < wd; x++) {
    guchar *q = px + y*rs + x*nc; q[0]=R; q[1]=G; q[2]=B;
    if (nc == 4) q[3] = (guchar)(q[3]*c.alpha);
  }
  return d;
}
static void icon_restyle(GtkWidget *img, gpointer data) {
  Instance *self = data;
  const char *name = g_object_get_data(G_OBJECT(img), "svg");
  if (!name) return;
  GdkPixbuf *pb = themed_pixbuf(img, self->icon_dir, self->icon_size, name);
  if (pb) { gtk_image_set_from_pixbuf(GTK_IMAGE(img), pb); g_object_unref(pb); }
}

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
  if (!self->anim_id) {
    // Defensive: never divide by an unset framerate. Config parse clamps it to
    // 10..60, but a directly-constructed Instance (e.g. in unit tests) may not
    // have run that path, and a /0 here would SIGFPE.
    int fps = self->framerate > 0 ? self->framerate : 30;
    self->anim_id = g_timeout_add(1000 / fps, tick, self);
  }
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

// ─── line readers (cava + playerctl) via WbReader: die with the bar
// (PDEATHSIG) and respawn with backoff if the child exits (audio-server or
// playerctl restart) — the visualiser/metadata recover on their own.
static void pctl_handle(Instance *self, char *line);   // defined below
static void on_cava_line(const char *line, gpointer user) { if (*line) cava_line(user, (char *)line); }
static void on_pctl_line(const char *line, gpointer user) { pctl_handle(user, (char *)line); }

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

  // .playing class first (so the icon recolours to @on_primary on the accent fill),
  // then swap the play/pause icon.
  if (self->btn_play) {
    GtkStyleContext *pc = gtk_widget_get_style_context(self->btn_play);
    if (self->playing) gtk_style_context_add_class(pc, "playing");
    else gtk_style_context_remove_class(pc, "playing");
  }
  if (self->lbl_play) {
    g_object_set_data_full(G_OBJECT(self->lbl_play), "svg",
                           g_strdup(self->playing ? "pause.svg" : "play.svg"), g_free);
    icon_restyle(self->lbl_play, self);
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
// box-model, so the round shape is sized here) + a centred image icon.
static GtkWidget *make_ctrl(Instance *self, const char *svg, const char *cls, const char *cmd,
                            int size, GtkWidget **img_out) {
  GtkWidget *b = gtk_event_box_new();
  gtk_widget_add_events(b, GDK_BUTTON_PRESS_MASK);
  gtk_widget_set_size_request(b, size, size);
  gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
  GtkStyleContext *c = gtk_widget_get_style_context(b);
  gtk_style_context_add_class(c, "media-btn");
  gtk_style_context_add_class(c, cls);
  GtkWidget *im = gtk_image_new();
  g_object_set_data_full(G_OBJECT(im), "svg", g_strdup(svg), g_free);
  g_signal_connect(im, "style-updated", G_CALLBACK(icon_restyle), self);
  icon_restyle(im, self);
  gtk_widget_set_halign(im, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(im, GTK_ALIGN_CENTER);
  gtk_container_add(GTK_CONTAINER(b), im);
  g_signal_connect(b, "button-press-event", G_CALLBACK(on_ctrl), (gpointer)cmd);
  if (img_out) *img_out = im;
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
    "[general]\nbars = %d\nframerate = %d\nautosens = 1\nsensitivity = 100\n"
    "lower_cutoff_freq = 50\nhigher_cutoff_freq = 12000\n"
    "[input]\nmethod = pipewire\nsource = %s\n"
    "[output]\nmethod = raw\nraw_target = /dev/stdout\ndata_format = ascii\n"
    "ascii_max_range = 1000\nchannels = mono\n"
    "[smoothing]\nnoise_reduction = 35\n",
    self->bars, self->framerate, source);
  g_file_set_contents(self->cava_cfg, cfg, -1, NULL);
  g_free(cfg); g_free(source);
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
  self->framerate = 30;        // redraw + cava rate; was a hardcoded 60fps
                               // redraw timer, the main GPU cost while playing
  self->art_size = 0;          // no album art in the bar
  self->max_length = 40;
  self->show_controls = 1;
  self->icon_size = 24;

  for (size_t i = 0; i < entries_len; i++) {
    const char *k = entries[i].key, *v = entries[i].value;
    if (!strcmp(k, "icon-size")) { self->icon_size = atoi(v); if (self->icon_size < 8) self->icon_size = 8; continue; }
    if (!strcmp(k, "icon-dir")) { g_free(self->icon_dir); self->icon_dir = g_strdup(v); continue; }
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
    else if (!strcmp(k, "framerate")) self->framerate = atoi(v);
  }
  if (self->smooth <= 0 || self->smooth > 1) self->smooth = 0.35;
  // Clamp: 60 is the old behaviour ceiling; below ~10 the easing looks steppy.
  self->framerate = CLAMP(self->framerate, 10, 60);
  // `smooth` is a per-frame factor tuned at 60fps; at a lower framerate it would
  // ease that many fewer times per second and the bars would lag the audio.
  // Re-derive it from a framerate-independent time constant so the visual decay
  // speed is identical regardless of framerate (and exactly the old value at 60).
  self->smooth = 1.0 - pow(1.0 - self->smooth, 60.0 / self->framerate);
  if (!self->icon_dir) {
    const char *dh = g_getenv("XDG_DATA_HOME");
    self->icon_dir = (dh && *dh) ? g_build_filename(dh, "waybar-media-cava", NULL)
                                 : g_build_filename(g_get_home_dir(), ".local/share/waybar-media-cava", NULL);
  }

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
    self->btn_prev = make_ctrl(self, "prev.svg", "media-prev", "previous", 34, NULL);
    self->btn_play = make_ctrl(self, "play.svg", "media-play", "play-pause", 42, &self->lbl_play);
    self->btn_next = make_ctrl(self, "next.svg", "media-next", "next", 34, NULL);
    gtk_box_pack_start(GTK_BOX(self->box), self->btn_prev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(self->box), self->btn_play, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(self->box), self->btn_next, FALSE, FALSE, 0);
  }

  gtk_container_add(root, self->box);
  gtk_widget_show_all(GTK_WIDGET(root));
  gtk_widget_hide(self->box);           // until a player appears

  write_cava_cfg(self);
  const char *cava_argv[] = {"cava", "-p", self->cava_cfg, NULL};
  self->cava_rdr = wb_reader_start(cava_argv, on_cava_line, self, G_PRIORITY_DEFAULT_IDLE);

  const char *pctl_argv[] = {"playerctl", "--follow", "--format",
    "{{status}}\t{{title}}\t{{artist}}\t{{mpris:artUrl}}", "metadata", NULL};
  self->pctl_rdr = wb_reader_start(pctl_argv, on_pctl_line, self, G_PRIORITY_DEFAULT);

  return self;
}

void wbcffi_deinit(void *instance) {
  Instance *self = instance;
  // Free the readers FIRST: they cancel pending async reads (callbacks bail
  // before touching `self`) and force-exit the children.
  wb_reader_free(self->cava_rdr);
  wb_reader_free(self->pctl_rdr);
  if (self->anim_id) g_source_remove(self->anim_id);
  if (self->cava_cfg) { g_unlink(self->cava_cfg); g_free(self->cava_cfg); }
  g_free(self->art_url);
  g_free(self->icon_dir);
  g_free(self);
}
