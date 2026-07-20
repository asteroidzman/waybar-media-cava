# waybar-media-cava

A [waybar](https://github.com/Alexays/Waybar) **CFFI plugin** that combines an
**MPRIS media widget** with a **live cava audio visualiser** in one module — a
compact top-bar media widget.

Layout, left → right:

```
[ ▍▂▆▃▅▁ ]  Title • Artist   󰒮  󰐊  󰒭
 visualiser      track       prev play next
```

- **Visualiser:** spawns the real `cava` binary (raw output) and draws 6 rounded
  bars, vertically centred/mirrored, with a `sqrt` level curve. The bar colour
  follows the widget's CSS `color`, so it
  tracks your matugen accent automatically.
- **Media:** track `title • artist` from `playerctl --follow`, live. Transport
  controls (prev / play-pause / next) dispatch `playerctl`. The play button fills
  with the accent colour while playing.
- **Auto-hide:** when no MPRIS player exists the whole module hides and waybar
  collapses it.
- Optional album art (`art-size > 0`), rounded, shown before the visualiser.

## Build & install

Arch Linux: `yay -S waybar-media-cava` (AUR).

Requires `gtk3`, `glib2` (dev headers), `cava`, `playerctl`, and a C compiler.

Uses [waybar-plugin-common](https://github.com/asteroidzman/waybar-plugin-common)
(git submodule at `common/`) -- clone with `--recursive`, or `git submodule
update --init` after a plain clone, or `make` fails looking for
`common/wbcommon.h`.

```sh
git clone --recursive https://github.com/asteroidzman/waybar-media-cava.git
cd waybar-media-cava
make
make install                 # → ~/.local/lib/waybar/libmedia_cava.so
# or: PREFIX=/usr/lib/waybar sudo make install
```

## waybar config

```jsonc
"modules-center": ["cffi/media"],

"cffi/media": {
    "module_path": "/home/YOU/.local/lib/waybar/libmedia_cava.so",
    "bars": 6,
    "max-length": 40,
    "controls": 1
}
```

Options:

| key | default | meaning |
|-----|---------|---------|
| `bars` | 6 | number of visualiser bars |
| `bar-width` | 4 | bar width (px) |
| `bar-gap` | 3 | gap between bars (px) |
| `bar-min` | 5 | bar length at rest (px) |
| `viz-height` | 34 | full vertical extent of the visualiser (px) |
| `mirror` | 1 | 1 = bars centred/mirrored; 0 = bottom-anchored |
| `smoothing` | 0.35 | easing factor per frame (0–1; lower = smoother/slower) |
| `controls` | 1 | show the prev / play-pause / next buttons |
| `max-length` | 40 | max characters of the title before ellipsis |
| `art-size` | 0 | album-art size (px); `0` disables art |

## style.css

The plugin exposes these selectors:

```css
#media-cava            { padding: 0 10px; }
#media-cava .cava      { color: @primary; }         /* bar colour */
#media-cava .media-title { color: @on_surface; }
#media-cava .media-btn:hover { background-color: @surface_container_highest; }
#media-cava .media-play.playing {                    /* filled while playing */
    background-color: @primary;
    color: @on_primary;
    border-radius: 999px;
}
```

Controls are `GtkEventBox`es (so the pointer keeps the compositor cursor size and
doesn't shrink on hover). Their circular size is set in the plugin; CSS paints the
background / colour.

## License

MIT
