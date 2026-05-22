/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * weh - tiny C image viewer for X11 and Wayland.
 * Copyright (C) 2026 Sean G.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version. See the LICENSE file for the
 * full license text, or <https://www.gnu.org/licenses/>.
 */

#include <SDL3/SDL.h>
#include <gegl.h>
#include <babl/babl.h>

/* gdk-pixbuf's animation iter API is officially deprecated in 2.44 with
   no replacement shipped. We use it intentionally; pin the min-required
   version to 2.42 so the compiler does not emit deprecation diagnostics
   for the symbols we knowingly call. */
#include <glib.h>
#define GDK_PIXBUF_VERSION_MIN_REQUIRED (G_ENCODE_VERSION(2, 42))
#include <gdk-pixbuf/gdk-pixbuf.h>

#include <cairo.h>
#include <librsvg/rsvg.h>
#include <pango/pangocairo.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define APP_NAME    "weh"
#define APP_TITLE   "weh"
#define APP_ID      "io.github.notmugi.weh"
#define APP_VERSION "0.1.0"
#define WINDOW_MARGIN 64
#define ZOOM_STEP 1.125
#define ZOOM_MIN 0.02
#define ZOOM_MAX 128.0
/* SVG tile-pyramid sizing.

   Base texture: rasterized once at load. Smaller is faster to load and
   uses less VRAM, but means the GPU does more bilinear upscaling when
   the user zooms in. The "sharp tile" below picks up the slack.

   Sharp tile: re-rasterized on demand by a worker thread when the user
   is zoomed in past base sharpness. Covers only the visible part of
   the image (plus a small margin for panning) at the exact pixel scale
   that the current view needs, capped at TILE_MAX_DIM.

   Debounce: wait this long after the last view change (resize / zoom /
   pan) before kicking off a tile raster job, so dragging the wheel
   doesn't queue dozens of redundant rasters. */
#define SVG_BASE_MAX_DIM 4096
#define SVG_TILE_MAX_DIM 4096
#define SVG_TILE_PAD_RATIO 0.15      /* extend visible rect by 15% per side */
#define SVG_DEBOUNCE_NS  (80ULL * 1000000ULL)  /* 80 ms */

typedef enum ImageKind {
    IMG_STATIC,
    IMG_SVG,
    IMG_ANIM
} ImageKind;

typedef struct StaticImage {
    int width;
    int height;
    unsigned char *rgba;
} StaticImage;

/* SVG with two-level pyramid:
     - BASE: rasterized once at load, capped at SVG_BASE_MAX_DIM. The
       fields `width`, `height`, `rgba` are aliases for the base raster
       so the rest of the renderer can treat the SVG as a static image
       for the always-visible "background" pass.
     - TILE: optional sharper raster for the current visible region.
       Produced by a worker thread on demand when the user zooms in
       past the base resolution. Drawn as an overlay on top of the
       base only when its generation matches the current view state,
       so a tile that's about to become stale never tears.

   The worker thread runs the entire SVG lifetime. Generation counter
   is bumped by the main thread on any view change (zoom / pan / resize
   / flip / rotation) — the worker stamps each finished job with the
   generation it was started for, and main thread discards results
   whose stamp != current generation. This is bulletproof against
   resize races: the only way for a tile to be drawn is for the worker
   to have just produced it for the CURRENT view, full stop. */
typedef struct SvgImage {
    /* Identity / natural dimensions in SVG user units. */
    RsvgHandle *handle;            /* kept alive for the worker's renders */
    double      natural_w_f;
    double      natural_h_f;

    /* BASE raster (CPU). Treated as the canonical "image" by the rest
       of the renderer; `width`, `height`, `rgba` alias the base. */
    int            width;          /* == base width */
    int            height;         /* == base height */
    unsigned char *rgba;           /* == base rgba */

    /* SHARP TILE (GPU). Lives on the renderer; only drawn when valid.
       Image-space rectangle covered, in SVG user units. */
    SDL_Texture *tile_tex;
    int          tile_tex_w;
    int          tile_tex_h;
    double       tile_ix, tile_iy, tile_iw, tile_ih;
    bool         tile_valid;       /* true iff tile_tex contains data for
                                      the rect above at the current gen */
    Uint64       tile_generation;  /* gen the tile was rendered for */

    /* Worker pipeline. Mutex protects the request slot and the done
       slot; the worker holds it only briefly at start and end of each
       job, never during the actual cairo render. */
    SDL_Thread     *worker;
    SDL_Mutex      *mu;
    SDL_Condition  *cv;
    bool            quit;
    Uint64          current_generation;  /* bumped on view change (main only) */

    /* Snapshot of the view-affecting state at the last render, used to
       detect view changes without instrumenting every mutation site.
       When any of these change between frames, current_generation is
       bumped (which invalidates the tile and resets the debounce
       timer so a re-raster will be queued after the window). */
    int    last_window_w, last_window_h;
    double last_zoom;
    double last_anchor_x, last_anchor_y;
    double last_pan_off_x, last_pan_off_y;
    int    last_rotation_steps;
    bool   last_flip_h, last_flip_v;
    bool   last_tile_on;
    int    last_tile_radius;
    bool   last_view_valid;

    /* Pending job — main thread queues the latest desired raster here.
       Worker pulls when job_pending, latest-wins. */
    bool   job_pending;
    Uint64 pending_generation;
    double pending_ix, pending_iy, pending_iw, pending_ih;
    int    pending_out_w, pending_out_h;

    /* Debounce window: main thread sets dirty_since_ns on each view
       change; the request goes out only after the window elapses. */
    Uint64 dirty_since_ns;
    /* Generation we last enqueued a request for. The request gate
       skips re-queueing when last_requested_generation matches
       current_generation. */
    bool   has_last_requested;
    Uint64 last_requested_generation;

    /* Two-buffer pipeline. The worker renders into `worker_rgba`; when
       it finishes it swaps `worker_rgba` <-> `delivered_rgba` under the
       mutex and sets job_done. Main thread reads `delivered_rgba` and
       uploads to the GPU. The two never alias, so main's
       SDL_UpdateTexture and worker's next cairo render can run
       concurrently without corrupting each other. */
    bool           job_done;
    Uint64         done_generation;
    double         done_ix, done_iy, done_iw, done_ih;
    int            done_out_w, done_out_h;
    unsigned char *delivered_rgba;
    size_t         delivered_cap;
    unsigned char *worker_rgba;
    size_t         worker_cap;
} SvgImage;

typedef struct AnimImage {
    GdkPixbufAnimation     *anim;
    GdkPixbufAnimationIter *iter;
    int width;
    int height;
    int frame_delay_ms;  /* delay reported by the iter for the current frame */
    Uint64 next_frame_ns;
    unsigned char *rgba; /* current frame as RGBA, packed (width*4 stride) */
} AnimImage;

typedef struct Source {
    ImageKind kind;
    int width;   /* canonical (natural) image width  */
    int height;  /* canonical (natural) image height */
    union {
        StaticImage st;
        SvgImage svg;
        AnimImage anim;
    } v;
} Source;

/* User-tunable options (CLI flags, env, defaults).
   All fields have sensible defaults; image paths are optional.

   image_paths is a non-owning array of pointers into argv. count == 0
   means no images given (blank window). count == 1 may be a single
   file OR a directory (dir is auto-expanded into a DirList). count > 1
   means an ad-hoc playlist — each path is treated as a single file and
   the union becomes the navigation list. */
typedef struct Options {
    const char **image_paths;   /* non-owning; points into argv */
    int          image_count;
    const char *title;          /* window title (defaults to APP_TITLE or filename) */
    const char *app_id;         /* xdg app_id / X11 WM_CLASS (defaults to APP_ID) */
    bool start_fullscreen;      /* --fullscreen */
    bool start_nearest;         /* --nearest */
    bool no_aspect_lock;        /* --no-aspect-lock */
    bool force_x11;             /* --x11 */
    bool force_wayland;         /* --wayland */
    double initial_zoom;        /* --zoom=N */
    Uint8 bg_r, bg_g, bg_b;     /* --bg=#rrggbb (border/letterbox color) */
} Options;

/* Directory navigation list. Populated only when launched with a
   directory argument (or when a directory is drag-dropped). When empty
   (count == 0), nav keys are no-ops. */
typedef struct DirList {
    char **entries;     /* full paths, sorted case-insensitively */
    int    count;
    int    current;     /* index of the currently-shown entry */
    char  *base_dir;    /* owned; root of the listing */
} DirList;

typedef struct Viewer {
    SDL_Window *window;
    SDL_Renderer *renderer;
    /* For static and GIF: a fixed-size texture matching the natural image.
       For SVG: re-created at the current rasterized size. */
    SDL_Texture *texture;
    int texture_w;
    int texture_h;

    Source src;
    char  *current_path;   /* owned copy of the path of the loaded image,
                              or NULL when no image is loaded (blank window). */
    size_t current_size_bytes; /* file size on disk (0 if unknown) */

    int initial_window_w;
    int initial_window_h;
    double zoom;
    /* Anchor point in normalized image coordinates [0,1] x [0,1].
       This image point is kept aligned with the window center. */
    double anchor_x;
    double anchor_y;
    /* Pure screen-space pan accumulated by left-drag, in window units.
       Applied as a translation to the final on-screen image rect after
       all rotation/flip math. Decouples dragging from image orientation
       so the image always tracks the cursor 1:1. */
    double pan_off_x;
    double pan_off_y;
    bool panning;
    bool fullscreen;
    /* Texture sampling mode. false = linear (smooth scaling, default);
       true = nearest neighbor (crisp pixel art). Toggled with 'n'. */
    bool nearest;
    /* Runtime aspect-lock toggle (initial value comes from options). */
    bool aspect_lock;

    /* --- transform --- */
    bool flip_h;
    bool flip_v;
    int  rotation_steps; /* 0..3, each step is 90° clockwise */

    /* --- tile mode --- */
    bool tile_on;
    bool tile_mirror;    /* w toggles; default false */
    int  tile_radius;    /* N: tiles extend N positions outward from the
                            center, producing a (2N+1)x(2N+1) grid.
                            1..9; 0 means "no tile" (off). Default 1. */

    /* --- info overlay --- */
    bool info_on;
    bool binds_on;   /* `b` toggles a centered keybinds cheat-sheet */
    /* Pre-formatted lines. Rebuilt whenever the current image changes. */
    char info_lines[16][256];
    int  info_line_count;

    /* Pango context, cached for the lifetime of the viewer. PangoContext
       is expensive to create (it spins up fontconfig + freetype lookups),
       so we make one per process. Per-draw font descriptions are built
       on the fly so we can vary size/weight. */
    PangoFontMap *pango_fontmap; /* singleton — do NOT unref */
    PangoContext *pango_ctx;

    /* --- animation playback control --- */
    bool paused;            /* p toggles; only meaningful for IMG_ANIM */
    /* Lazy all-frames cache for arbitrary-frame skim. Built on first
       skim or pause. Layout: contiguous array of (width*height*4) RGBA
       frames; delays[i] is the delay in ms reported by the iter for
       frame i. */
    bool         frames_decoded;
    int          frame_count;
    int          frame_w;
    int          frame_h;
    int          current_frame;
    unsigned char **frames; /* count entries, each width*height*4 bytes */
    int          *frame_delays_ms;

    /* --- directory navigation --- */
    DirList dir;

    /* Reference to the parsed options (for bg color, etc.). */
    const Options *opts;
} Viewer;

/* ---------- utilities ---------- */

static void print_version(FILE *stream) {
    fprintf(stream, "%s %s\n", APP_NAME, APP_VERSION);
}

static void print_usage(FILE *stream) {
    fprintf(stream,
        "Usage: %s [OPTIONS] [IMAGE...|DIRECTORY]\n"
        "\n"
        "A tiny image viewer for X11 and Wayland.\n"
        "\n"
        "With no arguments, weh opens a blank window; drag-drop an image,\n"
        "multiple images, or a directory onto it to start viewing.\n"
        "With multiple IMAGE arguments (or a multi-file drag-drop) the\n"
        "images form an ad-hoc playlist navigable with ',' and '.'.\n"
        "\n"
        "Options:\n"
        "  -h, --help              Show this help and exit\n"
        "  -v, --version           Show version and exit\n"
        "  -f, --fullscreen        Start in fullscreen mode\n"
        "  -n, --nearest           Start with nearest-neighbor sampling\n"
        "      --no-aspect-lock    Do not request aspect-ratio locking from the WM\n"
        "      --x11               Force the X11 video backend (uses XWayland on Wayland)\n"
        "      --wayland           Force the Wayland video backend\n"
        "  -z, --zoom=N            Initial zoom factor (default 1.0)\n"
        "  -t, --title=STR         Window title (default: filename)\n"
        "      --app-id=STR        Set Wayland app_id / X11 WM_CLASS (default %s)\n"
        "      --bg=#RRGGBB        Background / letterbox color (default ffffff)\n"
        "\n"
        "Controls (in-window):\n"
        "  f                  Toggle fullscreen\n"
        "  r                  Hard reset zoom, pan, and transforms\n"
        "  i                  Toggle info overlay\n"
        "  b                  Toggle keybind cheat-sheet\n"
        "  n                  Toggle nearest-neighbor sampling (pixel art)\n"
        "  a                  Toggle aspect-ratio locking\n"
        "  h / v              Flip horizontal / vertical\n"
        "  k / l              Rotate 90 deg CCW / CW\n"
        "  t                  Toggle tile mode\n"
        "  w                  Toggle mirrored tiles (when tiling)\n"
        "  1..9               Tile radius: 1 = 3x3 ... 9 = 19x19 (when tiling)\n"
        "  p                  Pause / resume animation (animated images only)\n"
        "  -  /  =            Step to previous / next frame (animated images only)\n"
        "  , / .              Previous / next image in playlist (with wrap)\n"
        "  Home / End         First / last image in playlist\n"
        "  q, Esc             Quit\n"
        "  Mouse wheel        Zoom in/out (anchors on the panned-to point)\n"
        "  Left-drag          Pan\n"
        "  Drop a file        Open that file\n"
        "  Drop a directory   Open the directory; arrow keys to navigate\n"
        ,
        APP_NAME, APP_ID);
}

/* Parse "#RRGGBB" or "RRGGBB" into 3 bytes. Returns false on failure. */
static bool parse_hex_color(const char *s, Uint8 *r, Uint8 *g, Uint8 *b) {
    if (!s) return false;
    if (*s == '#') s++;
    if (strlen(s) != 6) return false;
    for (int i = 0; i < 6; i++) {
        if (!isxdigit((unsigned char)s[i])) return false;
    }
    char buf[3] = {0};
    buf[0] = s[0]; buf[1] = s[1]; *r = (Uint8)strtol(buf, NULL, 16);
    buf[0] = s[2]; buf[1] = s[3]; *g = (Uint8)strtol(buf, NULL, 16);
    buf[0] = s[4]; buf[1] = s[5]; *b = (Uint8)strtol(buf, NULL, 16);
    return true;
}

/* Returns 0 on success, 1 on help/version (exit 0), 2 on parse error (exit 2). */
static int parse_options(int argc, char **argv, Options *opts) {
    memset(opts, 0, sizeof(*opts));
    opts->title  = NULL;
    opts->app_id = APP_ID;
    opts->initial_zoom = 1.0;
    opts->bg_r = opts->bg_g = opts->bg_b = 255;

    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break; /* end of options */
        if (strcmp(a, "--") == 0) { i++; break; }

        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            print_usage(stdout);
            return 1;
        }
        if (strcmp(a, "-v") == 0 || strcmp(a, "--version") == 0) {
            print_version(stdout);
            return 1;
        }
        if (strcmp(a, "-f") == 0 || strcmp(a, "--fullscreen") == 0) {
            opts->start_fullscreen = true; continue;
        }
        if (strcmp(a, "-n") == 0 || strcmp(a, "--nearest") == 0) {
            opts->start_nearest = true; continue;
        }
        if (strcmp(a, "--no-aspect-lock") == 0) {
            opts->no_aspect_lock = true; continue;
        }
        if (strcmp(a, "--x11") == 0) {
            opts->force_x11 = true; opts->force_wayland = false; continue;
        }
        if (strcmp(a, "--wayland") == 0) {
            opts->force_wayland = true; opts->force_x11 = false; continue;
        }
        /* Options that take a value: support --opt=VAL and --opt VAL */
        const char *value = NULL;
        const char *eq = strchr(a, '=');
        if (eq) {
            value = eq + 1;
        }
        #define TAKE_VALUE(short_, long_, fld_assign)                          \
            do {                                                              \
                if ((short_ && strcmp(a, short_) == 0) ||                     \
                    (eq && strncmp(a, long_, strlen(long_)) == 0 &&           \
                     a[strlen(long_)] == '=')) {                              \
                    const char *v = eq ? value : (i + 1 < argc ? argv[++i] : NULL); \
                    if (!v) {                                                 \
                        fprintf(stderr, "%s: %s requires a value\n",          \
                                APP_NAME, a);                                 \
                        return 2;                                             \
                    }                                                         \
                    fld_assign;                                               \
                    goto next_opt;                                            \
                }                                                             \
                if (strcmp(a, long_) == 0) {                                  \
                    if (i + 1 >= argc) {                                      \
                        fprintf(stderr, "%s: %s requires a value\n",          \
                                APP_NAME, a);                                 \
                        return 2;                                             \
                    }                                                         \
                    const char *v = argv[++i];                                \
                    fld_assign;                                               \
                    goto next_opt;                                            \
                }                                                             \
            } while (0)

        TAKE_VALUE("-z", "--zoom",
                   opts->initial_zoom = strtod(v, NULL));
        TAKE_VALUE("-t", "--title",
                   opts->title = v);
        TAKE_VALUE(NULL, "--app-id",
                   opts->app_id = v);
        #undef TAKE_VALUE

        /* --bg=#RRGGBB / --bg #RRGGBB (handled outside the macro because
           the parse can fail). */
        if (strcmp(a, "--bg") == 0 ||
            (eq && strncmp(a, "--bg", 4) == 0 && a[4] == '=')) {
            const char *v = eq ? value : (i + 1 < argc ? argv[++i] : NULL);
            if (!v) {
                fprintf(stderr, "%s: --bg requires a value\n", APP_NAME);
                return 2;
            }
            if (!parse_hex_color(v, &opts->bg_r, &opts->bg_g, &opts->bg_b)) {
                fprintf(stderr,
                        "%s: bad color '%s' (expected #RRGGBB)\n",
                        APP_NAME, v);
                return 2;
            }
            goto next_opt;
        }

        fprintf(stderr, "%s: unknown option '%s' (try --help)\n", APP_NAME, a);
        return 2;
    next_opt:;
    }

    /* Positional args are images. Zero is fine (blank window). One is
       fine (single file or directory). More than one creates an ad-hoc
       playlist — see DirList usage in main(). Allocated lazily so the
       common no-image case stays alloc-free. */
    int remaining = argc - i;
    if (remaining > 0) {
        opts->image_paths = malloc(sizeof(char *) * (size_t)remaining);
        if (!opts->image_paths) {
            fprintf(stderr, "%s: out of memory parsing arguments\n", APP_NAME);
            return 2;
        }
        for (int k = 0; i < argc; i++, k++) {
            opts->image_paths[k] = argv[i];
        }
        opts->image_count = remaining;
    }
    /* Sanitize initial_zoom. */
    if (!isfinite(opts->initial_zoom) || opts->initial_zoom <= 0.0) {
        opts->initial_zoom = 1.0;
    }
    return 0;
}

