PLUGIN  := libmedia_cava.so
PKGS    := gtk+-3.0 gio-2.0 gio-unix-2.0 gtk-layer-shell-0
WBCOMMON ?= common
CFLAGS  ?= -O2 -Wall -Wextra
CFLAGS  += -fPIC -I$(WBCOMMON) $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS)) -lm
PREFIX  ?= $(HOME)/.local/lib/waybar
DATADIR ?= $(HOME)/.local/share/waybar-media-cava

$(PLUGIN): src/media_cava.c $(WBCOMMON)/wbcommon.h
	$(CC) $(CFLAGS) -shared -o $@ $< $(LDLIBS)

install: $(PLUGIN)
	install -Dm755 $(PLUGIN) $(PREFIX)/$(PLUGIN)
	install -Dm644 -t $(DATADIR) assets/prev.svg assets/play.svg assets/pause.svg assets/next.svg
	@echo "installed to $(PREFIX)/$(PLUGIN) + icons in $(DATADIR)"

test_media_cava: tests/test_media_cava.c src/media_cava.c $(WBCOMMON)/wbcommon.h
	$(CC) $(CFLAGS) -o $@ tests/test_media_cava.c $(LDLIBS)

test: test_media_cava
	./test_media_cava

clean:
	rm -f $(PLUGIN) test_media_cava

.PHONY: install clean test
