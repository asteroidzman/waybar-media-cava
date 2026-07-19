// Unit tests for media_cava.c's cava ascii-line parsing (cava_line) -- no
// GTK init, no Wayland, no live compositor, no actual cava process.
// #includes the plugin source directly to reach its `static` functions
// without changing their visibility for production code; this file
// supplies its own main() (media_cava.c has none), so nothing conflicts.
//
// cava_line's only side effects are self->target[] (a plain double array)
// and scheduling a GLib timeout via ensure_anim -- g_timeout_add just
// registers a source on the default GMainContext (which GLib creates
// automatically) and returns immediately; nothing fires unless a main loop
// actually runs one, which this test never does, so it's safe to call
// without gtk_init. update_media/pctl_handle aren't testable this way --
// update_media unconditionally touches real GTK widgets (gtk_widget_hide,
// gtk_label_set_text, ...).
#include "../src/media_cava.c"
#include <math.h>
#include <stdio.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
	if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
	else { printf("ok - %s\n", msg); } \
} while (0)
#define CHECK_DOUBLE_EQ(a, b, msg) CHECK(((a) - (b) < 0.0001 && (b) - (a) < 0.0001), msg)

int main(void) {
	Instance inst = {0};
	inst.bars = 3;
	inst.playing = TRUE;

	cava_line(&inst, "0;500;1000;");
	CHECK_DOUBLE_EQ(inst.target[0], 0.0, "cava_line: raw 0 -> sqrt curve gives level 0");
	CHECK_DOUBLE_EQ(inst.target[1], sqrt(0.5), "cava_line: raw 500/1000 -> sqrt(0.5) level");
	CHECK_DOUBLE_EQ(inst.target[2], 1.0, "cava_line: raw 1000/1000 -> sqrt curve gives level 1");
	CHECK(inst.anim_id != 0, "cava_line schedules the easing animation (ensure_anim)");
	g_source_remove(inst.anim_id);

	Instance idle = {0};
	idle.bars = 3;
	idle.playing = FALSE;   // not playing -> every parsed level is forced to 0
	cava_line(&idle, "0;500;1000;");
	CHECK_DOUBLE_EQ(idle.target[0], 0.0, "cava_line while not playing: level 0 forced to 0");
	CHECK_DOUBLE_EQ(idle.target[1], 0.0, "cava_line while not playing: level 500 forced to 0");
	CHECK_DOUBLE_EQ(idle.target[2], 0.0, "cava_line while not playing: level 1000 forced to 0");
	if (idle.anim_id) g_source_remove(idle.anim_id);

	Instance clamp = {0};
	clamp.bars = 2;
	clamp.playing = TRUE;
	cava_line(&clamp, "-100;2000;");   // out-of-spec values still get clamped to [0,1000] first
	CHECK_DOUBLE_EQ(clamp.target[0], 0.0, "cava_line clamps a negative raw value to 0");
	CHECK_DOUBLE_EQ(clamp.target[1], 1.0, "cava_line clamps a >1000 raw value to level 1");
	if (clamp.anim_id) g_source_remove(clamp.anim_id);

	Instance partial = {0};
	partial.bars = 4;
	partial.playing = TRUE;
	partial.target[2] = 0.42; partial.target[3] = 0.99;  // pre-existing values
	cava_line(&partial, "0;1000;");   // fewer values than bars: only fills what's given
	CHECK_DOUBLE_EQ(partial.target[0], 0.0, "cava_line partial line: first value fills index 0");
	CHECK_DOUBLE_EQ(partial.target[1], 1.0, "cava_line partial line: second value fills index 1");
	CHECK_DOUBLE_EQ(partial.target[2], 0.42, "cava_line partial line: untouched index keeps its prior value");
	CHECK_DOUBLE_EQ(partial.target[3], 0.99, "cava_line partial line: untouched index keeps its prior value (2)");
	if (partial.anim_id) g_source_remove(partial.anim_id);

	printf("----\n%d failure(s)\n", failures);
	return failures ? 1 : 0;
}