static void die_sdl(const char *what) {
    fprintf(stderr, "%s: %s failed: %s\n", APP_NAME, what, SDL_GetError());
}

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static double clamp_double(double v, double lo, double hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static bool has_ext_ci(const char *path, const char *ext) {
    size_t pl = strlen(path);
    size_t el = strlen(ext);
    if (pl < el) return false;
    const char *tail = path + pl - el;
    for (size_t i = 0; i < el; i++) {
        if (tolower((unsigned char)tail[i]) != tolower((unsigned char)ext[i])) {
            return false;
        }
    }
    return true;
}

/* ---------- SVG loader & async rasterizer ---------- */

/* Convert cairo's ARGB32 (premultiplied, native-endian — i.e. BGRA on LE)
   in-place to SDL's straight RGBA32 (R G B A bytes). Operates on a
   width*height pixel buffer. */
static void cairo_argb32_to_rgba_inplace(unsigned char *buf, int width, int height) {
    /* On little-endian: pixel bytes are [B, G, R, A] in memory.
       On big-endian:    pixel bytes are [A, R, G, B] in memory.
       We want [R, G, B, A] either way, with straight (un-premultiplied) RGB. */
    const size_t n = (size_t)width * (size_t)height;
    for (size_t i = 0; i < n; i++) {
        unsigned char *p = buf + i * 4u;
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
        unsigned char a = p[0], r = p[1], g = p[2], b = p[3];
#else
        unsigned char b = p[0], g = p[1], r = p[2], a = p[3];
#endif
        if (a != 0 && a != 255) {
            /* Un-premultiply: rgb = rgb * 255 / a, rounded. */
            r = (unsigned char)((r * 255 + a / 2) / a);
            g = (unsigned char)((g * 255 + a / 2) / a);
            b = (unsigned char)((b * 255 + a / 2) / a);
        }
        p[0] = r; p[1] = g; p[2] = b; p[3] = a;
    }
}

/* Rasterize a sub-region of the SVG into `dst` (out_w * out_h, 4 bytes
   per pixel). Output is in cairo's native ARGB32 format: native-endian
   ARGB with premultiplied alpha. On little-endian (everything we care
   about) the byte order in memory is B,G,R,A. Callers wanting straight
   RGBA must call cairo_argb32_to_rgba_inplace() on the result.

   Leaving the conversion as the caller's choice avoids paying for it
   on the hot path: SDL can sample the cairo buffer directly via
   SDL_PIXELFORMAT_ARGB32 (== BGRA in memory on LE) with
   SDL_BLENDMODE_BLEND_PREMULTIPLIED, saving a full pass over the
   tile pixels (which can be tens of MB).

   The image-space region rendered spans:
       x in [ix, ix + out_w/scale)
       y in [iy, iy + out_h/scale)
   in SVG user units. */
static bool svg_render_region(RsvgHandle *handle,
                              double natural_w, double natural_h,
                              double ix, double iy, double scale,
                              unsigned char *dst, int out_w, int out_h) {
    if (out_w <= 0 || out_h <= 0 || scale <= 0.0 ||
        natural_w <= 0.0 || natural_h <= 0.0) {
        return false;
    }

    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, out_w);
    /* We allocated exactly out_w * 4 bytes per row, but cairo may want
       padding. If stride doesn't match, render into a separate cairo
       buffer and copy out (rare; cairo's stride is row-padded to a
       4-byte multiple, which our 4-byte pixels already satisfy except
       at pathologically odd widths). */
    cairo_surface_t *surface;
    unsigned char *cbuf = NULL;
    bool need_copy = (stride != out_w * 4);
    if (need_copy) {
        cbuf = calloc(1, (size_t)stride * (size_t)out_h);
        if (!cbuf) return false;
        surface = cairo_image_surface_create_for_data(
            cbuf, CAIRO_FORMAT_ARGB32, out_w, out_h, stride);
    } else {
        /* Zero dst so any uncovered pixels (e.g. SVG with transparent
           areas) start transparent. cairo composites with OVER by
           default, so untouched pixels would otherwise keep stale
           bytes from a previous tile. */
        memset(dst, 0, (size_t)out_w * (size_t)out_h * 4u);
        surface = cairo_image_surface_create_for_data(
            dst, CAIRO_FORMAT_ARGB32, out_w, out_h, stride);
    }
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        free(cbuf);
        return false;
    }

    cairo_t *cr = cairo_create(surface);
    if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        free(cbuf);
        return false;
    }

    /* Transform so the SVG sub-region [ix, iy] -> output (0,0). */
    cairo_scale(cr, scale, scale);
    cairo_translate(cr, -ix, -iy);

    RsvgRectangle viewport = { 0.0, 0.0, natural_w, natural_h };
    GError *err = NULL;
    gboolean ok = rsvg_handle_render_document(handle, cr, &viewport, &err);
    cairo_destroy(cr);
    cairo_surface_flush(surface);

    if (!ok) {
        if (err) { g_error_free(err); }
        cairo_surface_destroy(surface);
        free(cbuf);
        return false;
    }
    /* cairo can fail silently and still return ok=true. Check status. */
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        free(cbuf);
        return false;
    }

    if (need_copy) {
        for (int y = 0; y < out_h; y++) {
            memcpy(dst + (size_t)y * out_w * 4u,
                   cbuf + (size_t)y * stride,
                   (size_t)out_w * 4u);
        }
    }
    cairo_surface_destroy(surface);
    free(cbuf);
    /* No conversion here — see header comment. */
    return true;
}

/* Worker thread: rasterizes whatever plan main thread puts in pending_plan
   into svg->scratch (resized as needed), then signals job_done. Loops until
   svg->quit. */
/* Determine an SVG's natural dimensions in user units. Prefer the
   intrinsic size (CSS width/height) if available, fall back to the
   viewBox, and finally to a sensible default. Returns true on success. */
static bool svg_get_natural_size(RsvgHandle *handle,
                                 double *out_w, double *out_h) {
    gboolean has_width = FALSE, has_height = FALSE, has_viewbox = FALSE;
    RsvgLength width = {0}, height = {0};
    RsvgRectangle viewbox = {0};
    rsvg_handle_get_intrinsic_dimensions(handle,
                                         &has_width, &width,
                                         &has_height, &height,
                                         &has_viewbox, &viewbox);
    /* If the SVG declares pixel width/height, use those directly. */
    if (has_width && has_height &&
        width.unit == RSVG_UNIT_PX && height.unit == RSVG_UNIT_PX &&
        width.length > 0.0 && height.length > 0.0) {
        *out_w = width.length;
        *out_h = height.length;
        return true;
    }
    /* Otherwise try the resolved size in pixels. */
    gdouble rw = 0.0, rh = 0.0;
    if (rsvg_handle_get_intrinsic_size_in_pixels(handle, &rw, &rh) &&
        rw > 0.0 && rh > 0.0) {
        *out_w = rw;
        *out_h = rh;
        return true;
    }
    /* Final fallback: the viewBox. */
    if (has_viewbox && viewbox.width > 0.0 && viewbox.height > 0.0) {
        *out_w = viewbox.width;
        *out_h = viewbox.height;
        return true;
    }
    /* Truly unknown: pick a default so we still display something. */
    *out_w = 512.0;
    *out_h = 512.0;
    return true;
}

/* Worker thread: wait on the condvar for a pending job, snapshot it
   under the mutex, then run the cairo render with the mutex
   released (so the main thread can keep overwriting pending_*
   without blocking). On success, swap worker_rgba <-> delivered_rgba
   under the mutex, set job_done. On failure, set job_done with
   done_out_w/h = 0 to signal an empty result. Generation is stamped
   on every delivery so the main thread can discard stale results. */
static int svg_worker_main(void *userdata) {
    SvgImage *svg = (SvgImage *)userdata;
    for (;;) {
        SDL_LockMutex(svg->mu);
        while (!svg->job_pending && !svg->quit) {
            SDL_WaitCondition(svg->cv, svg->mu);
        }
        if (svg->quit) {
            SDL_UnlockMutex(svg->mu);
            return 0;
        }
        /* Snapshot the pending job. */
        Uint64 gen   = svg->pending_generation;
        double ix    = svg->pending_ix;
        double iy    = svg->pending_iy;
        double iw    = svg->pending_iw;
        double ih    = svg->pending_ih;
        int    out_w = svg->pending_out_w;
        int    out_h = svg->pending_out_h;
        svg->job_pending = false;
        /* Take a reference to the handle so a concurrent close can't
           pull it out from under us. We unref at end of render. */
        RsvgHandle *handle = svg->handle;
        if (handle) g_object_ref(handle);
        SDL_UnlockMutex(svg->mu);

        if (!handle || out_w <= 0 || out_h <= 0 || iw <= 0.0 || ih <= 0.0) {
            if (handle) g_object_unref(handle);
            continue;
        }

        /* Grow worker_rgba as needed. The worker is the sole writer of
           worker_rgba; main never reads or frees it (only delivered_*).
           So no lock is needed here. */
        size_t need = (size_t)out_w * (size_t)out_h * 4u;
        if (need > svg->worker_cap) {
            unsigned char *grown = realloc(svg->worker_rgba, need);
            if (!grown) {
                fprintf(stderr, "%s: SVG tile worker OOM at %dx%d\n",
                        APP_NAME, out_w, out_h);
                g_object_unref(handle);
                /* Deliver an empty done so main thread can drain the
                   slot. Swap buffers (delivered_* now empty, worker
                   keeps its possibly-already-grown buffer). */
                SDL_LockMutex(svg->mu);
                svg->done_generation = gen;
                svg->done_out_w = 0;
                svg->done_out_h = 0;
                svg->job_done = true;
                SDL_UnlockMutex(svg->mu);
                continue;
            }
            svg->worker_rgba = grown;
            svg->worker_cap  = need;
        }
        /* The tile covers image rect [ix, iy, ix+iw, iy+ih] in SVG user
           units. Scale: out_w / iw (== out_h / ih, modulo rounding). */
        double scale = (double)out_w / iw;
        bool ok = svg_render_region(handle, svg->natural_w_f, svg->natural_h_f,
                                    ix, iy, scale,
                                    svg->worker_rgba, out_w, out_h);
        g_object_unref(handle);
        if (!ok) {
            /* Render failed (e.g. cairo allocation, broken SVG). Mark
               an empty done so we don't spin retrying. */
            SDL_LockMutex(svg->mu);
            svg->done_generation = gen;
            svg->done_out_w = 0;
            svg->done_out_h = 0;
            svg->job_done = true;
            SDL_UnlockMutex(svg->mu);
            continue;
        }

        /* Successful render: swap worker_rgba <-> delivered_rgba so main
           can upload from delivered without racing the next render. */
        SDL_LockMutex(svg->mu);
        unsigned char *tmp_rgba = svg->delivered_rgba;
        size_t         tmp_cap  = svg->delivered_cap;
        svg->delivered_rgba = svg->worker_rgba;
        svg->delivered_cap  = svg->worker_cap;
        svg->worker_rgba    = tmp_rgba;
        svg->worker_cap     = tmp_cap;
        svg->done_generation = gen;
        svg->done_ix = ix; svg->done_iy = iy;
        svg->done_iw = iw; svg->done_ih = ih;
        svg->done_out_w = out_w;
        svg->done_out_h = out_h;
        svg->job_done = true;
        SDL_UnlockMutex(svg->mu);
    }
}

/* Main-thread API: queue a tile job. Latest wins (worker may be busy
   on an earlier request — it'll finish, deliver, and we'll discard). */
static void svg_request_tile(SvgImage *svg, Uint64 gen,
                             double ix, double iy, double iw, double ih,
                             int out_w, int out_h) {
    SDL_LockMutex(svg->mu);
    svg->pending_generation = gen;
    svg->pending_ix = ix; svg->pending_iy = iy;
    svg->pending_iw = iw; svg->pending_ih = ih;
    svg->pending_out_w = out_w;
    svg->pending_out_h = out_h;
    svg->job_pending = true;
    SDL_SignalCondition(svg->cv);
    SDL_UnlockMutex(svg->mu);

    svg->has_last_requested = true;
    svg->last_requested_generation = gen;
}

static bool svg_start_worker(SvgImage *svg) {
    svg->worker = SDL_CreateThread(svg_worker_main, "weh-svg", svg);
    if (!svg->worker) {
        fprintf(stderr, "%s: failed to start SVG worker: %s\n",
                APP_NAME, SDL_GetError());
        return false;
    }
    return true;
}

static void svg_stop_worker(SvgImage *svg) {
    if (!svg->worker) return;
    SDL_LockMutex(svg->mu);
    svg->quit = true;
    SDL_SignalCondition(svg->cv);
    SDL_UnlockMutex(svg->mu);
    SDL_WaitThread(svg->worker, NULL);
    svg->worker = NULL;
}

/* Main-thread API: bump generation so any in-flight tile result is
   discarded. Called on every view-state change. Also clears tile_valid
   since the previously-uploaded tile no longer corresponds to the
   current view (it'll get re-validated only if a new tile lands or if
   the new state happens to still be covered by the same image rect —
   we don't bother detecting that, the base texture handles it cheaply). */
static void svg_bump_generation(SvgImage *svg) {
    svg->current_generation++;
    svg->tile_valid = false;
    svg->dirty_since_ns = SDL_GetTicksNS();
}

static bool load_svg(const char *path, Source *out) {
    GError *err = NULL;
    RsvgHandle *handle = rsvg_handle_new_from_file(path, &err);
    if (!handle) {
        if (err) {
            fprintf(stderr, "%s: librsvg failed to load '%s': %s\n",
                    APP_NAME, path, err->message);
            g_error_free(err);
        }
        return false;
    }

    double natural_w_f = 0.0, natural_h_f = 0.0;
    if (!svg_get_natural_size(handle, &natural_w_f, &natural_h_f)) {
        g_object_unref(handle);
        return false;
    }

    /* Base raster: shrink to fit SVG_BASE_MAX_DIM on the long edge,
       never upscale. */
    double long_edge = natural_w_f > natural_h_f ? natural_w_f : natural_h_f;
    double base_scale = 1.0;
    if (long_edge > (double)SVG_BASE_MAX_DIM) {
        base_scale = (double)SVG_BASE_MAX_DIM / long_edge;
    }
    int bw = (int)ceil(natural_w_f * base_scale);
    int bh = (int)ceil(natural_h_f * base_scale);
    if (bw < 1) bw = 1;
    if (bh < 1) bh = 1;

    unsigned char *base = calloc(1, (size_t)bw * (size_t)bh * 4u);
    if (!base) {
        fprintf(stderr, "%s: out of memory rasterizing SVG base '%s' at %dx%d\n",
                APP_NAME, path, bw, bh);
        g_object_unref(handle);
        return false;
    }
    if (!svg_render_region(handle, natural_w_f, natural_h_f,
                           0.0, 0.0, base_scale, base, bw, bh)) {
        fprintf(stderr, "%s: librsvg failed to rasterize '%s'\n", APP_NAME, path);
        free(base);
        g_object_unref(handle);
        return false;
    }
    /* Base goes through the standard RGBA upload path; un-premultiply
       once at load. */
    cairo_argb32_to_rgba_inplace(base, bw, bh);

    /* Set up worker primitives. */
    SDL_Mutex     *mu = SDL_CreateMutex();
    SDL_Condition *cv = SDL_CreateCondition();
    if (!mu || !cv) {
        if (cv) SDL_DestroyCondition(cv);
        if (mu) SDL_DestroyMutex(mu);
        free(base);
        g_object_unref(handle);
        return false;
    }

    /* Populate the SVG and start the worker. */
    SvgImage *svg = &out->v.svg;
    memset(svg, 0, sizeof(*svg));
    svg->handle      = handle; /* worker shares it (refcount-bumped per job) */
    svg->natural_w_f = natural_w_f;
    svg->natural_h_f = natural_h_f;
    svg->width  = bw;
    svg->height = bh;
    svg->rgba   = base;
    svg->mu     = mu;
    svg->cv     = cv;
    svg->current_generation = 1; /* 0 is reserved for "no tile" */

    out->kind   = IMG_SVG;
    out->width  = bw;
    out->height = bh;

    if (!svg_start_worker(svg)) {
        SDL_DestroyCondition(cv);
        SDL_DestroyMutex(mu);
        free(base);
        g_object_unref(handle);
        memset(svg, 0, sizeof(*svg));
        return false;
    }
    return true;
}

static void free_svg(SvgImage *svg) {
    svg_stop_worker(svg);
    if (svg->tile_tex) SDL_DestroyTexture(svg->tile_tex);
    if (svg->cv) SDL_DestroyCondition(svg->cv);
    if (svg->mu) SDL_DestroyMutex(svg->mu);
    if (svg->handle) g_object_unref(svg->handle);
    free(svg->rgba);
    free(svg->delivered_rgba);
    free(svg->worker_rgba);
    memset(svg, 0, sizeof(*svg));
}

/* ---------- Animated raster (GIF/APNG/animated WebP/etc.) via gdk-pixbuf ----- */

/* Copy/convert a GdkPixbuf into anim->rgba (RGBA, packed, width*4 stride).
   Allocates anim->rgba on first call and refuses to resize after that (we
   assume animation frames are constant size; misbehaving files would have
   their frames clipped to the first frame's dimensions). */
static bool anim_copy_pixbuf(AnimImage *anim, GdkPixbuf *pix) {
    if (!pix) return false;
    int w = gdk_pixbuf_get_width(pix);
    int h = gdk_pixbuf_get_height(pix);
    int n = gdk_pixbuf_get_n_channels(pix);
    int stride = gdk_pixbuf_get_rowstride(pix);
    const guchar *src = gdk_pixbuf_read_pixels(pix);
    if (w <= 0 || h <= 0 || !src || (n != 3 && n != 4)) return false;

    if (!anim->rgba) {
        size_t bytes = (size_t)w * (size_t)h * 4u;
        anim->rgba = malloc(bytes);
        if (!anim->rgba) return false;
        anim->width = w;
        anim->height = h;
    }
    /* If subsequent frames report a different size, clamp to the first
       frame's size to avoid buffer overruns. */
    int use_w = w < anim->width ? w : anim->width;
    int use_h = h < anim->height ? h : anim->height;

    unsigned char *dst = anim->rgba;
    int dst_stride = anim->width * 4;

    /* Clear any rows/cols we won't write to (in case use_* < anim->*). */
    if (use_w < anim->width || use_h < anim->height) {
        memset(dst, 0, (size_t)anim->width * (size_t)anim->height * 4u);
    }

    if (n == 4) {
        for (int y = 0; y < use_h; y++) {
            memcpy(dst + (size_t)y * (size_t)dst_stride,
                   src + (size_t)y * (size_t)stride,
                   (size_t)use_w * 4u);
        }
    } else { /* n == 3 */
        for (int y = 0; y < use_h; y++) {
            const guchar *srow = src + (size_t)y * (size_t)stride;
            unsigned char *drow = dst + (size_t)y * (size_t)dst_stride;
            for (int x = 0; x < use_w; x++) {
                drow[x*4 + 0] = srow[x*3 + 0];
                drow[x*4 + 1] = srow[x*3 + 1];
                drow[x*4 + 2] = srow[x*3 + 2];
                drow[x*4 + 3] = 0xFF;
            }
        }
    }
    return true;
}

