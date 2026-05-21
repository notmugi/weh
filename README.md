# weh

A tiny C image viewer for X11 and Wayland.

`weh` opens an image (or a directory of images) and displays it. A minimal window with the image in it, sized to your display, that you can pan, zoom, flip, rotate, tile,
fullscreen, animate, skim, and drag-drop files onto.

## Features

- **Native X11 and Wayland** via SDL3.
- **Broad format support**: PNG, JPEG, TIFF, BMP, PSD, single-frame WebP,
  AVIF, PNM, HDR, EXR, TGA, etc. via GEGL; **animated GIF / APNG /
  animated WebP / animated AVIF** via gdk-pixbuf; **SVG** via librsvg,
  re-rasterized on demand for crispness at any zoom.
- **Aspect-locked window** that snaps back to the image's aspect during
  interactive resize, with a configurable letterbox fallback for tiled
  wms (white by default; override with `--bg`). Toggle with `a`.
- **Smooth pan and zoom** — wheel zoom anchors on whatever you pan to;
  clicking and dragging pans the image.
- **Flip and rotate** with `h` (horizontal), `v` (vertical), `k` (rotate CCW), `l` (rotate CW).
- **Tile mode** — repeat the image outward in a grid, with optional
  mirrored tiling so seams match. Great for viewing tiled game textures. Toggle with `t`
- **Pixel-art mode** — nearest-neighbor texture sampling toggle. Toggle with `n`
- **Animation control** — pause/resume, frame-by-frame skim. Pause with `p` and skim with `-/=`
- **Directory navigation** — launch with a directory as input or drag it onto the window. Navigate with `,` to move backward and `.` to move forward.
- **Multi-file playlist** — launch with multiple image arguments (or drag-drop several images at once) to navigate through just those files, no directory scan involved.
- **Drag-and-drop** an image, a set of images, or a directory onto the window to start viewing.
- **Info overlay:** toggle with `i` to see path, size, dimensions,
  format, animation status, current frame, and zoom level.
- **Binds overlay:** Display all keybinds with the `b` key

## Build

### Arch Linux

```sh
sudo pacman -S base-devel sdl3 gegl babl gdk-pixbuf2 librsvg cairo
git clone https://github.com/notmugi/weh
cd weh
make
```

For more animated format support, also install the matching gdk-pixbuf
loaders, e.g. `webp-pixbuf-loader`.

### Other distros

You need development headers for:

| Package      | pkg-config       | minimum |
|--------------|------------------|---------|
| SDL3         | `sdl3`           | 3.2     |
| GEGL         | `gegl-0.4`       | 0.4     |
| babl         | `babl-0.1`       | 0.1     |
| gdk-pixbuf 2 | `gdk-pixbuf-2.0` | 2.42    |

Then `make`. There is no autoconf or cmake step.

## Install

```sh
sudo make install                # to /usr/local
sudo make PREFIX=/usr install    # system-wide
make DESTDIR=/tmp/pkg install    # staged for packaging
```

`make install` lays down the binary, the manpage (`weh.1`), the
`weh.desktop` file, and (if present in `dist/icons/`) the icon files
under the freedesktop hicolor tree.

To uninstall:

```sh
sudo make uninstall
```

## Usage

```sh
weh [OPTIONS] [IMAGE...|DIRECTORY]
```

Open a single image:

```sh
weh image.png
```

Open a directory and navigate through it:

```sh
weh ~/Pictures/
```

Open several specific images as an ad-hoc playlist:

```sh
weh a.png b.jpg c.webp
```

In all three forms above, `,` moves backwards through the list and `.`
moves forward (with wrap-around). `Home` / `End` jump to the first /
last entry.

Launch blank — drop something onto the window to begin:

```sh
weh
```

Fullscreen, pixel-art mode, starting zoomed in:

```sh
weh -f -n -z 2.0 sprite.png
```

Custom title and magenta letterbox:

```sh
weh -t "Reference" --bg=#ff00ff ref.jpg
```

Force X11 backend (uses XWayland on a Wayland session):

```sh
weh --x11 image.png
```

See `weh --help` and `man weh` for the full reference.

## Controls