static void anim_capture_delay_and_schedule(AnimImage *anim) {
    int delay_ms = gdk_pixbuf_animation_iter_get_delay_time(anim->iter);
    if (delay_ms < 0) delay_ms = 100;       /* unknown */
    if (delay_ms < 10) delay_ms = 100;      /* spec violations: many GIFs set 0 */
    anim->frame_delay_ms = delay_ms;
    anim->next_frame_ns = SDL_GetTicksNS()
                        + (Uint64)delay_ms * (Uint64)1000000;
}

static bool load_anim(const char *path, Source *out) {
    AnimImage anim;
    memset(&anim, 0, sizeof(anim));

    GError *err = NULL;
    anim.anim = gdk_pixbuf_animation_new_from_file(path, &err);
    if (!anim.anim) {
        if (err) {
            /* not necessarily fatal: caller falls through to static loader */
            g_clear_error(&err);
        }
        return false;
    }

    if (gdk_pixbuf_animation_is_static_image(anim.anim)) {
        /* Single-frame file (e.g., regular PNG opened by extension probe).
           Tell caller to use the static path. */
        g_object_unref(anim.anim);
        return false;
    }

    anim.iter = gdk_pixbuf_animation_get_iter(anim.anim, NULL);
    if (!anim.iter) {
        g_object_unref(anim.anim);
        return false;
    }

    GdkPixbuf *frame = gdk_pixbuf_animation_iter_get_pixbuf(anim.iter);
    if (!anim_copy_pixbuf(&anim, frame)) {
        g_object_unref(anim.iter);
        g_object_unref(anim.anim);
        return false;
    }
    anim_capture_delay_and_schedule(&anim);

    out->kind = IMG_ANIM;
    out->width = anim.width;
    out->height = anim.height;
    out->v.anim = anim;
    return true;
}

/* Advance to the next frame if its display time is due. Returns true if the
   frame changed (caller should re-upload the texture and redraw). */
static bool anim_advance_if_due(AnimImage *anim) {
    Uint64 now = SDL_GetTicksNS();
    if (now < anim->next_frame_ns) return false;
    /* Pass NULL: gdk-pixbuf consults the real wall clock internally. */
    bool changed = gdk_pixbuf_animation_iter_advance(anim->iter, NULL);
    if (!changed) {
        /* No frame change yet according to gdk-pixbuf's internal clock,
           but our scheduled time has elapsed. Reschedule using the iter's
           reported delay so we wake again at the right time. */
        anim_capture_delay_and_schedule(anim);
        return false;
    }
    GdkPixbuf *frame = gdk_pixbuf_animation_iter_get_pixbuf(anim->iter);
    anim_copy_pixbuf(anim, frame);
    anim_capture_delay_and_schedule(anim);
    return true;
}

static void free_anim(AnimImage *anim) {
    if (anim->iter) g_object_unref(anim->iter);
    if (anim->anim) g_object_unref(anim->anim);
    free(anim->rgba);
    memset(anim, 0, sizeof(*anim));
}

/* Decode every frame of an animation into RGBA buffers, recording the
   per-frame delay. This is done lazily on the first pause/skim because
   it's expensive (frames × dims × 4 bytes) and most users never skim.

   The frame iterator only goes forward, so we walk it by passing
   wall-clock times in monotonic increments large enough to step.
   Returns true on success; on failure, leaves the destination arrays
   NULL and the caller proceeds without a frame cache. */
static bool decode_all_frames(GdkPixbufAnimation *animation,
                              int *out_w, int *out_h,
                              unsigned char ***out_frames,
                              int **out_delays_ms,
                              int *out_count) {
    *out_w = 0; *out_h = 0;
    *out_frames = NULL; *out_delays_ms = NULL; *out_count = 0;

    /* We need a fresh iterator started at a known time. CRITICAL:
       passing NULL to get_iter uses the current wall-clock time, which
       means any GTimeVal we later pass to advance() is interpreted as
       absolute wall-clock — and if it's smaller than that start the
       iter stays on frame 0. Pass a deterministic start time and offset
       from there. */
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    GTimeVal start_time;
    start_time.tv_sec  = 1000000;  /* arbitrary but deterministic */
    start_time.tv_usec = 0;
    GdkPixbufAnimationIter *iter =
        gdk_pixbuf_animation_get_iter(animation, &start_time);
    #pragma GCC diagnostic pop
    if (!iter) return false;

    int cap = 16;
    unsigned char **frames = malloc(sizeof(*frames) * cap);
    int *delays = malloc(sizeof(*delays) * cap);
    if (!frames || !delays) {
        free(frames); free(delays);
        g_object_unref(iter);
        return false;
    }
    int count = 0;
    int w = 0, h = 0;
    /* For looping animations, gdk-pixbuf's iter loops back to frame 0
       and would walk forever. Detect that by hashing the first frame
       and bailing once we see a matching hash later. */
    Uint64 first_frame_hash = 0;

    /* Walk frames. gdk-pixbuf returns -1 for "infinite delay" on the
       last frame of non-looping animations; we treat that as the end.
       Otherwise we step the iterator forward by exactly the reported
       delay, which deterministically yields the next frame.
       fake_time_us is measured from start_time. */
    Uint64 fake_time_us = (Uint64)start_time.tv_sec * 1000000ULL
                        + (Uint64)start_time.tv_usec;
    bool done = false;
    while (!done) {
        GdkPixbuf *pix = gdk_pixbuf_animation_iter_get_pixbuf(iter);
        if (!pix) break;
        int pw = gdk_pixbuf_get_width(pix);
        int ph = gdk_pixbuf_get_height(pix);
        int n  = gdk_pixbuf_get_n_channels(pix);
        int stride = gdk_pixbuf_get_rowstride(pix);
        const guchar *src = gdk_pixbuf_read_pixels(pix);
        if (pw <= 0 || ph <= 0 || !src || (n != 3 && n != 4)) break;
        if (w == 0) { w = pw; h = ph; }
        int use_w = pw < w ? pw : w;
        int use_h = ph < h ? ph : h;

        unsigned char *buf = calloc(1, (size_t)w * h * 4u);
        if (!buf) break;
        if (n == 4) {
            for (int y = 0; y < use_h; y++) {
                memcpy(buf + (size_t)y * w * 4u,
                       src + (size_t)y * stride,
                       (size_t)use_w * 4u);
            }
        } else {
            for (int y = 0; y < use_h; y++) {
                const guchar *srow = src + (size_t)y * stride;
                unsigned char *drow = buf + (size_t)y * w * 4u;
                for (int x = 0; x < use_w; x++) {
                    drow[x*4+0] = srow[x*3+0];
                    drow[x*4+1] = srow[x*3+1];
                    drow[x*4+2] = srow[x*3+2];
                    drow[x*4+3] = 0xFF;
                }
            }
        }

        int delay = gdk_pixbuf_animation_iter_get_delay_time(iter);
        if (delay < 0) delay = 100;
        if (delay < 10) delay = 100;

        /* Hash a few bytes to detect when the loop returns to frame 0. */
        Uint64 hash = 1469598103934665603ULL; /* FNV-64 offset basis */
        size_t hash_step = ((size_t)w * h * 4u) / 64u;
        if (hash_step < 1) hash_step = 1;
        for (size_t off = 0; off < (size_t)w * h * 4u; off += hash_step) {
            hash ^= buf[off];
            hash *= 1099511628211ULL;
        }
        if (count == 0) {
            first_frame_hash = hash;
        } else if (hash == first_frame_hash && count >= 2) {
            /* We've looped back to frame 0; stop without recording this dup. */
            free(buf);
            break;
        }

        if (count == cap) {
            int nc = cap * 2;
            unsigned char **gf = realloc(frames, sizeof(*frames) * nc);
            int *gd = realloc(delays, sizeof(*delays) * nc);
            if (!gf || !gd) { free(gf ? gf : frames); free(gd ? gd : delays); free(buf); break; }
            frames = gf; delays = gd; cap = nc;
        }
        frames[count] = buf;
        delays[count] = delay;
        count++;

        /* Step the iterator forward by exactly the current frame's delay
           plus 1ms to ensure it crosses into the next frame. We use the
           legacy GTimeVal API on purpose: the modern GDateTime path is
           not exposed by gdk_pixbuf_animation_iter_advance. */
        fake_time_us += (Uint64)delay * 1000ULL + 1000ULL;
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        GTimeVal tv;
        tv.tv_sec  = (glong)(fake_time_us / 1000000ULL);
        tv.tv_usec = (glong)(fake_time_us % 1000000ULL);
        bool changed = gdk_pixbuf_animation_iter_advance(iter, &tv);
        #pragma GCC diagnostic pop
        if (!changed) {
            /* Some loaders signal end-of-stream by not advancing. */
            done = true;
        }
        /* Cap to prevent runaway on malformed files. */
        if (count >= 4096) done = true;
    }
    g_object_unref(iter);

    if (count == 0) {
        free(frames); free(delays);
        return false;
    }

    *out_w = w; *out_h = h;
    *out_frames = frames;
    *out_delays_ms = delays;
    *out_count = count;
    return true;
}

/* ---------- generic GEGL static loader ---------- */

static bool load_static_gegl(const char *path, Source *out) {
    StaticImage st;
    memset(&st, 0, sizeof(st));

    GeglNode *graph = gegl_node_new();
    if (!graph) return false;

    GeglNode *load = gegl_node_new_child(graph,
                                         "operation", "gegl:load",
                                         "path", path, NULL);
    if (!load) {
        g_object_unref(graph);
        return false;
    }

    GeglRectangle bounds = gegl_node_get_bounding_box(load);
    if (bounds.width <= 0 || bounds.height <= 0) {
        g_object_unref(graph);
        return false;
    }
    if (bounds.width > INT32_MAX / 4 || bounds.height > INT32_MAX / 4) {
        g_object_unref(graph);
        return false;
    }

    size_t row_bytes = (size_t)bounds.width * 4u;
    size_t total_bytes = row_bytes * (size_t)bounds.height;
    if (bounds.height != 0 && total_bytes / (size_t)bounds.height != row_bytes) {
        g_object_unref(graph);
        return false;
    }

    unsigned char *pixels = malloc(total_bytes);
    if (!pixels) {
        fprintf(stderr, "%s: could not allocate %.2f MiB for image: %s\n",
                APP_NAME, (double)total_bytes / (1024.0 * 1024.0), strerror(errno));
        g_object_unref(graph);
        return false;
    }

    gegl_node_blit(load, 1.0, &bounds,
                   babl_format("R'G'B'A u8"),
                   pixels, GEGL_AUTO_ROWSTRIDE, GEGL_BLIT_DEFAULT);

    st.width = bounds.width;
    st.height = bounds.height;
    st.rgba = pixels;

    out->kind = IMG_STATIC;
    out->width = st.width;
    out->height = st.height;
    out->v.st = st;

    g_object_unref(graph);
    return true;
}

static void free_static(StaticImage *st) {
    free(st->rgba);
    memset(st, 0, sizeof(*st));
}

/* ---------- dispatch loader ---------- */

static bool load_source(const char *path, Source *out) {
    memset(out, 0, sizeof(*out));

    if (has_ext_ci(path, ".svg") || has_ext_ci(path, ".svgz")) {
        if (load_svg(path, out)) return true;
        fprintf(stderr, "%s: SVG parse failed, falling back to raster loader\n", APP_NAME);
    }
    /* Try gdk-pixbuf's animation loader for any file extension that might
       be animated. load_anim returns false (without error) for static
       single-frame files; those fall through to GEGL. */
    if (has_ext_ci(path, ".gif")  ||
        has_ext_ci(path, ".png")  || has_ext_ci(path, ".apng") ||
        has_ext_ci(path, ".webp") || has_ext_ci(path, ".avif") ||
        has_ext_ci(path, ".mng")) {
        if (load_anim(path, out)) return true;
    }
    if (load_static_gegl(path, out)) return true;
    fprintf(stderr, "%s: failed to load '%s'\n", APP_NAME, path);
    return false;
}

static void free_source(Source *src) {
    switch (src->kind) {
    case IMG_STATIC: free_static(&src->v.st);   break;
    case IMG_SVG:    free_svg(&src->v.svg);     break;
    case IMG_ANIM:   free_anim(&src->v.anim);   break;
    }
    memset(src, 0, sizeof(*src));
}

/* ---------- directory listing for arrow-key navigation ---------- */

/* Comma-separated list of extensions we'll try to load. Anything not
   on this list is skipped during directory listing. */
static const char *const IMAGE_EXTS[] = {
    ".png",  ".jpg",  ".jpeg", ".jpe",   ".jfif",
    ".gif",  ".apng", ".webp", ".avif",  ".mng",
    ".bmp",  ".tif",  ".tiff", ".tga",
    ".ppm",  ".pgm",  ".pbm",  ".pnm",
    ".psd",  ".svg",  ".svgz",
    ".hdr",  ".exr",
    NULL
};

static bool is_image_ext(const char *name) {
    for (int i = 0; IMAGE_EXTS[i]; i++) {
        if (has_ext_ci(name, IMAGE_EXTS[i])) return true;
    }
    return false;
}

static int strcmp_case_insensitive(const void *pa, const void *pb) {
    const char *a = *(const char *const *)pa;
    const char *b = *(const char *const *)pb;
    /* Compare basenames, locale-insensitive ASCII fold. */
    const char *sa = strrchr(a, '/'); sa = sa ? sa + 1 : a;
    const char *sb = strrchr(b, '/'); sb = sb ? sb + 1 : b;
    while (*sa && *sb) {
        int ca = tolower((unsigned char)*sa);
        int cb = tolower((unsigned char)*sb);
        if (ca != cb) return ca - cb;
        sa++; sb++;
    }
    return (unsigned char)*sa - (unsigned char)*sb;
}

static void dirlist_free(DirList *dl) {
    if (!dl) return;
    for (int i = 0; i < dl->count; i++) free(dl->entries[i]);
    free(dl->entries);
    free(dl->base_dir);
    memset(dl, 0, sizeof(*dl));
}

/* Populate `dl` with sorted image files in `dir_path`. Returns true on
   success (even if the directory is empty). */
static bool dirlist_load(DirList *dl, const char *dir_path) {
    dirlist_free(dl);
    DIR *d = opendir(dir_path);
    if (!d) return false;

    int cap = 32;
    dl->entries = malloc(sizeof(char *) * cap);
    if (!dl->entries) { closedir(d); return false; }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;     /* skip hidden + . / .. */
        if (!is_image_ext(de->d_name)) continue;

        /* Build full path. */
        size_t dpl = strlen(dir_path);
        size_t npl = strlen(de->d_name);
        bool need_slash = (dpl > 0 && dir_path[dpl - 1] != '/');
        size_t total = dpl + (need_slash ? 1 : 0) + npl + 1;
        char *full = malloc(total);
        if (!full) continue;
        if (need_slash) {
            snprintf(full, total, "%s/%s", dir_path, de->d_name);
        } else {
            snprintf(full, total, "%s%s", dir_path, de->d_name);
        }

        if (dl->count == cap) {
            int new_cap = cap * 2;
            char **grown = realloc(dl->entries, sizeof(char *) * new_cap);
            if (!grown) { free(full); break; }
            dl->entries = grown;
            cap = new_cap;
        }
        dl->entries[dl->count++] = full;
    }
    closedir(d);

    qsort(dl->entries, dl->count, sizeof(char *), strcmp_case_insensitive);

    dl->base_dir = strdup(dir_path);
    dl->current = 0;
    return true;
}

/* Populate `dl` with the given list of file paths verbatim. Used to
   build an ad-hoc playlist from multiple CLI args or from a multi-file
   drag-drop. Unlike dirlist_load() this does NOT scan a directory, does
   NOT sort (caller-supplied order is preserved — usually the order the
   user typed or dropped them), and does NOT filter by extension (the
   user explicitly asked for these files, so trust them).

   Paths are duplicated into the DirList so the caller can free its
   own storage immediately. Returns true on success; on OOM, partial
   state is freed and dl is left empty. */
static bool dirlist_from_paths(DirList *dl, const char *const *paths, int count) {
    dirlist_free(dl);
    if (count <= 0) return true;
    dl->entries = malloc(sizeof(char *) * (size_t)count);
    if (!dl->entries) return false;
    for (int i = 0; i < count; i++) {
        dl->entries[i] = strdup(paths[i]);
        if (!dl->entries[i]) {
            for (int k = 0; k < i; k++) free(dl->entries[k]);
            free(dl->entries);
            dl->entries = NULL;
            dl->count = 0; /* reset so a later dirlist_free won't
                              iterate over the freed array */
            return false;
        }
        dl->count++;
    }
    dl->current = 0;
    dl->base_dir = NULL; /* not anchored to a single directory */
    return true;
}

/* Returns true if `path` refers to an existing directory. */
static bool is_directory(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

/* Convert a byte count into a human-readable string like "1.2 MB". */
static void human_size(size_t bytes, char *out, size_t out_size) {
    static const char *const units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; u++; }
    if (u == 0) snprintf(out, out_size, "%zu %s", bytes, units[u]);
    else        snprintf(out, out_size, "%.2f %s", v, units[u]);
}

/* Best-guess format name from the loader-kind + the filename extension.
   Used for the info overlay; deliberately concise. */
static const char *guess_format(const char *path, const Source *src) {
    if (!path) return "(none)";
    /* The loader kind tells us the broad family. */
    switch (src->kind) {
    case IMG_SVG:  return "SVG";
    case IMG_ANIM: {
        size_t pl = strlen(path);
        if (pl >= 4 && strcasecmp(path + pl - 4, ".gif")  == 0) return "GIF";
        if (pl >= 5 && strcasecmp(path + pl - 5, ".apng") == 0) return "APNG";
        if (pl >= 5 && strcasecmp(path + pl - 5, ".webp") == 0) return "WebP (animated)";
        if (pl >= 5 && strcasecmp(path + pl - 5, ".avif") == 0) return "AVIF (animated)";
        if (pl >= 4 && strcasecmp(path + pl - 4, ".mng")  == 0) return "MNG";
        if (pl >= 4 && strcasecmp(path + pl - 4, ".png")  == 0) return "PNG (animated)";
        return "Animated";
    }
    case IMG_STATIC: {
        const char *dot = strrchr(path, '.');
        if (!dot) return "Raster";
        if (strcasecmp(dot, ".png")  == 0) return "PNG";
        if (strcasecmp(dot, ".jpg")  == 0 ||
            strcasecmp(dot, ".jpeg") == 0 ||
            strcasecmp(dot, ".jpe")  == 0 ||
            strcasecmp(dot, ".jfif") == 0) return "JPEG";
        if (strcasecmp(dot, ".webp") == 0) return "WebP";
        if (strcasecmp(dot, ".avif") == 0) return "AVIF";
        if (strcasecmp(dot, ".bmp")  == 0) return "BMP";
        if (strcasecmp(dot, ".tif")  == 0 ||
            strcasecmp(dot, ".tiff") == 0) return "TIFF";
        if (strcasecmp(dot, ".tga")  == 0) return "TGA";
        if (strcasecmp(dot, ".psd")  == 0) return "PSD";
        if (strcasecmp(dot, ".hdr")  == 0) return "HDR";
        if (strcasecmp(dot, ".exr")  == 0) return "EXR";
        if (strcasecmp(dot, ".ppm")  == 0 ||
            strcasecmp(dot, ".pgm")  == 0 ||
            strcasecmp(dot, ".pbm")  == 0 ||
            strcasecmp(dot, ".pnm")  == 0) return "PNM";
        return "Raster";
    }
    }
    return "(unknown)";
}

/* Rebuild the info-overlay lines from the current state. Called whenever
   the image changes (open, drop, nav) or playback state changes. */
static void rebuild_info_lines(Viewer *viewer) {
    viewer->info_line_count = 0;
    if (!viewer->current_path) {
        snprintf(viewer->info_lines[viewer->info_line_count++], 256,
                 "(no image)");
        return;
    }

    /* Split the path into directory + file, and collapse $HOME to ~ in
       the directory so the line stays short for files under the user's
       home tree. */
    const char *fullpath = viewer->current_path;
    const char *slash = strrchr(fullpath, '/');
    char dirbuf[512];
    const char *filename = NULL;
    if (slash) {
        size_t dlen = (size_t)(slash - fullpath);
        if (dlen == 0) {
            /* path is "/foo" -> dir = "/" */
            snprintf(dirbuf, sizeof(dirbuf), "/");
        } else {
            if (dlen >= sizeof(dirbuf)) dlen = sizeof(dirbuf) - 1;
            memcpy(dirbuf, fullpath, dlen);
            dirbuf[dlen] = '\0';
        }
        filename = slash + 1;
    } else {
        /* No slash -> just a bare filename, no directory. */
        dirbuf[0] = '.';
        dirbuf[1] = '\0';
        filename = fullpath;
    }

    /* Collapse $HOME -> ~. Sized to leave room for the "Dir:  " prefix
       inside a 256-byte info line. */
    const char *home = getenv("HOME");
    char dirshown[240];
    if (home && *home) {
        size_t hlen = strlen(home);
        size_t dlen = strlen(dirbuf);
        if (dlen >= hlen && strncmp(dirbuf, home, hlen) == 0 &&
            (dirbuf[hlen] == '\0' || dirbuf[hlen] == '/')) {
            snprintf(dirshown, sizeof(dirshown), "~%s", dirbuf + hlen);
        } else {
            snprintf(dirshown, sizeof(dirshown), "%s", dirbuf);
        }
    } else {
        snprintf(dirshown, sizeof(dirshown), "%s", dirbuf);
    }

    snprintf(viewer->info_lines[viewer->info_line_count++], 256,
             "Dir:  %s", dirshown);
    snprintf(viewer->info_lines[viewer->info_line_count++], 256,
             "File: %s", filename ? filename : "");

    /* file size */
    char size_buf[64];
    human_size(viewer->current_size_bytes, size_buf, sizeof(size_buf));
    snprintf(viewer->info_lines[viewer->info_line_count++], 256,
             "Size: %s", size_buf);

    /* dims */
    snprintf(viewer->info_lines[viewer->info_line_count++], 256,
             "Dimensions: %d x %d", viewer->src.width, viewer->src.height);

    /* format */
    snprintf(viewer->info_lines[viewer->info_line_count++], 256,
             "Format: %s", guess_format(viewer->current_path, &viewer->src));

    /* animated? */
    if (viewer->src.kind == IMG_ANIM) {
        if (viewer->frames_decoded) {
            snprintf(viewer->info_lines[viewer->info_line_count++], 256,
                     "Animated: yes (%d frames, frame %d)",
                     viewer->frame_count, viewer->current_frame + 1);
        } else {
            snprintf(viewer->info_lines[viewer->info_line_count++], 256,
                     "Animated: yes");
        }
    } else {
        snprintf(viewer->info_lines[viewer->info_line_count++], 256,
                 "Animated: no");
    }

    /* zoom */
    snprintf(viewer->info_lines[viewer->info_line_count++], 256,
             "Zoom: %.2fx", viewer->zoom);

    /* directory position (if applicable) */
    if (viewer->dir.count > 0) {
        snprintf(viewer->info_lines[viewer->info_line_count++], 256,
                 "Dir: %d / %d", viewer->dir.current + 1, viewer->dir.count);
    }
}

/* ---------- sizing & SDL setup ---------- */

static void choose_initial_size(int img_w, int img_h, int display_w, int display_h,
                                int *window_w, int *window_h) {
    int usable_w = display_w > WINDOW_MARGIN ? display_w - WINDOW_MARGIN : display_w;
    int usable_h = display_h > WINDOW_MARGIN ? display_h - WINDOW_MARGIN : display_h;
    usable_w = clamp_int(usable_w, 1, display_w > 0 ? display_w : 1);
    usable_h = clamp_int(usable_h, 1, display_h > 0 ? display_h : 1);

    double scale = 1.0;
    if (img_w > usable_w || img_h > usable_h) {
        double sx = (double)usable_w / (double)img_w;
        double sy = (double)usable_h / (double)img_h;
        scale = sx < sy ? sx : sy;
    }
    if (scale <= 0.0 || !isfinite(scale)) scale = 1.0;

    int w = (int)floor((double)img_w * scale + 0.5);
    int h = (int)floor((double)img_h * scale + 0.5);
    w = clamp_int(w, 1, usable_w);
    h = clamp_int(h, 1, usable_h);

    *window_w = w;
    *window_h = h;
}

static bool ensure_texture(Viewer *viewer, int w, int h) {
    if (viewer->texture && viewer->texture_w == w && viewer->texture_h == h) {
        return true;
    }
    if (viewer->texture) {
        SDL_DestroyTexture(viewer->texture);
        viewer->texture = NULL;
    }
    viewer->texture = SDL_CreateTexture(viewer->renderer,
                                        SDL_PIXELFORMAT_RGBA32,
                                        SDL_TEXTUREACCESS_STATIC,
                                        w, h);
    if (!viewer->texture) {
        die_sdl("SDL_CreateTexture");
        viewer->texture_w = 0;
        viewer->texture_h = 0;
        return false;
    }
    SDL_SetTextureScaleMode(viewer->texture,
                            viewer->nearest ? SDL_SCALEMODE_NEAREST
                                            : SDL_SCALEMODE_LINEAR);
    viewer->texture_w = w;
    viewer->texture_h = h;
    return true;
}

static void try_set_window_icon(Viewer *viewer);
static void free_frame_cache(Viewer *viewer);
static void apply_aspect_lock(Viewer *viewer);

static bool create_sdl(Viewer *viewer, const char *title) {
    SDL_DisplayID display = SDL_GetPrimaryDisplay();
    int display_w = 1920, display_h = 1080;
    if (display != 0) {
        SDL_Rect bounds;
        if (SDL_GetDisplayUsableBounds(display, &bounds) && bounds.w > 0 && bounds.h > 0) {
            display_w = bounds.w;
            display_h = bounds.h;
        } else {
            const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(display);
            if (mode && mode->w > 0 && mode->h > 0) {
                display_w = mode->w;
                display_h = mode->h;
            }
        }
    }

    /* If no image was loaded yet, pick a reasonable default window size. */
    if (viewer->src.width > 0 && viewer->src.height > 0) {
        choose_initial_size(viewer->src.width, viewer->src.height,
                            display_w, display_h,
                            &viewer->initial_window_w,
                            &viewer->initial_window_h);
    } else {
        viewer->initial_window_w = display_w > 800 ? 800 : display_w;
        viewer->initial_window_h = display_h > 600 ? 600 : display_h;
    }
    viewer->zoom = (viewer->opts ? viewer->opts->initial_zoom : 1.0);
    if (viewer->zoom <= 0.0 || !isfinite(viewer->zoom)) viewer->zoom = 1.0;
    viewer->anchor_x = 0.5;
    viewer->pan_off_x = 0.0;
    viewer->anchor_y = 0.5;
    viewer->pan_off_y = 0.0;
    viewer->nearest  = (viewer->opts ? viewer->opts->start_nearest : false);
    viewer->aspect_lock = !(viewer->opts && viewer->opts->no_aspect_lock);

    Uint32 win_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (viewer->opts && viewer->opts->start_fullscreen) {
        win_flags |= SDL_WINDOW_FULLSCREEN;
        viewer->fullscreen = true;
    }

    viewer->window = SDL_CreateWindow(title,
                                      viewer->initial_window_w,
                                      viewer->initial_window_h,
                                      win_flags);
    if (!viewer->window) {
        die_sdl("SDL_CreateWindow");
        return false;
    }

    apply_aspect_lock(viewer);
    SDL_SetWindowMinimumSize(viewer->window, 1, 1);
    /* Set the X11/XWayland icon if a hicolor icon is installed. Wayland
       ignores this and uses the .desktop file's Icon= instead. */
    try_set_window_icon(viewer);

    viewer->renderer = SDL_CreateRenderer(viewer->window, NULL);
    if (!viewer->renderer) {
        die_sdl("SDL_CreateRenderer");
        return false;
    }

    /* Pango / cairo text stack: ask the system for its default font map
       (singleton, no unref), build a font description honoring whatever
       Sans the user has configured via fontconfig, and create a context
       used to lay out short strings later.

       NOTE: this must happen unconditionally — including the no-image
       blank-window case — because draw_text() short-circuits when
       pango_ctx is NULL. Initializing it only on the eagerly-loaded
       cmdline path made the info / cheat-sheet overlays render as
       tofu (empty boxes) after drag-dropping into a blank window. */
    viewer->pango_fontmap = pango_cairo_font_map_get_default();
    viewer->pango_ctx = pango_font_map_create_context(viewer->pango_fontmap);

    /* Force grayscale antialiasing for our offscreen text surfaces.
       If the user's fontconfig enables LCD-subpixel rendering (rgba:
       rgb / bgr / vrgb / vbgr) — common on Linux desktops — cairo
       happily honors it when rasterizing to an offscreen ARGB32
       buffer. The result has colored per-channel coverage at glyph
       edges that's only correct when blitted 1:1 onto a real LCD
       window with the matching subpixel order. We upload as a regular
       RGBA texture and let SDL composite it, so the subpixel coverage
       shows up as green/magenta fringing. CAIRO_ANTIALIAS_GRAY tells
       cairo to use plain coverage, which composites cleanly. */
    if (viewer->pango_ctx) {
        cairo_font_options_t *fo = cairo_font_options_create();
        cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_GRAY);
        cairo_font_options_set_subpixel_order(fo, CAIRO_SUBPIXEL_ORDER_DEFAULT);
        pango_cairo_context_set_font_options(viewer->pango_ctx, fo);
        cairo_font_options_destroy(fo);
    }

    /* For static, GIF, and SVG: create the base texture at natural
       size and upload its pixels. SVGs use the same upload path
       because load_svg already rasterized the base raster (the
       sharp-tile pyramid then adds an overlay on top). With no
       image loaded, skip texture setup; the blank window simply
       paints the background color. */
    if (viewer->src.width <= 0 || viewer->src.height <= 0) {
        rebuild_info_lines(viewer);
        return true;
    }

    switch (viewer->src.kind) {
    case IMG_STATIC: {
        if (!ensure_texture(viewer, viewer->src.v.st.width, viewer->src.v.st.height)) {
            return false;
        }
        if (!SDL_UpdateTexture(viewer->texture, NULL,
                               viewer->src.v.st.rgba,
                               viewer->src.v.st.width * 4)) {
            die_sdl("SDL_UpdateTexture");
            return false;
        }
        break;
    }
    case IMG_ANIM: {
        AnimImage *a = &viewer->src.v.anim;
        if (!ensure_texture(viewer, a->width, a->height)) {
            return false;
        }
        if (!SDL_UpdateTexture(viewer->texture, NULL, a->rgba, a->width * 4)) {
            die_sdl("SDL_UpdateTexture");
            return false;
        }
        break;
    }
    case IMG_SVG: {
        /* The SVG was already rasterized to a static RGBA buffer in
           load_svg; upload it like any other static image. */
        SvgImage *svg = &viewer->src.v.svg;
        if (!ensure_texture(viewer, svg->width, svg->height)) {
            return false;
        }
        if (!SDL_UpdateTexture(viewer->texture, NULL,
                               svg->rgba, svg->width * 4)) {
            die_sdl("SDL_UpdateTexture");
            return false;
        }
        break;
    }
    }

    rebuild_info_lines(viewer);
    return true;
}

/* Try to find and apply a window icon (X11/XWayland only — Wayland
   compositors ignore this and use the .desktop file's Icon= line via
   the window's app_id). Returns true on success, false otherwise.
   Search order tries common freedesktop paths under XDG_DATA_HOME,
   /usr/local, and /usr. Both PNG and SVG icons are accepted. */
static bool try_load_icon_rgba(const char *path,
                               unsigned char **out_rgba,
                               int *out_w, int *out_h) {
    *out_rgba = NULL; *out_w = 0; *out_h = 0;

    /* SVG: rasterize at a sensible default icon size via librsvg. */
    size_t pl = strlen(path);
    bool is_svg = (pl >= 4 &&
                   (strcasecmp(path + pl - 4, ".svg") == 0));
    if (is_svg) {
        GError *err = NULL;
        RsvgHandle *handle = rsvg_handle_new_from_file(path, &err);
        if (!handle) { if (err) g_error_free(err); return false; }
        double nw, nh;
        if (!svg_get_natural_size(handle, &nw, &nh) || nw <= 0.0 || nh <= 0.0) {
            g_object_unref(handle);
            return false;
        }
        int target = 64;
        double sw = (double)target / nw;
        double sh = (double)target / nh;
        double scale = sw < sh ? sw : sh;
        int w = (int)ceil(nw * scale);
        int h = (int)ceil(nh * scale);
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        unsigned char *buf = calloc(1, (size_t)w * h * 4u);
        if (!buf) { g_object_unref(handle); return false; }
        if (!svg_render_region(handle, nw, nh, 0.0, 0.0, scale, buf, w, h)) {
            free(buf);
            g_object_unref(handle);
            return false;
        }
        cairo_argb32_to_rgba_inplace(buf, w, h);
        g_object_unref(handle);
        *out_rgba = buf; *out_w = w; *out_h = h;
        return true;
    }
    /* PNG/other raster via gdk-pixbuf. */
    GdkPixbuf *pix = gdk_pixbuf_new_from_file(path, NULL);
    if (!pix) return false;
    int w = gdk_pixbuf_get_width(pix);
    int h = gdk_pixbuf_get_height(pix);
    int n = gdk_pixbuf_get_n_channels(pix);
    int stride = gdk_pixbuf_get_rowstride(pix);
    const guchar *src = gdk_pixbuf_read_pixels(pix);
    if (w <= 0 || h <= 0 || !src || (n != 3 && n != 4)) {
        g_object_unref(pix);
        return false;
    }
    unsigned char *buf = malloc((size_t)w * h * 4u);
    if (!buf) { g_object_unref(pix); return false; }
    if (n == 4) {
        for (int y = 0; y < h; y++) {
            memcpy(buf + (size_t)y * w * 4u,
                   src + (size_t)y * stride,
                   (size_t)w * 4u);
        }
    } else {
        for (int y = 0; y < h; y++) {
            const guchar *srow = src + (size_t)y * stride;
            unsigned char *drow = buf + (size_t)y * w * 4u;
            for (int x = 0; x < w; x++) {
                drow[x*4+0] = srow[x*3+0];
                drow[x*4+1] = srow[x*3+1];
                drow[x*4+2] = srow[x*3+2];
                drow[x*4+3] = 0xFF;
            }
        }
    }
    g_object_unref(pix);
    *out_rgba = buf; *out_w = w; *out_h = h;
    return true;
}

static void try_set_window_icon(Viewer *viewer) {
    static const char *const candidates[] = {
        /* SVG (any size, best fidelity). */
        "%s/icons/hicolor/scalable/apps/weh.svg",
        /* PNGs in descending size order. */
        "%s/icons/hicolor/512x512/apps/weh.png",
        "%s/icons/hicolor/256x256/apps/weh.png",
        "%s/icons/hicolor/128x128/apps/weh.png",
        "%s/icons/hicolor/64x64/apps/weh.png",
        "%s/icons/hicolor/48x48/apps/weh.png",
        "%s/icons/hicolor/32x32/apps/weh.png",
        NULL,
    };
    const char *prefixes[8];
    int np = 0;
    const char *xdg = getenv("XDG_DATA_HOME");
    char xdg_buf[1024] = {0};
    if (xdg && *xdg) {
        prefixes[np++] = xdg;
    } else {
        const char *home = getenv("HOME");
        if (home && *home) {
            snprintf(xdg_buf, sizeof(xdg_buf), "%s/.local/share", home);
            prefixes[np++] = xdg_buf;
        }
    }
    prefixes[np++] = "/usr/local/share";
    prefixes[np++] = "/usr/share";

    for (int p = 0; p < np; p++) {
        for (int c = 0; candidates[c]; c++) {
            char path[1024];
            snprintf(path, sizeof(path), candidates[c], prefixes[p]);
            unsigned char *rgba = NULL;
            int w = 0, h = 0;
            if (try_load_icon_rgba(path, &rgba, &w, &h)) {
                SDL_Surface *surf = SDL_CreateSurfaceFrom(
                    w, h, SDL_PIXELFORMAT_RGBA32, rgba, w * 4);
                if (surf) {
                    SDL_SetWindowIcon(viewer->window, surf);
                    SDL_DestroySurface(surf);
                }
                free(rgba);
                return;
            }
        }
    }
}

static void destroy_viewer(Viewer *viewer) {
    free_frame_cache(viewer);
    dirlist_free(&viewer->dir);
    free(viewer->current_path);
    if (viewer->pango_ctx)       g_object_unref(viewer->pango_ctx);
    /* PangoFontMap returned by pango_cairo_font_map_get_default() is
       singleton — DO NOT unref. */
    if (viewer->texture) SDL_DestroyTexture(viewer->texture);
    if (viewer->renderer) SDL_DestroyRenderer(viewer->renderer);
    if (viewer->window) SDL_DestroyWindow(viewer->window);
    free_source(&viewer->src);
    memset(viewer, 0, sizeof(*viewer));
}

/* Free the lazy all-frames cache (rebuilt on demand for skim/pause). */
static void free_frame_cache(Viewer *viewer) {
    if (viewer->frames) {
        for (int i = 0; i < viewer->frame_count; i++) free(viewer->frames[i]);
        free(viewer->frames);
    }
    free(viewer->frame_delays_ms);
    viewer->frames = NULL;
    viewer->frame_delays_ms = NULL;
    viewer->frame_count = 0;
    viewer->frame_w = viewer->frame_h = 0;
    viewer->current_frame = 0;
    viewer->frames_decoded = false;
}

/* Drop GPU resources tied to the current Source. Used internally by
   reopen_image and during teardown. Also drops the SVG sharp-tile
   texture (free_svg destroys the tile_tex itself, but this helper is
   called BEFORE free_svg on the new path's failure-rollback branch,
   so we proactively destroy here as well). */