| Key / Mouse       | Action                                                       |
|-------------------|--------------------------------------------------------------|
| `f`               | Toggle fullscreen                                            |
| `r`               | Hard reset: zoom, pan, flips, rotation                       |
| `i`               | Toggle the info overlay (bottom-left)                        |
| `b`               | Toggle the keybind cheat-sheet (centered)                    |
| `n`               | Toggle nearest-neighbor sampling (pixel art)                 |
| `a`               | Toggle aspect-ratio locking                                  |
| `h`               | Flip horizontal                                              |
| `v`               | Flip vertical                                                |
| `k`               | Rotate 90° counter-clockwise                                 |
| `l`               | Rotate 90° clockwise                                         |
| `t`               | Toggle tile mode                                             |
| `w`               | Toggle mirrored tiling (when tile mode is on)                |
| `1`–`9`           | Tile radius (N tiles from center → (2N+1) × (2N+1) grid)     |
| `p`               | Pause/resume animation (animated images only)                |
| `-`               | Previous animation frame (skim; implicitly pauses)           |
| `=`               | Next animation frame                                         |
| `,` / `.`         | Previous / next image in playlist (wraps)                    |
| `Home` / `End`    | First / last image in playlist                               |
| `q` or `Esc`      | Quit                                                         |
| Mouse wheel       | Zoom in/out (anchors on the panned-to point)                 |
| Left-drag         | Pan (1:1 with cursor)                                        |
| Drop a file / dir | Open the dropped path                                        |

## CLI options

| Flag                  | Description                                       |
|-----------------------|---------------------------------------------------|
| `-h`, `--help`        | Show help and exit                                |
| `-v`, `--version`     | Show version and exit                             |
| `-f`, `--fullscreen`  | Start in fullscreen                               |
| `-n`, `--nearest`     | Start with nearest-neighbor sampling              |
| `--no-aspect-lock`    | Don't request aspect-lock from the WM             |
| `--x11`               | Force the X11 video backend (uses XWayland)       |
| `--wayland`           | Force the Wayland video backend                   |
| `-z`, `--zoom=N`      | Initial zoom factor (default `1.0`)               |
| `-t`, `--title=STR`   | Window title (default: filename)                  |
| `--app-id=STR`        | Wayland `app_id` / X11 `WM_CLASS` (default below) |
| `--bg=#RRGGBB`        | Background / letterbox color (default `#ffffff`)  |

The default `app_id` is `io.github.notmugi.weh`. This is the string a
compositor matches against `weh.desktop` to pick up the icon, so don't
override it unless you also install a matching `.desktop`.

## Behavior notes

- **`r` is partial reset**: it clears zoom, pan, flip, and rotation,
  and refits the image into the current window. Tile mode and
  aspect-lock state are preserved. Pause/skim state is also preserved; if you were paused on frame 4, it should stay on frame 4.
- **Per-image reset**: when navigating with arrows or drag-dropping a
  new file, the view state (zoom, pan, flip, rotate, pause) resets.
  Tile mode is preserved across navigation.
- **Skim builds a frame cache** on first use. For a typical 500x500x30-frame
  GIF that's ~30MB. The cache is freed when you navigate away.

### X11 / XWayland

On X11, `weh` also pushes the icon directly via `SDL_SetWindowIcon`,
reading the same hicolor files at startup. This works even without a
`.desktop` file installed.

## Format support

| Family        | Animation | Loader     | Notes                       |
|---------------|-----------|------------|-----------------------------|
| PNG           | n/a       | GEGL       |                             |
| JPEG          | n/a       | GEGL       |                             |
| TIFF          | n/a       | GEGL       | first frame                 |
| BMP           | n/a       | GEGL       |                             |
| PSD           | n/a       | GEGL       | composited                  |
| TGA           | n/a       | GEGL       |                             |
| PNM / PPM     | n/a       | GEGL       |                             |
| HDR / EXR     | n/a       | GEGL       | tone-mapped to SDR display  |
| GIF           | yes       | gdk-pixbuf |                             |
| APNG          | yes       | gdk-pixbuf |                             |
| Animated WebP | yes       | gdk-pixbuf | needs webp loader pkg       |
| Animated AVIF | yes       | gdk-pixbuf | needs avif loader pkg       |
| Static WebP   | n/a       | GEGL       |                             |
| SVG           | n/a       | librsvg    | vector; crisp at every zoom |

## Architecture notes

- **SVG path** keeps a low-res whole-image "backdrop" rasterized once at
  load, drawn beneath a high-quality clipped raster produced by a
  dedicated worker thread on demand. A 60 ms debounce keeps fast wheel
  / pan from queuing redundant work. Flips, rotation, and tile mode are
  supported on SVGs: when any of those are active the worker switches
  from "raster only the visible portion" to "raster the whole image",
  so each rendered tile is sharp at the current per-tile scale.
- **Animated images** use SDL3's event-loop timeout to wake at each
  frame's delay; nothing polls. Pause/skim builds a lazy full-frame
  cache only when needed.

## Dependencies (runtime)

- `libsdl3`
- `libgegl-0.4`, `libbabl-0.1` (and the loader modules GEGL needs for
  the formats you intend to view)
- `libgdk_pixbuf-2.0`
- `librsvg-2` and `cairo` (used for SVG rasterization)

Plus, optionally, gdk-pixbuf loader packages for additional animated
formats (`webp-pixbuf-loader`, AVIF loaders, …).

## License

GPLv3-or-later. See `LICENSE`.

## Contributing

Issues and pull requests welcome at
<https://github.com/notmugi/weh>.