static void release_source_gpu(Viewer *viewer) {
    if (viewer->src.kind == IMG_SVG) {
        SvgImage *svg = &viewer->src.v.svg;
        if (svg->tile_tex) {
            SDL_DestroyTexture(svg->tile_tex);
            svg->tile_tex = NULL;
            svg->tile_tex_w = 0;
            svg->tile_tex_h = 0;
            svg->tile_valid = false;
        }
    }
    if (viewer->texture) {
        SDL_DestroyTexture(viewer->texture);
        viewer->texture = NULL;
        viewer->texture_w = 0;
        viewer->texture_h = 0;
    }
}

/* Look up the usable bounds of whatever display the window currently
   sits on, falling back to the primary display, then to a 1080p
   default if the system doesn't tell us anything sensible. Returns
   true if the bounds came from a real display, false if it's the
   fallback (caller usually doesn't care). */
static bool get_window_display_bounds(SDL_Window *win, SDL_Rect *out) {
    out->x = 0; out->y = 0; out->w = 1920; out->h = 1080;
    SDL_DisplayID disp = win ? SDL_GetDisplayForWindow(win) : 0;
    if (disp == 0) disp = SDL_GetPrimaryDisplay();
    if (disp == 0) return false;
    SDL_Rect b;
    if (!SDL_GetDisplayUsableBounds(disp, &b) || b.w <= 0 || b.h <= 0) {
        return false;
    }
    *out = b;
    return true;
}

/* Pick a window size that fits the new image's aspect into roughly
   the same screen area the current window occupies, capped at the
   display's usable bounds (minus WINDOW_MARGIN). "Preserve area"
   keeps the user's coarse size choice — small stays small, big stays
   big — while the cap kills the death spiral you get when SDL clamps
   a wrong-aspect window in place across navigations between tall and
   wide images. Inputs must be > 0; outputs are clamped to >= 1. */
static void fit_window_to_aspect(int cur_w, int cur_h,
                                 double new_aspect,
                                 int disp_w, int disp_h,
                                 int *out_w, int *out_h) {
    double area = (double)cur_w * (double)cur_h;
    double h = sqrt(area / new_aspect);
    double w = h * new_aspect;
    int tw = (int)floor(w + 0.5);
    int th = (int)floor(h + 0.5);

    int cap_w = disp_w > WINDOW_MARGIN ? disp_w - WINDOW_MARGIN : disp_w;
    int cap_h = disp_h > WINDOW_MARGIN ? disp_h - WINDOW_MARGIN : disp_h;
    if (tw > cap_w) {
        tw = cap_w;
        th = (int)floor((double)cap_w / new_aspect + 0.5);
    }
    if (th > cap_h) {
        th = cap_h;
        tw = (int)floor((double)cap_h * new_aspect + 0.5);
    }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;

    *out_w = tw;
    *out_h = th;
}

/* Open a new image file in-place, preserving the existing window/renderer.
   Used by drag-and-drop and directory navigation. Returns true on success;
   on failure the previous image (if any) is restored. */
static bool reopen_image(Viewer *viewer, const char *path) {
    release_source_gpu(viewer);
    free_frame_cache(viewer);
    /* Stop the SVG worker (if any) BEFORE copying the Source struct.
       The worker was started with a pointer to viewer->src.v.svg; if
       we let it keep running while we copy the struct out, the worker
       will read/write a memory slot that's about to be overwritten by
       load_source, and the stop signal we eventually send will go to
       the wrong copy. */
    if (viewer->src.kind == IMG_SVG) {
        svg_stop_worker(&viewer->src.v.svg);
    }
    Source old = viewer->src;
    memset(&viewer->src, 0, sizeof(viewer->src));

    if (!load_source(path, &viewer->src)) {
        /* Restore the old source on failure so we don't end up with nothing. */
        viewer->src = old;
        switch (viewer->src.kind) {
        case IMG_STATIC:
            if (ensure_texture(viewer,
                               viewer->src.v.st.width,
                               viewer->src.v.st.height)) {
                SDL_UpdateTexture(viewer->texture, NULL,
                                  viewer->src.v.st.rgba,
                                  viewer->src.v.st.width * 4);
            }
            break;
        case IMG_ANIM:
            if (ensure_texture(viewer,
                               viewer->src.v.anim.width,
                               viewer->src.v.anim.height)) {
                SDL_UpdateTexture(viewer->texture, NULL,
                                  viewer->src.v.anim.rgba,
                                  viewer->src.v.anim.width * 4);
            }
            break;
        case IMG_SVG:
            if (ensure_texture(viewer,
                               viewer->src.v.svg.width,
                               viewer->src.v.svg.height)) {
                SDL_UpdateTexture(viewer->texture, NULL,
                                  viewer->src.v.svg.rgba,
                                  viewer->src.v.svg.width * 4);
            }
            /* We stopped the worker above; restart it now that the
               SvgImage is back in viewer->src.v.svg. */
            svg_start_worker(&viewer->src.v.svg);
            break;
        }
        return false;
    }

    /* Successfully loaded. Free the old image. */
    free_source(&old);

    /* Update path + file size. */
    free(viewer->current_path);
    viewer->current_path = strdup(path);
    {
        struct stat st;
        viewer->current_size_bytes = (stat(path, &st) == 0) ? (size_t)st.st_size : 0;
    }

    /* Update window title for the new image. */
    SDL_SetWindowTitle(viewer->window,
                       (viewer->opts && viewer->opts->title)
                         ? viewer->opts->title : path);

    /* Refit the window to the new image BEFORE re-applying the aspect
       lock, then recenter so the window stays anchored on its previous
       center. Skipping the refit makes navigation between images of
       different aspects spiral the window smaller: SDL clamps the new
       aspect into the old bounds, shrinking one axis; the next image
       then shrinks the other. fit_window_to_aspect() preserves the
       window's current area (so "small stays small, big stays big")
       and caps at the display's usable bounds (kills the spiral).
       Fullscreen windows are left untouched. */
    if (viewer->window && !viewer->fullscreen
        && viewer->src.width > 0 && viewer->src.height > 0) {
        int cur_w = 0, cur_h = 0;
        SDL_GetWindowSize(viewer->window, &cur_w, &cur_h);
        if (cur_w > 0 && cur_h > 0) {
            SDL_Rect db;
            get_window_display_bounds(viewer->window, &db);

            double new_aspect = (double)viewer->src.width
                              / (double)viewer->src.height;
            if (viewer->rotation_steps == 1 || viewer->rotation_steps == 3) {
                new_aspect = 1.0 / new_aspect;
            }

            int tw, th;
            fit_window_to_aspect(cur_w, cur_h, new_aspect,
                                 db.w, db.h, &tw, &th);

            /* Anchor the resize on the window's current center.
               SDL_SetWindowSize keeps the top-left pinned by default,
               so without this the new window jumps up to the old
               top-left. Capture center, resize, push position back,
               and clamp inside the usable display bounds so we can't
               end up off-screen or under a panel. */
            int old_x = 0, old_y = 0;
            SDL_GetWindowPosition(viewer->window, &old_x, &old_y);
            int cx = old_x + cur_w / 2;
            int cy = old_y + cur_h / 2;

            /* Clear the aspect constraint so SetWindowSize isn't
               clamped to the previous (wrong) aspect; apply_aspect_lock
               at the end reinstates it. */
            SDL_SetWindowAspectRatio(viewer->window, 0.0f, 0.0f);
            SDL_SetWindowSize(viewer->window, tw, th);

            int nx = cx - tw / 2;
            int ny = cy - th / 2;
            if (nx < db.x) nx = db.x;
            if (ny < db.y) ny = db.y;
            if (nx + tw > db.x + db.w) nx = db.x + db.w - tw;
            if (ny + th > db.y + db.h) ny = db.y + db.h - th;
            SDL_SetWindowPosition(viewer->window, nx, ny);
        }
    }
    apply_aspect_lock(viewer);

    /* Per-kind GPU resource setup, mirroring create_sdl(). */
    switch (viewer->src.kind) {
    case IMG_STATIC:
        if (ensure_texture(viewer,
                           viewer->src.v.st.width,
                           viewer->src.v.st.height)) {
            SDL_UpdateTexture(viewer->texture, NULL,
                              viewer->src.v.st.rgba,
                              viewer->src.v.st.width * 4);
        }
        break;
    case IMG_ANIM:
        if (ensure_texture(viewer,
                           viewer->src.v.anim.width,
                           viewer->src.v.anim.height)) {
            SDL_UpdateTexture(viewer->texture, NULL,
                              viewer->src.v.anim.rgba,
                              viewer->src.v.anim.width * 4);
        }
        break;
    case IMG_SVG: {
        SvgImage *svg = &viewer->src.v.svg;
        if (ensure_texture(viewer, svg->width, svg->height)) {
            SDL_UpdateTexture(viewer->texture, NULL,
                              svg->rgba, svg->width * 4);
        }
        break;
    }
    }

    /* Reset per-image view state. Per spec: zoom/anchor/flip/rotate/skim
       all reset on a new image; tile mode is preserved if currently on. */
    viewer->zoom = 1.0;
    viewer->anchor_x = 0.5;
    viewer->pan_off_x = 0.0;
    viewer->anchor_y = 0.5;
    viewer->pan_off_y = 0.0;
    viewer->flip_h = false;
    viewer->flip_v = false;
    viewer->rotation_steps = 0;
    viewer->paused = false;

    rebuild_info_lines(viewer);
    return true;
}

/* Try to open `path`. If it's a directory, populate the dir list and open
   the first image; otherwise open the file directly (no dir nav). Used by
   both startup and drag-drop. Returns true on success. */
static bool open_path(Viewer *viewer, const char *path) {
    if (is_directory(path)) {
        DirList dl;
        memset(&dl, 0, sizeof(dl));
        if (!dirlist_load(&dl, path)) {
            fprintf(stderr, "%s: cannot read directory '%s'\n", APP_NAME, path);
            return false;
        }
        if (dl.count == 0) {
            fprintf(stderr, "%s: no images in directory '%s'\n", APP_NAME, path);
            dirlist_free(&dl);
            return false;
        }
        if (!reopen_image(viewer, dl.entries[0])) {
            dirlist_free(&dl);
            return false;
        }
        dirlist_free(&viewer->dir);
        viewer->dir = dl;
        viewer->dir.current = 0;
        rebuild_info_lines(viewer);
        return true;
    }
    /* Single file: clear any existing dir list (user explicitly chose
       one file; spec says don't auto-discover its parent dir). */
    dirlist_free(&viewer->dir);
    return reopen_image(viewer, path);
}

/* Move to the directory list entry at index i, wrapping. No-op if dir
   list is empty. */
static bool nav_to(Viewer *viewer, int i) {
    if (viewer->dir.count <= 0) return false;
    int n = viewer->dir.count;
    int idx = ((i % n) + n) % n;
    /* Skip files that fail to load (broken file in the dir shouldn't
       lock us out of the rest). */
    int tried = 0;
    while (tried < n) {
        if (reopen_image(viewer, viewer->dir.entries[idx])) {
            viewer->dir.current = idx;
            rebuild_info_lines(viewer);
            return true;
        }
        fprintf(stderr, "%s: skipping unreadable file '%s'\n",
                APP_NAME, viewer->dir.entries[idx]);
        idx = (idx + 1) % n;
        tried++;
    }
    return false;
}

/* ---------- render ---------- */

/* Returns the scale factor for converting window-unit deltas (the units
   used by mouse events and pan_off_*) to renderer-pixel units. On HiDPI
   displays the renderer output is larger than the window in screen-unit
   terms. */
static void window_to_pixel_scale(const Viewer *viewer,
                                  double *out_sx, double *out_sy) {
    int win_w = 1, win_h = 1;
    int pix_w = 1, pix_h = 1;
    SDL_GetWindowSize(viewer->window, &win_w, &win_h);
    SDL_GetCurrentRenderOutputSize(viewer->renderer, &pix_w, &pix_h);
    *out_sx = (win_w > 0) ? (double)pix_w / (double)win_w : 1.0;
    *out_sy = (win_h > 0) ? (double)pix_h / (double)win_h : 1.0;
}

/* Build a transient PangoFontDescription from "Sans" + size + bold. The
   "Sans" family alias resolves via fontconfig to whatever the user
   configured as their generic Sans. Returns NULL on OOM; caller frees
   with pango_font_description_free(). */
static PangoFontDescription *make_font_desc(double size_pt, bool bold) {
    PangoFontDescription *fd = pango_font_description_new();
    if (!fd) return NULL;
    pango_font_description_set_family(fd, "Sans");
    pango_font_description_set_absolute_size(fd, size_pt * PANGO_SCALE);
    pango_font_description_set_weight(fd,
        bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    return fd;
}

/* Measure pango text. Returns width and height in pixels of the
   laid-out text using the given pixel-size and weight. */
static void measure_text(Viewer *viewer, const char *text,
                         double size_px, bool bold,
                         int *out_w, int *out_h) {
    *out_w = 0; *out_h = 0;
    if (!viewer->pango_ctx || !text || !*text) return;
    PangoFontDescription *fd = make_font_desc(size_px, bold);
    if (!fd) return;
    PangoLayout *layout = pango_layout_new(viewer->pango_ctx);
    pango_layout_set_font_description(layout, fd);
    pango_font_description_free(fd); /* pango copied internally */
    pango_layout_set_text(layout, text, -1);
    int w_pu = 0, h_pu = 0;
    pango_layout_get_pixel_size(layout, &w_pu, &h_pu);
    *out_w = w_pu;
    *out_h = h_pu;
    g_object_unref(layout);
}

/* Render `text` at screen position (x, y) in the given RGBA color.
   Uses pango+cairo to lay out the glyphs into an off-screen ARGB32
   surface, converts to straight RGBA, uploads as a one-shot SDL
   texture, and blits. Subpixel-accurate, antialiased, honors whatever
   font fontconfig serves for "Sans 11" (typically the user's
   configured Sans default). */
static bool draw_text(Viewer *viewer, float x, float y,
                      const char *text,
                      double size_px, bool bold,
                      Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
    if (!viewer->pango_ctx || !text || !*text) return true;

    PangoFontDescription *fd = make_font_desc(size_px, bold);
    if (!fd) return false;
    PangoLayout *layout = pango_layout_new(viewer->pango_ctx);
    pango_layout_set_font_description(layout, fd);
    pango_font_description_free(fd); /* pango copied internally */
    pango_layout_set_text(layout, text, -1);

    int w_pu = 0, h_pu = 0;
    pango_layout_get_pixel_size(layout, &w_pu, &h_pu);
    if (w_pu <= 0 || h_pu <= 0) {
        g_object_unref(layout);
        return true;
    }

    /* Pad to keep ascenders/descenders/antialiased fringes from being
       clipped. */
    int pad = 2;
    int W = w_pu + pad * 2;
    int H = h_pu + pad * 2;

    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, W);
    unsigned char *cbuf = calloc(1, (size_t)stride * (size_t)H);
    if (!cbuf) {
        g_object_unref(layout);
        return false;
    }
    cairo_surface_t *surf = cairo_image_surface_create_for_data(
        cbuf, CAIRO_FORMAT_ARGB32, W, H, stride);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        free(cbuf);
        g_object_unref(layout);
        return false;
    }
    cairo_t *cr = cairo_create(surf);
    /* Color: use straight RGB; cairo's source RGBA -> ARGB32 will
       premultiply for us. Alpha applied separately at blit time. */
    cairo_set_source_rgba(cr, r / 255.0, g / 255.0, b / 255.0, 1.0);
    cairo_move_to(cr, (double)pad, (double)pad);
    pango_cairo_show_layout(cr, layout);
    cairo_destroy(cr);
    cairo_surface_flush(surf);

    /* If cairo padded the stride beyond W*4 bytes, repack to tight
       rows. SDL UpdateTexture takes a pitch so we could pass `stride`
       directly — and we do, no repack needed. */

    /* Convert cairo's premultiplied BGRA -> straight RGBA in-place.
       Note: per-row stride is `stride` bytes, not W*4. The helper
       assumes a tight buffer; do row-by-row instead. */
    {
        for (int row = 0; row < H; row++) {
            unsigned char *p = cbuf + (size_t)row * stride;
            for (int col = 0; col < W; col++) {
                unsigned char *px = p + col * 4;
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
                unsigned char A = px[0], R = px[1], G = px[2], B = px[3];
#else
                unsigned char B = px[0], G = px[1], R = px[2], A = px[3];
#endif
                if (A != 0 && A != 255) {
                    R = (unsigned char)((R * 255 + A / 2) / A);
                    G = (unsigned char)((G * 255 + A / 2) / A);
                    B = (unsigned char)((B * 255 + A / 2) / A);
                }
                px[0] = R; px[1] = G; px[2] = B; px[3] = A;
            }
        }
    }

    SDL_Texture *tex = SDL_CreateTexture(viewer->renderer,
                                         SDL_PIXELFORMAT_RGBA32,
                                         SDL_TEXTUREACCESS_STATIC,
                                         W, H);
    if (!tex) {
        cairo_surface_destroy(surf);
        free(cbuf);
        g_object_unref(layout);
        return false;
    }
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureAlphaMod(tex, a);
    SDL_UpdateTexture(tex, NULL, cbuf, stride);

    SDL_FRect dst = { x - (float)pad, y - (float)pad,
                      (float)W, (float)H };
    SDL_RenderTexture(viewer->renderer, tex, NULL, &dst);
    SDL_DestroyTexture(tex);

    cairo_surface_destroy(surf);
    free(cbuf);
    g_object_unref(layout);
    return true;
}



/* The keybinds cheat-sheet. Each entry is a (key, description) pair. */
typedef struct BindEntry {
    const char *key;
    const char *desc;
} BindEntry;
static const BindEntry BINDS[] = {
    {"f",         "Toggle fullscreen"},
    {"r",         "Reset zoom, pan, flips, rotation"},
    {"i",         "Toggle info overlay"},
    {"b",         "Toggle this keybind list"},
    {"n",         "Toggle nearest-neighbor sampling (pixel art)"},
    {"a",         "Toggle aspect-ratio lock"},
    {"h / v",     "Flip horizontal / vertical"},
    {"k / l",     "Rotate 90 CCW / CW"},
    {"t",         "Toggle tile mode"},
    {"w",         "Toggle mirrored tiles (in tile mode)"},
    {"1-9",       "Tile radius (in tile mode)"},
    {"p",         "Pause / resume animation"},
    {"- / =",     "Previous / next animation frame"},
    {", / .",     "Previous / next image in playlist"},
    {"Home / End", "First / last image in playlist"},
    {"Wheel",     "Zoom in/out"},
    {"Drag",      "Pan"},
    {"q / Esc",   "Quit"},
};
static const int BINDS_COUNT = (int)(sizeof(BINDS) / sizeof(BINDS[0]));

/* ---------- SVG tile-pyramid management (main-thread) ---------- */

/* Returns true if the current view state differs from what was
   recorded last frame. Also refreshes the recorded values. */
static bool svg_view_changed(SvgImage *svg, int window_w, int window_h,
                             const Viewer *viewer) {
    if (!svg->last_view_valid
        || svg->last_window_w     != window_w
        || svg->last_window_h     != window_h
        || svg->last_zoom         != viewer->zoom
        || svg->last_anchor_x     != viewer->anchor_x
        || svg->last_anchor_y     != viewer->anchor_y
        || svg->last_pan_off_x    != viewer->pan_off_x
        || svg->last_pan_off_y    != viewer->pan_off_y
        || svg->last_rotation_steps != viewer->rotation_steps
        || svg->last_flip_h       != viewer->flip_h
        || svg->last_flip_v       != viewer->flip_v
        || svg->last_tile_on      != viewer->tile_on
        || svg->last_tile_radius  != viewer->tile_radius) {
        svg->last_view_valid       = true;
        svg->last_window_w         = window_w;
        svg->last_window_h         = window_h;
        svg->last_zoom             = viewer->zoom;
        svg->last_anchor_x         = viewer->anchor_x;
        svg->last_anchor_y         = viewer->anchor_y;
        svg->last_pan_off_x        = viewer->pan_off_x;
        svg->last_pan_off_y        = viewer->pan_off_y;
        svg->last_rotation_steps   = viewer->rotation_steps;
        svg->last_flip_h           = viewer->flip_h;
        svg->last_flip_v           = viewer->flip_v;
        svg->last_tile_on          = viewer->tile_on;
        svg->last_tile_radius      = viewer->tile_radius;
        return true;
    }
    return false;
}

/* Maintain the SVG sharp-tile pyramid for this frame:
   - Detect view-state changes and bump generation accordingly.
   - Decide whether a sharp tile is wanted (skipped in tile-mode, and
     when current zoom doesn't exceed base resolution).
   - Queue a worker request after the debounce window elapses.
   - Claim any finished worker buffer matching the current generation
     and upload it into svg->tile_tex.
   Called near the top of render(), before the texture draw block.
   Rotation and flip are handled by svg_draw_tile_overlay using the
   parent's transform pipeline; the worker always rasterizes in
   un-transformed image space. */
static void svg_update_pyramid(Viewer *viewer, int window_w, int window_h,
                               double draw_w, double draw_h) {
    if (viewer->src.kind != IMG_SVG) return;
    SvgImage *svg = &viewer->src.v.svg;

    if (svg_view_changed(svg, window_w, window_h, viewer)) {
        svg_bump_generation(svg);
    }

    /* Tile-mode (the (2R+1)x(2R+1) grid) draws each grid cell as a
       full image; producing an overlay tile per grid cell would be
       expensive and rarely useful. Stay on the base in tile mode. */
    bool tiled = (viewer->tile_on && viewer->tile_radius > 0);
    if (tiled) {
        svg->tile_valid = false;
        return;
    }

    if (viewer->src.width < 1 || viewer->src.height < 1
        || draw_w < 1.0 || draw_h < 1.0) return;

    /* For rotated/flipped views we conservatively raster the whole
       image rather than computing a rotated-visibility intersection
       (which is fiddly and the SVG_TILE_MAX_DIM cap keeps it bounded
       anyway). draw_w/draw_h still correctly describe the on-screen
       footprint, so PPU is right; only the visible-rect calc below is
       affected. */
    bool transformed = (viewer->rotation_steps != 0
                        || viewer->flip_h || viewer->flip_v);

    /* PPU = on-screen pixels per SVG user unit. The base raster's PPU
       (relative to the natural SVG) is base_w / natural_w. If the
       on-screen image's PPU exceeds the base's by more than ~5 %, we
       want a sharper tile. Below that, GPU bilinear on the base is
       fine and a tile would just waste cycles. */
    double ppu_x = draw_w / svg->natural_w_f;
    double ppu_y = draw_h / svg->natural_h_f;
    double ppu   = (ppu_x < ppu_y) ? ppu_x : ppu_y;
    double base_ppu = (double)svg->width / svg->natural_w_f;
    if (ppu <= base_ppu * 1.05) {
        /* Base is sharp enough — no tile needed. */
        svg->tile_valid = false;
        /* Mark as if we'd already requested for current gen, so the
           gate below won't fire for this view. The generation will
           bump on the next view change anyway. */
        svg->has_last_requested = true;
        svg->last_requested_generation = svg->current_generation;
        return;
    }

    /* Compute the visible image rect on screen, then convert to SVG
       user-unit coordinates. For rotated/flipped views we just raster
       the whole image (see comment above). */
    double ix0, iy0, ix1, iy1;
    if (transformed) {
        ix0 = 0.0; iy0 = 0.0;
        ix1 = svg->natural_w_f;
        iy1 = svg->natural_h_f;
    } else {
        /* On-screen position of the full image rect (matches render's math). */
        double w2p_sx, w2p_sy;
        window_to_pixel_scale(viewer, &w2p_sx, &w2p_sy);
        double pan_px_x = viewer->pan_off_x * w2p_sx;
        double pan_px_y = viewer->pan_off_y * w2p_sy;
        double img_x = (double)window_w * 0.5 - viewer->anchor_x * draw_w + pan_px_x;
        double img_y = (double)window_h * 0.5 - viewer->anchor_y * draw_h + pan_px_y;

        double vis_x0 = img_x > 0.0 ? 0.0 : -img_x;
        double vis_y0 = img_y > 0.0 ? 0.0 : -img_y;
        double vis_x1 = ((double)window_w - img_x);
        double vis_y1 = ((double)window_h - img_y);
        if (vis_x1 > draw_w) vis_x1 = draw_w;
        if (vis_y1 > draw_h) vis_y1 = draw_h;
        if (vis_x1 <= vis_x0 || vis_y1 <= vis_y0) {
            /* Image is fully off-screen — don't bother. Same trick as
               the "base is sharp enough" branch: pretend we've already
               requested for this gen so we don't spin trying. */
            svg->has_last_requested = true;
            svg->last_requested_generation = svg->current_generation;
            return;
        }

        /* Map screen-px to SVG user units. */
        double img_to_svg_x = svg->natural_w_f / draw_w;
        double img_to_svg_y = svg->natural_h_f / draw_h;
        ix0 = vis_x0 * img_to_svg_x;
        iy0 = vis_y0 * img_to_svg_y;
        ix1 = vis_x1 * img_to_svg_x;
        iy1 = vis_y1 * img_to_svg_y;
    }

    /* Pad by SVG_TILE_PAD_RATIO so small pans don't immediately invalidate
       the tile. Clamp to the SVG's natural bounds. */
    double iw = ix1 - ix0;
    double ih = iy1 - iy0;
    double pad_x = iw * SVG_TILE_PAD_RATIO;
    double pad_y = ih * SVG_TILE_PAD_RATIO;
    ix0 -= pad_x; iy0 -= pad_y;
    ix1 += pad_x; iy1 += pad_y;
    if (ix0 < 0.0) ix0 = 0.0;
    if (iy0 < 0.0) iy0 = 0.0;
    if (ix1 > svg->natural_w_f) ix1 = svg->natural_w_f;
    if (iy1 > svg->natural_h_f) iy1 = svg->natural_h_f;
    iw = ix1 - ix0;
    ih = iy1 - iy0;
    if (iw <= 0.0 || ih <= 0.0) return;

    /* Output size: covers the (padded) visible region at current PPU,
       capped at SVG_TILE_MAX_DIM AND at a render scale where the
       document-times-scale fits within cairo's max image surface size
       (~32767 px on the image backend). librsvg internally allocates
       group/filter surfaces sized to viewport*scale, and exceeding the
       cap silently produces a CAIRO_STATUS_INVALID_SIZE.

       The cap depends on the natural SVG dimensions: for a long
       document the safe scale is small. Past the cap, the GPU
       bilinearly upscales the tile, which is imperceptibly different
       from a true vector raster at high zoom. */
    /* Two caps: (1) doc_long * scale must fit cairo's max image
       surface size (~32k px); (2) the scale itself must stay within
       librsvg's numerical sanity. Empirically rsvg returns
       InvalidSize past scale ~16 even on small documents (the
       filter/group machinery scales internal buffers by the cairo
       matrix). 16× the base is well past the point of useful sharper
       detail anyway — GPU bilinear handles the rest. */
    double doc_long = svg->natural_w_f > svg->natural_h_f
                    ? svg->natural_w_f : svg->natural_h_f;
    double safe_scale_cap = doc_long > 0.0 ? (28000.0 / doc_long) : 16.0;
    if (safe_scale_cap > 16.0) safe_scale_cap = 16.0;
    if (safe_scale_cap < 1.0)  safe_scale_cap = 1.0;
    double render_scale = ppu; /* SVG-units to output-pixels */
    if (render_scale > safe_scale_cap) {
        render_scale = safe_scale_cap;
    }
    double want_out_w = iw * render_scale;
    double want_out_h = ih * render_scale;
    double cap_scale = 1.0;
    if (want_out_w > (double)SVG_TILE_MAX_DIM) {
        cap_scale = (double)SVG_TILE_MAX_DIM / want_out_w;
    }
    if (want_out_h > (double)SVG_TILE_MAX_DIM) {
        double s = (double)SVG_TILE_MAX_DIM / want_out_h;
        if (s < cap_scale) cap_scale = s;
    }
    int out_w = (int)floor(want_out_w * cap_scale + 0.5);
    int out_h = (int)floor(want_out_h * cap_scale + 0.5);
    if (out_w < 1) out_w = 1;
    if (out_h < 1) out_h = 1;
    if (out_w > SVG_TILE_MAX_DIM) out_w = SVG_TILE_MAX_DIM;
    if (out_h > SVG_TILE_MAX_DIM) out_h = SVG_TILE_MAX_DIM;

    /* Decide whether to queue a worker request. The single rule is:

         request iff
             - the tile we have isn't valid for the CURRENT generation
               (so the user can't already see a sharp tile), AND
             - we haven't already queued a request for the CURRENT
               generation (so we don't spam the worker), AND
             - the debounce window has elapsed since the last view
               change (so fast-scrolling doesn't queue dozens of
               jobs).

       Tracking only `last_requested_generation` (instead of a
       separate dirty flag) means there's exactly one source of
       truth for "is a sharp tile in the pipeline for this view".
       If a stale tile is discarded by the consume block below, the
       generation comparison ensures we'll re-request automatically
       — no manual dirty-flag management required. */
    Uint64 now = SDL_GetTicksNS();
    bool tile_already_for_current_gen =
        svg->tile_valid
        && svg->tile_generation == svg->current_generation;
    bool request_already_for_current_gen =
        svg->has_last_requested
        && svg->last_requested_generation == svg->current_generation;
    if (!tile_already_for_current_gen
        && !request_already_for_current_gen
        && now - svg->dirty_since_ns >= SVG_DEBOUNCE_NS) {
        svg_request_tile(svg, svg->current_generation,
                         ix0, iy0, iw, ih, out_w, out_h);
    }

    /* Try to consume any finished tile. We hold svg->mu for the entire
       claim+upload sequence so the worker can't start writing into
       delivered_rgba (via the post-render swap) while we're still
       reading from it. The lock is released BEFORE creating SDL
       textures (which are renderer-thread state, not tile state) and
       reacquired only briefly to drop job_done. */
    SDL_LockMutex(svg->mu);
    bool have_done = svg->job_done;
    bool gen_ok    = have_done && svg->done_generation == svg->current_generation;
    bool nonempty  = have_done && svg->done_out_w > 0 && svg->done_out_h > 0;
    int    cw  = svg->done_out_w;
    int    ch  = svg->done_out_h;
    double cix = svg->done_ix;
    double ciy = svg->done_iy;
    double ciw = svg->done_iw;
    double cih = svg->done_ih;
    if (!have_done) {
        SDL_UnlockMutex(svg->mu);
    } else if (!(gen_ok && nonempty)) {
        /* Stale or failed render — drain the slot and move on. The
           buffer (if any) stays in delivered_rgba; the worker will
           overwrite it on the next render via the swap. */
        svg->job_done = false;
        SDL_UnlockMutex(svg->mu);
    } else {
        /* Fresh tile for the current view. The lock stays held until
           after SDL_UpdateTexture finishes — so the worker, even if
           it's just finished another job, can't overwrite the buffer
           we're reading. */
        unsigned char *cbuf = svg->delivered_rgba;

        bool create = (!svg->tile_tex
                       || svg->tile_tex_w != cw
                       || svg->tile_tex_h != ch);
        if (create) {
            if (svg->tile_tex) SDL_DestroyTexture(svg->tile_tex);
            /* Cairo's CAIRO_FORMAT_ARGB32 stores bytes B,G,R,A in
               memory on little-endian (the "32" refers to a 32-bit
               value 0xAARRGGBB, which is BGRA in memory on LE).
               SDL_PIXELFORMAT_ARGB8888 has matching memory layout
               (the "8888" naming describes the packed value in MSB
               order, which on LE means BGRA in memory). With this
               format SDL can sample cairo's buffer with no byte
               swapping, and SDL_BLENDMODE_BLEND_PREMULTIPLIED handles
               the premultiplied alpha correctly. Saves a full pass
               over the tile pixels every time a new tile lands. */
            svg->tile_tex = SDL_CreateTexture(viewer->renderer,
                                              SDL_PIXELFORMAT_ARGB8888,
                                              SDL_TEXTUREACCESS_STATIC,
                                              cw, ch);
            if (!svg->tile_tex) {
                /* GPU rejected the size (over max texture, OOM, etc.).
                   Stay on the base. We don't clear has_last_requested
                   here — re-requesting the same plan would just fail
                   the same way. The next view change will bump the
                   generation, which triggers a new plan with possibly
                   a smaller size. */
                svg->tile_valid = false;
                svg->tile_tex_w = 0;
                svg->tile_tex_h = 0;
                svg->job_done = false;
                SDL_UnlockMutex(svg->mu);
                return;
            }
            SDL_SetTextureScaleMode(svg->tile_tex,
                                    viewer->nearest ? SDL_SCALEMODE_NEAREST
                                                    : SDL_SCALEMODE_LINEAR);
            SDL_SetTextureBlendMode(svg->tile_tex,
                                    SDL_BLENDMODE_BLEND_PREMULTIPLIED);
            svg->tile_tex_w = cw;
            svg->tile_tex_h = ch;
        }
        bool ok = SDL_UpdateTexture(svg->tile_tex, NULL, cbuf, cw * 4);
        svg->job_done = false;
        SDL_UnlockMutex(svg->mu);

        if (!ok) {
            /* Upload failed — invalidate so we don't draw stale, and
               clear last_requested for the current gen so the request
               gate will fire again on the next render. */
            svg->tile_valid = false;
            svg->has_last_requested = false;
            return;
        }
        svg->tile_ix = cix; svg->tile_iy = ciy;
        svg->tile_iw = ciw; svg->tile_ih = cih;
        svg->tile_generation = svg->current_generation;
        svg->tile_valid = true;
    }
}

/* Draw the sharp tile overlay on top of the base, sharing the parent's
   transform pipeline (flip → rotate about parent center). The tile
   texture itself was rasterized in un-rotated, un-flipped image space;
   we map it through the same transform as the parent so the overlay
   lands exactly where the corresponding base pixels are.

   parent_pre_dst is the parent's *pre-rotation* dst rect (its width
   and height are swapped relative to the on-screen footprint when
   `quarter` rotation is in effect — same convention as the base draw).
   parent_cx/cy is the parent's screen-space center (post-flip,
   post-rotation centroid; rotation is about this point).

   No-op if the tile is invalid or doesn't match the current view's
   generation. */
static void svg_draw_tile_overlay(const Viewer *viewer,
                                  double parent_cx, double parent_cy,
                                  double parent_pre_w, double parent_pre_h,
                                  double angle_deg,
                                  SDL_FlipMode flip,
                                  bool quarter) {
    if (viewer->src.kind != IMG_SVG) return;
    const SvgImage *svg = &viewer->src.v.svg;
    if (!svg->tile_valid || !svg->tile_tex) return;
    if (svg->tile_generation != svg->current_generation) return;
    if (svg->natural_w_f <= 0.0 || svg->natural_h_f <= 0.0) return;

    /* The "image-frame" dst is the rect that, when SDL rotates it about
       its center, becomes the on-screen footprint of the un-rotated
       image. For quarter rotations the parent code swaps w/h so that
       post-rotation orientation is correct — we work in that same
       frame.

       Image dimensions in the frame (= natural image aspect, scaled
       to parent_pre size). When quarter, the texture's natural width
       maps to parent_pre_h on screen (because rotation will swap them
       back). To keep the tile's texture-space-to-image-space mapping
       linear, we always reason in "image space" where x runs along
       natural_w and y runs along natural_h — the parent_pre rect
       already represents that frame. */
    double frame_w = parent_pre_w;
    double frame_h = parent_pre_h;
    if (quarter) {
        /* In the parent_pre rect for quarter rotations, w corresponds
           to the image's HEIGHT and h to its WIDTH (because rotation
           swaps them). Flip so frame_w/frame_h match natural axes. */
        frame_w = parent_pre_h;
        frame_h = parent_pre_w;
    }

    /* Tile rect in image space, normalized to [0..1]. */
    double rel_x0 = svg->tile_ix / svg->natural_w_f;
    double rel_y0 = svg->tile_iy / svg->natural_h_f;
    double rel_w  = svg->tile_iw / svg->natural_w_f;
    double rel_h  = svg->tile_ih / svg->natural_h_f;

    /* Tile rect in the un-rotated, un-flipped frame (axis-aligned with
       the natural image, centered at parent center). */
    double tile_off_x = (rel_x0 + rel_w * 0.5 - 0.5) * frame_w;
    double tile_off_y = (rel_y0 + rel_h * 0.5 - 0.5) * frame_h;
    double tile_w_img = rel_w * frame_w;
    double tile_h_img = rel_h * frame_h;

    /* Apply flip in image space about the parent center. SDL will also
       flip the tile texture itself when we pass `flip`, which mirrors
       the *content* — we just need to mirror the *position*. */
    if (flip & SDL_FLIP_HORIZONTAL) tile_off_x = -tile_off_x;
    if (flip & SDL_FLIP_VERTICAL)   tile_off_y = -tile_off_y;

    /* Build the pre-rotation dst rect for the tile. In the parent's
       pre-rotation frame, the rect lives at (parent_cx + tile_off_x,
       parent_cy + tile_off_y) with size matching its image rect.

       For quarter rotations, the parent dst's w corresponds to
       image-height on screen, so we similarly swap the tile dst's
       w/h here. */
    double tile_pre_w = quarter ? tile_h_img : tile_w_img;
    double tile_pre_h = quarter ? tile_w_img : tile_h_img;

    /* For quarter rotations, the offset-along-image-axes also needs
       swapping to match the pre-rotation frame. (Pre-rotation x-axis
       runs along image-y, etc.) */
    double pre_off_x = quarter ? tile_off_y : tile_off_x;
    double pre_off_y = quarter ? tile_off_x : tile_off_y;

    SDL_FRect dst;
    dst.w = (float)tile_pre_w;
    dst.h = (float)tile_pre_h;
    dst.x = (float)(parent_cx + pre_off_x - tile_pre_w * 0.5);
    dst.y = (float)(parent_cy + pre_off_y - tile_pre_h * 0.5);
    if (dst.w < 0.5f || dst.h < 0.5f) return;

    /* Pivot rotation about the parent's center, not the tile's. */
    SDL_FPoint pivot = {
        (float)(parent_cx - dst.x),
        (float)(parent_cy - dst.y)
    };

    if (angle_deg != 0.0 || flip != SDL_FLIP_NONE) {
        SDL_RenderTextureRotated(viewer->renderer, svg->tile_tex,
                                 NULL, &dst, angle_deg, &pivot, flip);
    } else {
        SDL_RenderTexture(viewer->renderer, svg->tile_tex, NULL, &dst);
    }
}

static void render(Viewer *viewer) {
    int window_w = 1, window_h = 1;
    SDL_GetCurrentRenderOutputSize(viewer->renderer, &window_w, &window_h);

    Uint8 br = viewer->opts ? viewer->opts->bg_r : 0;
    Uint8 bg = viewer->opts ? viewer->opts->bg_g : 0;
    Uint8 bb = viewer->opts ? viewer->opts->bg_b : 0;
    SDL_SetRenderDrawColor(viewer->renderer, br, bg, bb, 255);
    SDL_RenderClear(viewer->renderer);

    /* Classic full-image bilinear scale, with optional flip / rotation
       / tile. Rotation by 90 or 270 degrees swaps the on-screen aspect;
       we honor that so the rotated image fits the same way the natural
       image would. All image kinds (static raster, animated, pre-
       rasterized SVG) go through this path — they share the same
       viewer->texture. */
    if (viewer->texture) {
        int eff_w = viewer->src.width;
        int eff_h = viewer->src.height;
        bool quarter = (viewer->rotation_steps == 1 || viewer->rotation_steps == 3);
        if (quarter) { int tmp = eff_w; eff_w = eff_h; eff_h = tmp; }

        /* Compute fit/draw size against the rotated aspect. */
        double img_aspect = (double)eff_w / (double)eff_h;
        double win_aspect = (double)window_w / (double)window_h;
        double fit_w, fit_h;
        if (win_aspect > img_aspect) {
            fit_h = (double)window_h;
            fit_w = (double)window_h * img_aspect;
        } else {
            fit_w = (double)window_w;
            fit_h = (double)window_w / img_aspect;
        }
        double zoom = viewer->zoom;
        if (zoom <= 0.0 || !isfinite(zoom)) zoom = 1.0;
        double draw_w = fit_w * zoom;
        double draw_h = fit_h * zoom;
        if (draw_w < 1.0) draw_w = 1.0;
        if (draw_h < 1.0) draw_h = 1.0;

        /* SVG: maintain the sharp-tile pyramid. May queue a worker
           request and/or upload a freshly-rendered tile. The base
           texture (viewer->texture) is drawn regardless; the tile
           is overlaid afterwards when valid. */
        svg_update_pyramid(viewer, window_w, window_h, draw_w, draw_h);

        double center_x = (double)window_w * 0.5;
        double center_y = (double)window_h * 0.5;

        /* Drag pans in window-unit screen coordinates. Convert to
           renderer-pixel units and apply as a final translation. */
        double w2p_sx, w2p_sy;
        window_to_pixel_scale(viewer, &w2p_sx, &w2p_sy);
        double pan_px_x = viewer->pan_off_x * w2p_sx;
        double pan_px_y = viewer->pan_off_y * w2p_sy;

        SDL_FlipMode base_flip =
            (viewer->flip_h && viewer->flip_v) ? SDL_FLIP_HORIZONTAL_AND_VERTICAL :
            viewer->flip_h ? SDL_FLIP_HORIZONTAL :
            viewer->flip_v ? SDL_FLIP_VERTICAL :
            SDL_FLIP_NONE;
        double angle = (double)viewer->rotation_steps * 90.0;

        /* Tile mode produces a (2R+1) x (2R+1) grid of tiles around
           the center, where R = tile_radius. Each tile is the same
           size as a single draw_w x draw_h, contiguous. With mirror
           mode on, alternating tiles are flipped to make seams match. */
        int radius = (viewer->tile_on && viewer->tile_radius > 0) ? viewer->tile_radius : 0;

        for (int gy = -radius; gy <= radius; gy++) {
            for (int gx = -radius; gx <= radius; gx++) {
                /* Compute per-tile flip when in mirror mode. */
                SDL_FlipMode flip = base_flip;
                if (viewer->tile_on && viewer->tile_mirror) {
                    bool fh = (gx & 1) != 0;
                    bool fv = (gy & 1) != 0;
                    /* XOR the base flips with the per-tile flips. */
                    bool h = ((flip & SDL_FLIP_HORIZONTAL) != 0) ^ fh;
                    bool v = ((flip & SDL_FLIP_VERTICAL)   != 0) ^ fv;
                    flip = (h && v) ? SDL_FLIP_HORIZONTAL_AND_VERTICAL :
                           h ? SDL_FLIP_HORIZONTAL :
                           v ? SDL_FLIP_VERTICAL :
                           SDL_FLIP_NONE;
                }
                /* Position: anchor places (anchor_x, anchor_y) of the
                   CENTER tile at the window center, then drag pan
                   offset translates the entire composition in
                   screen space. Other tiles are offset by integer
                   multiples of draw_w / draw_h (the post-rotation
                   footprint). */
                double tile_cx = center_x - viewer->anchor_x * draw_w + draw_w * 0.5
                               + (double)gx * draw_w + pan_px_x;
                double tile_cy = center_y - viewer->anchor_y * draw_h + draw_h * 0.5
                               + (double)gy * draw_h + pan_px_y;

                /* SDL_RenderTextureRotated stretches the texture to
                   fill dst, THEN rotates around the rect center.
                   For a 90/270 rotation, the on-screen footprint
                   gets swapped by the rotation — so we must build
                   the pre-rotation dst with swapped dimensions so
                   that the texture's natural aspect is preserved
                   and the post-rotation footprint matches draw_w x
                   draw_h. */
                SDL_FRect dst;
                if (quarter) {
                    dst.w = (float)draw_h;
                    dst.h = (float)draw_w;
                } else {
                    dst.w = (float)draw_w;
                    dst.h = (float)draw_h;
                }
                /* Center the rect on (tile_cx, tile_cy) regardless of
                   which dimensions we chose; rotation is around the
                   rect center, so the centroid stays put. */
                dst.x = (float)(tile_cx - (double)dst.w * 0.5);
                dst.y = (float)(tile_cy - (double)dst.h * 0.5);

                if (angle != 0.0 || flip != SDL_FLIP_NONE) {
                    SDL_RenderTextureRotated(viewer->renderer,
                                             viewer->texture,
                                             NULL, &dst,
                                             angle, NULL, flip);
                } else {
                    SDL_RenderTexture(viewer->renderer,
                                      viewer->texture, NULL, &dst);
                }
                /* For non-tile-mode SVG, overlay the sharp tile (if any)
                   sharing the parent's transform pipeline. The overlay
                   no-ops in tile mode and when the tile is invalid /
                   stale, so the call is cheap and always safe. */
                if (radius == 0) {
                    svg_draw_tile_overlay(viewer,
                                          tile_cx, tile_cy,
                                          (double)dst.w, (double)dst.h,
                                          angle, flip, quarter);
                }
            }
        }
    }


    /* Compute a window-adaptive text size that grows/shrinks with the
       smaller window dimension. Soft clamps keep it readable on tiny
       windows and not absurd on huge ones. */
    int min_dim = window_w < window_h ? window_w : window_h;
    if (min_dim < 1) min_dim = 1;
    double info_size_px = (double)min_dim * 0.018;
    if (info_size_px < 11.0) info_size_px = 11.0;
    if (info_size_px > 32.0) info_size_px = 32.0;
    double binds_size_px = (double)min_dim * 0.024;
    if (binds_size_px < 13.0) binds_size_px = 13.0;
    if (binds_size_px > 40.0) binds_size_px = 40.0;

    /* Info overlay (drawn last so it sits on top of the image). Bottom-
       left. Text is laid out by pango using the user's configured Sans
       font; the dark backdrop hugs the widest line. */
    if (viewer->info_on && viewer->info_line_count > 0) {
        const float pad      = 10.0f;
        const float line_gap = 2.0f;

        /* Measure each line so we know how wide / tall the box is. */
        int max_w = 0;
        int line_h = 0;
        for (int i = 0; i < viewer->info_line_count; i++) {
            int lw = 0, lh = 0;
            measure_text(viewer, viewer->info_lines[i], info_size_px, false,
                         &lw, &lh);
            if (lw > max_w) max_w = lw;
            if (lh > line_h) line_h = lh;
        }
        if (line_h <= 0) line_h = 16;

        float total_h = (float)viewer->info_line_count * (float)line_h
                      + (float)(viewer->info_line_count - 1) * line_gap;
        float box_w = (float)max_w + 2.0f * pad;
        float box_h = total_h + 2.0f * pad;

        SDL_FRect bg = {
            6.0f,
            (float)window_h - 6.0f - box_h,
            box_w,
            box_h
        };
        SDL_SetRenderDrawBlendMode(viewer->renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(viewer->renderer, 0, 0, 0, 180);
        SDL_RenderFillRect(viewer->renderer, &bg);

        for (int i = 0; i < viewer->info_line_count; i++) {
            float y = bg.y + pad + (float)i * ((float)line_h + line_gap);
            draw_text(viewer, bg.x + pad, y, viewer->info_lines[i],
                      info_size_px, false,
                      255, 255, 255, 255);
        }
    }

    /* Keybind cheat-sheet (`b`). Top-left anchored. Two columns:
       key (right-aligned in its column), description (left-aligned). */
    if (viewer->binds_on) {
        const float pad      = 14.0f;
        const float line_gap = 4.0f;
        const float gutter   = 16.0f;

        /* Measure every key (bold) and description (regular) so we can
           size the box and right-align the keys. */
        int line_h = 0;
        int key_w[64] = {0};
        int max_key_w = 0, max_desc_w = 0;
        for (int i = 0; i < BINDS_COUNT && i < 64; i++) {
            int kw = 0, kh = 0;
            int dw = 0, dh = 0;
            measure_text(viewer, BINDS[i].key,  binds_size_px, true,  &kw, &kh);
            measure_text(viewer, BINDS[i].desc, binds_size_px, false, &dw, &dh);
            key_w[i] = kw;
            if (kw > max_key_w)  max_key_w  = kw;
            if (dw > max_desc_w) max_desc_w = dw;
            int lh = kh > dh ? kh : dh;
            if (lh > line_h) line_h = lh;
        }
        if (line_h <= 0) line_h = 16;

        float content_w = (float)max_key_w + gutter + (float)max_desc_w;
        float content_h = (float)BINDS_COUNT * (float)line_h
                        + (float)(BINDS_COUNT - 1) * line_gap;
        float box_w = content_w + 2.0f * pad;
        float box_h = content_h + 2.0f * pad;
        float box_x = 16.0f;
        float box_y = 16.0f;

        SDL_FRect bg = { box_x, box_y, box_w, box_h };
        SDL_SetRenderDrawBlendMode(viewer->renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(viewer->renderer, 0, 0, 0, 210);
        SDL_RenderFillRect(viewer->renderer, &bg);
        SDL_SetRenderDrawColor(viewer->renderer, 255, 255, 255, 220);
        SDL_RenderRect(viewer->renderer, &bg);

        for (int i = 0; i < BINDS_COUNT; i++) {
            float row_y = box_y + pad + (float)i * ((float)line_h + line_gap);
            float key_x = box_x + pad + (float)(max_key_w - key_w[i]);
            float desc_x = box_x + pad + (float)max_key_w + gutter;
            draw_text(viewer, key_x, row_y, BINDS[i].key,
                      binds_size_px, true,
                      255, 220, 120, 255); /* warm key (bold) */
            draw_text(viewer, desc_x, row_y, BINDS[i].desc,
                      binds_size_px, false,
                      235, 235, 235, 255);
        }
    }

    SDL_RenderPresent(viewer->renderer);
}

/* ---------- input handlers ---------- */

static void toggle_fullscreen(Viewer *viewer) {
    viewer->fullscreen = !viewer->fullscreen;
    if (!SDL_SetWindowFullscreen(viewer->window, viewer->fullscreen)) {
        die_sdl("SDL_SetWindowFullscreen");
        viewer->fullscreen = !viewer->fullscreen;
    }
}

static void toggle_nearest(Viewer *viewer) {
    viewer->nearest = !viewer->nearest;
    /* Apply to the main texture (used by static + animated images).
       SVG textures are intentionally left in linear mode: SVGs are
       re-rasterized at the target pixel size so nearest sampling there
       would only expose the raster grid. */
    if (viewer->texture) {
        SDL_SetTextureScaleMode(viewer->texture,
                                viewer->nearest ? SDL_SCALEMODE_NEAREST
                                                : SDL_SCALEMODE_LINEAR);
    }
}

static void reset_view(Viewer *viewer) {
    /* Per spec: r clears zoom, anchor, flip, and rotate; preserves tile
       mode (but recenters the composition); preserves pause/skim state
       (animation continues from current frame; do not jump back to 0).

       Does NOT resize or reposition the window — the user's chosen
       window size is sticky. The image is just refit and recentered
       into whatever space the window currently has. */
    viewer->zoom = 1.0;
    viewer->anchor_x = 0.5;
    viewer->pan_off_x = 0.0;
    viewer->anchor_y = 0.5;
    viewer->pan_off_y = 0.0;
    viewer->flip_h = false;
    viewer->flip_v = false;
    viewer->rotation_steps = 0;
    /* Rotation went back to 0; restore the unrotated aspect lock so
       subsequent user resizes snap to the unrotated aspect. */
    apply_aspect_lock(viewer);
    rebuild_info_lines(viewer);
}

/* Push the aspect-ratio constraint to the window based on the current
   image dimensions AND the current rotation. Quarter-turn rotations
   (90 / 270) swap the displayed aspect, so the locked aspect needs to
   swap with them; otherwise the window snaps back to the unrotated
   aspect and the rotated image is letterboxed inside a wrong-shaped
   frame. Pass 0/0 to clear the constraint when aspect-lock is off or
   when no image is loaded. */
static void apply_aspect_lock(Viewer *viewer) {
    if (!viewer || !viewer->window) return;
    if (viewer->aspect_lock
        && viewer->src.width > 0 && viewer->src.height > 0) {
        bool quarter = (viewer->rotation_steps == 1
                        || viewer->rotation_steps == 3);
        float w = (float)(quarter ? viewer->src.height : viewer->src.width);
        float h = (float)(quarter ? viewer->src.width  : viewer->src.height);
        float aspect = w / h;
        SDL_SetWindowAspectRatio(viewer->window, aspect, aspect);
    } else {
        SDL_SetWindowAspectRatio(viewer->window, 0.0f, 0.0f);
    }
}

static void toggle_aspect_lock(Viewer *viewer) {
    viewer->aspect_lock = !viewer->aspect_lock;
    apply_aspect_lock(viewer);
}

/* Ensure the lazy frame cache is built. Called the first time we pause
   or skim an animated file. Safe no-op for non-animated images.
   After decoding, scans the cache to find the frame whose pixels best
   match the currently-displayed live frame, and sets current_frame to
   that index — so pausing snaps to "this frame" rather than frame 0. */
static bool ensure_frame_cache(Viewer *viewer) {
    if (viewer->src.kind != IMG_ANIM) return false;
    if (viewer->frames_decoded) return true;
    int w, h, n;
    unsigned char **frames = NULL;
    int *delays = NULL;
    if (!decode_all_frames(viewer->src.v.anim.anim,
                           &w, &h, &frames, &delays, &n)) {
        return false;
    }
    viewer->frame_w = w;
    viewer->frame_h = h;
    viewer->frames = frames;
    viewer->frame_delays_ms = delays;
    viewer->frame_count = n;
    viewer->frames_decoded = true;

    /* Find the cached frame that best matches the currently-displayed
       live frame, so a pause "freezes here" rather than snapping to 0.
       Compare a sparse sample of bytes; that's enough to disambiguate
       distinct frames cheaply. */
    viewer->current_frame = 0;
    AnimImage *a = &viewer->src.v.anim;
    if (a->rgba && a->width == w && a->height == h && n > 0) {
        size_t total = (size_t)w * h * 4u;
        size_t step = total / 256u;
        if (step < 1) step = 1;
        long best_diff = -1;
        int best_idx = 0;
        for (int i = 0; i < n; i++) {
            long diff = 0;
            for (size_t off = 0; off < total; off += step) {
                int d = (int)a->rgba[off] - (int)frames[i][off];
                diff += d < 0 ? -d : d;
            }
            if (best_diff < 0 || diff < best_diff) {
                best_diff = diff;
                best_idx = i;
                if (diff == 0) break;
            }
        }
        viewer->current_frame = best_idx;
    }
    return true;
}

/* Skim to a relative frame (delta = +1 next, -1 prev). Builds the frame
   cache on first use, pauses playback, uploads the new frame. */
static void skim_frame(Viewer *viewer, int delta) {
    if (viewer->src.kind != IMG_ANIM) return;
    if (!ensure_frame_cache(viewer)) return;
    viewer->paused = true;
    int n = viewer->frame_count;
    if (n <= 0) return;
    int next = ((viewer->current_frame + delta) % n + n) % n;
    viewer->current_frame = next;
    /* Upload the new frame to the existing texture. */
    if (ensure_texture(viewer, viewer->frame_w, viewer->frame_h)) {
        SDL_UpdateTexture(viewer->texture, NULL,
                          viewer->frames[next], viewer->frame_w * 4);
    }
    rebuild_info_lines(viewer);
}

/* Toggle pause/resume of animation. On first pause we build the frame
   cache so a subsequent skim is instant. */
static void toggle_pause(Viewer *viewer) {
    if (viewer->src.kind != IMG_ANIM) return;
    viewer->paused = !viewer->paused;
    if (viewer->paused) {
        ensure_frame_cache(viewer);
        /* If frame cache is now built, switch the displayed texture to
           match whatever the cached current_frame is. We try to stay on
           the frame the live iterator was on, but this is approximate. */
        if (viewer->frames_decoded) {
            int idx = viewer->current_frame;
            if (idx < 0 || idx >= viewer->frame_count) idx = 0;
            if (ensure_texture(viewer, viewer->frame_w, viewer->frame_h)) {
                SDL_UpdateTexture(viewer->texture, NULL,
                                  viewer->frames[idx], viewer->frame_w * 4);
            }
        }
    }
    rebuild_info_lines(viewer);
}

static void toggle_info(Viewer *viewer) {
    viewer->info_on = !viewer->info_on;
    if (viewer->info_on) rebuild_info_lines(viewer);
}

static void toggle_tile(Viewer *viewer) {
    viewer->tile_on = !viewer->tile_on;
    if (viewer->tile_on && viewer->tile_radius < 1) viewer->tile_radius = 1;
}

static void cycle_rotation(Viewer *viewer, int delta) {
    int old_steps = viewer->rotation_steps;
    viewer->rotation_steps = ((viewer->rotation_steps + delta) % 4 + 4) % 4;

    /* A quarter-turn (odd parity change between old and new) swaps the
       displayed aspect. Two things have to happen, in this order:
         1) Swap the window's current width and height. Otherwise the
            window keeps its old shape and SDL_SetWindowAspectRatio will
            shrink one axis to fit the new aspect inside the old frame,
            making the rotated image visibly smaller.
         2) Push the new aspect-lock so subsequent user resizes snap to
            the rotated aspect.
       Half-turns (180°) keep the aspect the same — nothing to do. */
    bool old_quarter = (old_steps == 1 || old_steps == 3);
    bool new_quarter = (viewer->rotation_steps == 1
                        || viewer->rotation_steps == 3);
    if (old_quarter != new_quarter
        && viewer->window && !viewer->fullscreen) {
        int win_w = 0, win_h = 0;
        SDL_GetWindowSize(viewer->window, &win_w, &win_h);
        if (win_w > 0 && win_h > 0) {
            /* Clear the aspect-ratio constraint first; SDL/Wayland will
               otherwise clamp the SetWindowSize call to the still-old
               aspect, leaving us with a wrong-shaped window. */
            SDL_SetWindowAspectRatio(viewer->window, 0.0f, 0.0f);
            SDL_SetWindowSize(viewer->window, win_h, win_w);
        }
    }
    apply_aspect_lock(viewer);
}

/* Pan by a mouse delta. dx/dy are in window-unit screen coordinates
   (the same units SDL reports in motion.xrel/yrel). We just accumulate
   a screen-space pan offset and let the renderer apply it as a final
   translation after all rotation/flip math. This makes dragging always
   move the visible content 1:1 with the cursor, regardless of rotation. */
static void pan_by(Viewer *viewer, double dx, double dy) {
    viewer->pan_off_x += dx;
    viewer->pan_off_y += dy;
}

/* ---------- event loop ---------- */

static Sint32 time_until_next_anim_frame_ms(const Viewer *viewer) {
    if (viewer->src.kind != IMG_ANIM) return -1;
    if (viewer->paused) return -1; /* no animation timer needed */
    Uint64 now = SDL_GetTicksNS();
    Uint64 next = viewer->src.v.anim.next_frame_ns;
    if (next <= now) return 0;
    Uint64 diff_ns = next - now;
    Uint64 diff_ms = (diff_ns + 999999) / 1000000;
    if (diff_ms > (Uint64)INT32_MAX) diff_ms = INT32_MAX;
    return (Sint32)diff_ms;
}

static void maybe_advance_anim(Viewer *viewer, bool *out_redraw) {
    if (viewer->src.kind != IMG_ANIM) return;
    if (viewer->paused) return;
    AnimImage *a = &viewer->src.v.anim;
    /* anim_advance_if_due will either advance to a new frame (returns true,
       updates rgba + next_frame_ns) or reschedule for a later check
       (returns false). Loop in case multiple frames are overdue (e.g. we
       were paused for a long time). */
    Uint64 guard_start = SDL_GetTicksNS();
    while (SDL_GetTicksNS() >= a->next_frame_ns) {
        bool changed = anim_advance_if_due(a);
        if (changed) {
            if (!SDL_UpdateTexture(viewer->texture, NULL,
                                   a->rgba, a->width * 4)) {
                die_sdl("SDL_UpdateTexture");
                return;
            }
            /* If the frame cache exists, keep its index roughly in sync
               with what the live iterator is showing so a subsequent
               skim starts at a sensible frame. We just increment with
               wrap; it'll drift if the iter doesn't go strictly forward,
               but that's harmless. */
            if (viewer->frames_decoded && viewer->frame_count > 0) {
                viewer->current_frame =
                    (viewer->current_frame + 1) % viewer->frame_count;
            }
            *out_redraw = true;
        }
        /* Safety: don't spin forever on a pathological file. */
        if (SDL_GetTicksNS() - guard_start > 100ULL * 1000000ULL) {
            a->next_frame_ns = SDL_GetTicksNS() + 16ULL * 1000000ULL;
            break;
        }
        if (!changed) break;
    }
}

/* True if the SVG pipeline has unfinished business — we owe a sharp
   tile for the current generation, or the worker is in flight, or
   it's delivered one we haven't picked up yet. While true, the event
   loop wants a periodic tick so render() can drive the pipeline. */
static bool svg_pipeline_pending(Viewer *viewer) {
    if (viewer->src.kind != IMG_SVG) return false;
    SvgImage *svg = &viewer->src.v.svg;
    bool need_request = !svg->tile_valid
        || svg->tile_generation != svg->current_generation;
    SDL_LockMutex(svg->mu);
    bool worker_busy = svg->job_pending || svg->job_done;
    SDL_UnlockMutex(svg->mu);
    return need_request || worker_busy;
}

/* Time (ms) until the event loop should wake. Negative = wait forever
   (no animation, no SVG pipeline work). */
static Sint32 time_until_next_wake_ms(Viewer *viewer) {
    Sint32 best = -1;

    Sint32 anim_ms = time_until_next_anim_frame_ms(viewer);
    if (anim_ms >= 0 && (best < 0 || anim_ms < best)) best = anim_ms;

    if (svg_pipeline_pending(viewer)) {
        if (best < 0 || best > 30) best = 30;
    }

    return best;
}

static void event_loop(Viewer *viewer) {
    bool running = true;

    /* Drag-drop batching state. SDL3 fires multi-file drops as a
       BEGIN, one DROP_FILE per item, then COMPLETE. While a batch is
       open we accumulate paths here instead of opening each one as it
       arrives; on COMPLETE we either open the single file (preserving
       directory auto-detect via open_path) or build a playlist from
       the whole batch. The batch is also "implicit" — if a DROP_FILE
       arrives without a BEGIN (some compositors send it that way for
       single drops) we open it inline. */
    bool   drop_batch_open    = false;
    char **drop_batch_paths   = NULL;
    int    drop_batch_count   = 0;
    int    drop_batch_cap     = 0;

    render(viewer);

    while (running) {
        bool redraw = false;

        /* Advance any due GIF frames first so the timeout below is honest. */
        maybe_advance_anim(viewer, &redraw);

        /* Tick render() at ~30 Hz while SVG pipeline work is pending
           so it can drive the worker and pick up finished tiles. */
        if (svg_pipeline_pending(viewer)) redraw = true;

        SDL_Event event;
        Sint32 timeout = time_until_next_wake_ms(viewer);
        bool got = (timeout < 0)
                 ? SDL_WaitEvent(&event)
                 : SDL_WaitEventTimeout(&event, timeout);

        if (got) {
            do {
                switch (event.type) {
                case SDL_EVENT_QUIT:
                    running = false;
                    break;

                case SDL_EVENT_WINDOW_EXPOSED:
                case SDL_EVENT_WINDOW_RESIZED:
                case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                    redraw = true;
                    break;

                case SDL_EVENT_KEY_DOWN: {
                    SDL_Keycode key = event.key.key;
                    /* Most keys are one-shot; ignore OS key-repeat for them.
                       Frame-skim and directory navigation feel better with
                       auto-repeat so the user can hold to scrub or page. */
                    bool allow_repeat = (key == SDLK_MINUS ||
                                         key == SDLK_EQUALS ||
                                         key == SDLK_COMMA ||
                                         key == SDLK_PERIOD);
                    if (!event.key.repeat || allow_repeat) {
                        switch (key) {
                        case SDLK_ESCAPE:
                        case SDLK_Q:
                            running = false;
                            break;
                        case SDLK_F:
                            toggle_fullscreen(viewer);
                            redraw = true;
                            break;
                        case SDLK_R:
                            reset_view(viewer);
                            redraw = true;
                            break;
                        case SDLK_N:
                            toggle_nearest(viewer);
                            redraw = true;
                            break;
                        case SDLK_I:
                            toggle_info(viewer);
                            redraw = true;
                            break;
                        case SDLK_B:
                            viewer->binds_on = !viewer->binds_on;
                            redraw = true;
                            break;
                        case SDLK_A:
                            toggle_aspect_lock(viewer);
                            redraw = true;
                            break;
                        case SDLK_H:
                            viewer->flip_h = !viewer->flip_h;
                            redraw = true;
                            break;
                        case SDLK_V:
                            viewer->flip_v = !viewer->flip_v;
                            redraw = true;
                            break;
                        case SDLK_K:
                            cycle_rotation(viewer, -1);
                            redraw = true;
                            break;
                        case SDLK_L:
                            cycle_rotation(viewer, +1);
                            redraw = true;
                            break;
                        case SDLK_T:
                            toggle_tile(viewer);
                            redraw = true;
                            break;
                        case SDLK_W:
                            viewer->tile_mirror = !viewer->tile_mirror;
                            redraw = true;
                            break;
                        case SDLK_P:
                            toggle_pause(viewer);
                            redraw = true;
                            break;
                        case SDLK_MINUS:
                            skim_frame(viewer, -1);
                            redraw = true;
                            break;
                        case SDLK_EQUALS:
                            skim_frame(viewer, +1);
                            redraw = true;
                            break;
                        case SDLK_COMMA:
                            if (nav_to(viewer, viewer->dir.current - 1))
                                redraw = true;
                            break;
                        case SDLK_PERIOD:
                            if (nav_to(viewer, viewer->dir.current + 1))
                                redraw = true;
                            break;
                        case SDLK_HOME:
                            if (nav_to(viewer, 0)) redraw = true;
                            break;
                        case SDLK_END:
                            if (nav_to(viewer, viewer->dir.count - 1))
                                redraw = true;
                            break;
                        case SDLK_1: case SDLK_2: case SDLK_3:
                        case SDLK_4: case SDLK_5: case SDLK_6:
                        case SDLK_7: case SDLK_8: case SDLK_9:
                            if (viewer->tile_on) {
                                viewer->tile_radius = (int)(key - SDLK_0);
                                redraw = true;
                            }
                            break;
                        default:
                            break;
                        }
                        if (viewer->info_on && redraw) rebuild_info_lines(viewer);
                    }
                    break;
                }

                case SDL_EVENT_MOUSE_WHEEL: {
                    float y = event.wheel.y;
                    double old_zoom = viewer->zoom;
                    double new_zoom = old_zoom;
                    if (y > 0.0f) {
                        new_zoom *= pow(ZOOM_STEP, (double)y);
                    } else if (y < 0.0f) {
                        new_zoom /= pow(ZOOM_STEP, (double)-y);
                    }
                    new_zoom = clamp_double(new_zoom, ZOOM_MIN, ZOOM_MAX);

                    /* Keep the image point currently under the cursor
                       stationary in screen space. The image rect grows
                       around (window_center + pan_off) since the
                       anchor stays at (0.5, 0.5), so we adjust pan_off
                       so the cursor's offset from the image center
                       scales by the same factor as the image itself. */
                    if (new_zoom != old_zoom && old_zoom > 0.0) {
                        int win_w = 1, win_h = 1;
                        SDL_GetWindowSize(viewer->window, &win_w, &win_h);
                        double cx = (double)win_w * 0.5;
                        double cy = (double)win_h * 0.5;
                        double mx = (double)event.wheel.mouse_x;
                        double my = (double)event.wheel.mouse_y;
                        double k = new_zoom / old_zoom;
                        viewer->pan_off_x = (1.0 - k) * (mx - cx)
                                          + k * viewer->pan_off_x;
                        viewer->pan_off_y = (1.0 - k) * (my - cy)
                                          + k * viewer->pan_off_y;
                    }
                    viewer->zoom = new_zoom;

                    if (viewer->info_on) rebuild_info_lines(viewer);
                    redraw = true;
                    break;
                }

                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        viewer->panning = true;
                        SDL_SetWindowMouseGrab(viewer->window, true);
                    }
                    break;

                case SDL_EVENT_MOUSE_BUTTON_UP:
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        viewer->panning = false;
                        SDL_SetWindowMouseGrab(viewer->window, false);
                    }
                    break;

                case SDL_EVENT_MOUSE_MOTION:
                    if (viewer->panning) {
                        pan_by(viewer,
                               (double)event.motion.xrel,
                               (double)event.motion.yrel);
                        redraw = true;
                    }
                    break;

                case SDL_EVENT_DROP_BEGIN:
                    /* Start collecting paths until DROP_COMPLETE. */
                    drop_batch_open  = true;
                    drop_batch_count = 0;
                    break;

                case SDL_EVENT_DROP_FILE:
                    if (event.drop.data) {
                        if (drop_batch_open) {
                            /* Append to the batch. event.drop.data is
                               owned by SDL and only valid for this
                               event, so dup it. */
                            if (drop_batch_count == drop_batch_cap) {
                                int new_cap = drop_batch_cap ? drop_batch_cap * 2 : 8;
                                char **g = realloc(drop_batch_paths,
                                                   sizeof(char *) * (size_t)new_cap);
                                if (g) {
                                    drop_batch_paths = g;
                                    drop_batch_cap = new_cap;
                                }
                            }
                            if (drop_batch_count < drop_batch_cap) {
                                char *dup = strdup(event.drop.data);
                                if (dup) drop_batch_paths[drop_batch_count++] = dup;
                            }
                        } else {
                            /* No BEGIN bracketing — open immediately.
                               open_path() handles single files and
                               directories. */
                            if (open_path(viewer, event.drop.data)) {
                                redraw = true;
                            } else {
                                fprintf(stderr,
                                        "%s: failed to open dropped path '%s'\n",
                                        APP_NAME, event.drop.data);
                            }
                        }
                    }
                    break;

                case SDL_EVENT_DROP_COMPLETE:
                    if (drop_batch_open) {
                        if (drop_batch_count == 1) {
                            /* Single file/dir dropped: same as old behavior. */
                            if (open_path(viewer, drop_batch_paths[0])) {
                                redraw = true;
                            } else {
                                fprintf(stderr,
                                        "%s: failed to open dropped path '%s'\n",
                                        APP_NAME, drop_batch_paths[0]);
                            }
                        } else if (drop_batch_count > 1) {
                            /* Multiple files: build an ad-hoc playlist
                               and open the first. If any entry is a
                               directory it's silently treated as a
                               file path here (open_path would expand
                               a dir, but a playlist of mixed dirs+files
                               doesn't make sense — the user can drop a
                               single dir if they want that). */
                            if (reopen_image(viewer, drop_batch_paths[0])) {
                                DirList dl;
                                memset(&dl, 0, sizeof(dl));
                                if (dirlist_from_paths(&dl,
                                        (const char *const *)drop_batch_paths,
                                        drop_batch_count)) {
                                    dirlist_free(&viewer->dir);
                                    viewer->dir = dl;
                                    viewer->dir.current = 0;
                                    rebuild_info_lines(viewer);
                                }
                                redraw = true;
                            } else {
                                fprintf(stderr,
                                        "%s: failed to open first of %d "
                                        "dropped paths\n",
                                        APP_NAME, drop_batch_count);
                            }
                        }
                        /* Tear down the batch regardless. */
                        for (int k = 0; k < drop_batch_count; k++) {
                            free(drop_batch_paths[k]);
                        }
                        drop_batch_count = 0;
                        drop_batch_open  = false;
                    }
                    break;

                default:
                    break;
                }
            } while (running && SDL_PollEvent(&event));
        }

        if (redraw && running) render(viewer);
    }

    /* If we exit mid-batch (rare; e.g. user closes the window during
       a drag), free anything we accumulated. */
    for (int k = 0; k < drop_batch_count; k++) free(drop_batch_paths[k]);
    free(drop_batch_paths);
}

/* ---------- main ---------- */

int main(int argc, char **argv) {
    Options opts;
    int parse_result = parse_options(argc, argv, &opts);
    if (parse_result == 1) return 0;   /* --help / --version */
    if (parse_result == 2) return 2;   /* parse error */

    /* Tell SDL who we are. SDL_HINT_APP_ID becomes the Wayland xdg-shell
       app_id and the X11 WM_CLASS, which is what compositors match against
       a .desktop file to look up the icon. Must be set before SDL_Init. */
    SDL_SetHint(SDL_HINT_APP_ID, opts.app_id);
    SDL_SetAppMetadata(APP_TITLE, APP_VERSION, opts.app_id);

    /* --x11 / --wayland force the video backend BEFORE SDL_Init. SDL
       respects either SDL_HINT_VIDEO_DRIVER or the SDL_VIDEODRIVER env. */
    if (opts.force_x11) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
        setenv("SDL_VIDEODRIVER", "x11", 1);
    } else if (opts.force_wayland) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland");
        setenv("SDL_VIDEODRIVER", "wayland", 1);
    }

    Viewer viewer;
    memset(&viewer, 0, sizeof(viewer));
    viewer.opts = &opts;

    gegl_init(&argc, &argv);

    /* If at least one path was given on the command line, load the
       first one eagerly so we can size the initial window to it. For
       a directory, defer to open_path() after the window exists (it
       populates the nav list and opens the first image). For multiple
       files, we eagerly load the first and build the playlist after
       the window exists. */
    const char *first_path = (opts.image_count > 0) ? opts.image_paths[0] : NULL;
    bool is_dir_arg     = false;
    bool is_multi_arg   = (opts.image_count > 1);
    if (first_path) {
        if (is_directory(first_path)) {
            is_dir_arg = true;
            /* Multi-arg with the first being a directory: not supported
               cleanly (mixing a dir-listing with explicit files would
               be confusing). Treat it as the dir case and warn. */
            if (is_multi_arg) {
                fprintf(stderr,
                        "%s: first path is a directory; ignoring the "
                        "remaining %d argument(s)\n",
                        APP_NAME, opts.image_count - 1);
                is_multi_arg = false;
            }
            /* Don't load via load_source yet; need the window & dirlist. */
        } else if (!load_source(first_path, &viewer.src)) {
            gegl_exit();
            return 1;
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        die_sdl("SDL_Init");
        free_source(&viewer.src);
        gegl_exit();
        return 1;
    }

    const char *window_title = opts.title
        ? opts.title
        : (first_path ? first_path : APP_TITLE);
    if (!create_sdl(&viewer, window_title)) {
        destroy_viewer(&viewer);
        SDL_Quit();
        gegl_exit();
        return 1;
    }

    /* Populate current_path / current_size_bytes for the eagerly-loaded
       file case (the directory case goes through open_path below, which
       sets these via reopen_image). */
    if (first_path && !is_dir_arg && viewer.src.width > 0) {
        viewer.current_path = strdup(first_path);
        struct stat st;
        viewer.current_size_bytes =
            (stat(first_path, &st) == 0) ? (size_t)st.st_size : 0;
        rebuild_info_lines(&viewer);
    }

    /* If launched with a directory, do that now (window exists, so any
       failures can be reported but the window stays up). */
    if (is_dir_arg) {
        if (!open_path(&viewer, first_path)) {
            fprintf(stderr,
                    "%s: directory '%s' had no openable images; "
                    "starting blank — drop a file to begin\n",
                    APP_NAME, first_path);
        }
    }

    /* Multi-file ad-hoc playlist: the first image is already loaded
       (eager load above), so just build the DirList from all the CLI
       paths and point current at index 0. */
    if (is_multi_arg) {
        DirList dl;
        memset(&dl, 0, sizeof(dl));
        if (dirlist_from_paths(&dl, opts.image_paths, opts.image_count)) {
            dirlist_free(&viewer.dir);
            viewer.dir = dl;
            viewer.dir.current = 0;
            rebuild_info_lines(&viewer);
        } else {
            fprintf(stderr,
                    "%s: out of memory building image playlist; nav "
                    "keys (, .) will be disabled\n",
                    APP_NAME);
        }
    }

    event_loop(&viewer);

    destroy_viewer(&viewer);
    SDL_Quit();
    gegl_exit();
    free(opts.image_paths); /* shallow: argv strings are not owned */
    return 0;
}
