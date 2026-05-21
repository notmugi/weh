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

#define APP_NAME    "cimg"
#define APP_TITLE   "cimg"
#define APP_ID      "io.github.notmugi.cimg"
#define APP_VERSION "0.1.0"
#define WINDOW_MARGIN 64
#define ZOOM_STEP 1.125
#define ZOOM_MIN 0.02
#define ZOOM_MAX 128.0
/* Maximum raster buffer we will ever produce for an SVG. If the visible
   region would exceed this, we rasterize at the cap and let SDL bilinearly
   scale the texture. */
#define SVG_MAX_RASTER_DIM 4096

/* The always-on low-res backdrop is rasterized once at load. Its long edge
   is at most this many pixels. SDL bilinearly scales it to whatever the
   current view requires. */
#define SVG_BACKDROP_MAX_DIM 1024
/* After the last interaction event, wait this long before kicking off a
   final-quality re-rasterization. Keeps wheel/pan smooth. */
#define SVG_DEBOUNCE_NS  (60ULL * 1000000ULL)  /* 60 ms */

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

/* SVG render plan: describes a sub-region of the SVG's "natural" image
   coordinate space (in image pixels at scale 1.0) and the scale at which
   to rasterize it. The rasterized output buffer has size (out_w, out_h)
   pixels and represents image region
       [ix, iy, ix + out_w/scale, iy + out_h/scale].
   When zoomed out, this region covers the entire SVG. When zoomed in,
   it covers only the visible sub-rect, capped at SVG_MAX_RASTER_DIM. */
typedef struct SvgPlan {
    float ix;      /* image-space top-left x (units = SVG user units) */
    float iy;      /* image-space top-left y                          */
    float scale;   /* output pixels per SVG user unit                 */
    int   out_w;   /* output pixels wide                              */
    int   out_h;   /* output pixels tall                              */
} SvgPlan;

typedef struct SvgImage {
    /* librsvg handle, owned by this struct. RsvgHandle is GObject-style,
       so we g_object_ref/unref to share with the worker thread (librsvg's
       handle is read-safe for concurrent renders since 2.36). */
    RsvgHandle *handle;
    /* Intrinsic dimensions in user units (CSS pixels), as floats — used
       directly when computing cairo render scales. */
    double natural_w_f;
    double natural_h_f;
    int natural_w;
    int natural_h;

    /* --- backdrop (full image, low-res, rasterized once at load) --- */
    /* Drawn beneath the sharp clipped buffer so panning never reveals
       black where the high-quality raster hasn't covered yet. Bilinearly
       stretched by the GPU; never reraster ed after creation. */
    unsigned char *backdrop;
    int            backdrop_w;
    int            backdrop_h;
    SDL_Texture   *backdrop_tex; /* lives in renderer; created in create_sdl */

    /* --- shown buffer (owned by main thread) --- */
    /* The most recent fully-rasterized buffer that the SDL texture
       currently mirrors. May be smaller than the SVG when zoomed in. */
    unsigned char *shown;
    size_t        shown_cap;     /* allocated bytes of `shown` */
    SvgPlan shown_plan;

    /* --- scratch buffer the worker rasterizes into --- */
    unsigned char *scratch;
    size_t scratch_cap;          /* allocated bytes of `scratch` */
    SvgPlan scratch_plan;        /* plan currently being filled / just filled */

    /* --- threading --- */
    SDL_Thread *worker;
    SDL_Mutex  *mu;
    SDL_Condition *cv;           /* worker waits on this */
    bool job_pending;            /* main → worker: there's a job (plan in pending_plan) */
    bool job_done;               /* worker → main: scratch is freshly populated */
    bool quit;                   /* main → worker: exit */
    SvgPlan pending_plan;        /* the plan the worker should produce next */

    /* --- pacing / dirty tracking (main thread only) --- */
    bool   dirty;
    Uint64 dirty_since_ns;
    SvgPlan last_requested_plan; /* what main thread has most recently queued */
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
   All fields have sensible defaults; only `image_path` is required. */
typedef struct Options {
    const char *image_path;     /* positional argument; NULL → start blank */
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
        "Usage: %s [OPTIONS] [IMAGE|DIRECTORY]\n"
        "\n"
        "A tiny image viewer for X11 and Wayland.\n"
        "\n"
        "If IMAGE is omitted, cimg opens a blank window; drag-drop an image\n"
        "or a directory onto it to start viewing.\n"
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
        "      --bg=#RRGGBB        Background / letterbox color (default 000000)\n"
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
        "  , / .              Previous / next image in directory (with wrap)\n"
        "  Home / End         First / last image in directory\n"
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
    opts->bg_r = opts->bg_g = opts->bg_b = 0;

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

    /* IMAGE is optional. With no argument, we launch a blank window
       and wait for a drag-drop. */
    if (i < argc) {
        opts->image_path = argv[i++];
    }
    if (i < argc) {
        fprintf(stderr, "%s: extra arguments after IMAGE\n", APP_NAME);
        return 2;
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

/* Rasterize a sub-region of the SVG into `dst` (out_w * out_h RGBA32).
   Output is straight (un-premultiplied) RGBA. Returns true on success.

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
    /* Zero the destination so any uncovered pixels are transparent. */
    memset(dst, 0, (size_t)out_w * (size_t)out_h * 4u);

    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, out_w);
    /* We allocated exactly out_w * 4 bytes per row, but cairo may want
       padding. If stride doesn't match, render into a separate cairo
       buffer and copy out. The common case (no padding) is fast-path. */
    cairo_surface_t *surface;
    unsigned char *cbuf = NULL;
    bool need_copy = (stride != out_w * 4);
    if (need_copy) {
        cbuf = calloc(1, (size_t)stride * (size_t)out_h);
        if (!cbuf) return false;
        surface = cairo_image_surface_create_for_data(
            cbuf, CAIRO_FORMAT_ARGB32, out_w, out_h, stride);
    } else {
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

    if (need_copy) {
        for (int y = 0; y < out_h; y++) {
            memcpy(dst + (size_t)y * out_w * 4u,
                   cbuf + (size_t)y * stride,
                   (size_t)out_w * 4u);
        }
    }
    cairo_surface_destroy(surface);
    free(cbuf);

    cairo_argb32_to_rgba_inplace(dst, out_w, out_h);
    return true;
}

/* Worker thread: rasterizes whatever plan main thread puts in pending_plan
   into svg->scratch (resized as needed), then signals job_done. Loops until
   svg->quit. */
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
        SvgPlan plan = svg->pending_plan;
        svg->job_pending = false;
        SDL_UnlockMutex(svg->mu);

        /* Allocate / grow the scratch buffer outside the lock. */
        size_t bytes = (size_t)plan.out_w * (size_t)plan.out_h * 4u;
        if (bytes > svg->scratch_cap) {
            unsigned char *grown = realloc(svg->scratch, bytes);
            if (!grown) {
                fprintf(stderr, "%s: SVG worker OOM at %dx%d\n",
                        APP_NAME, plan.out_w, plan.out_h);
                /* Pretend done with a zero-size plan; main will ignore. */
                SDL_LockMutex(svg->mu);
                svg->scratch_plan = (SvgPlan){0};
                svg->job_done = true;
                SDL_UnlockMutex(svg->mu);
                continue;
            }
            svg->scratch = grown;
            svg->scratch_cap = bytes;
        }
        svg_render_region(svg->handle,
                          svg->natural_w_f, svg->natural_h_f,
                          (double)plan.ix, (double)plan.iy,
                          (double)plan.scale,
                          svg->scratch, plan.out_w, plan.out_h);

        SDL_LockMutex(svg->mu);
        svg->scratch_plan = plan;
        svg->job_done = true;
        SDL_UnlockMutex(svg->mu);
    }
}

/* Rasterize the entire SVG at a small fixed resolution. Called once on
   the main thread, before the worker starts. Stores into svg->backdrop. */
static bool svg_rasterize_backdrop(SvgImage *svg) {
    double w = svg->natural_w_f;
    double h = svg->natural_h_f;
    if (w <= 0.0 || h <= 0.0) return false;

    double long_edge = w > h ? w : h;
    double scale = 1.0;
    if (long_edge > (double)SVG_BACKDROP_MAX_DIM) {
        scale = (double)SVG_BACKDROP_MAX_DIM / long_edge;
    }
    int bw = (int)ceil(w * scale);
    int bh = (int)ceil(h * scale);
    if (bw < 1) bw = 1;
    if (bh < 1) bh = 1;

    unsigned char *buf = calloc(1, (size_t)bw * (size_t)bh * 4u);
    if (!buf) {
        fprintf(stderr, "%s: out of memory rasterizing SVG backdrop\n", APP_NAME);
        return false;
    }

    if (!svg_render_region(svg->handle, w, h, 0.0, 0.0, scale, buf, bw, bh)) {
        free(buf);
        return false;
    }

    svg->backdrop = buf;
    svg->backdrop_w = bw;
    svg->backdrop_h = bh;
    return true;
}

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

static bool load_svg(const char *path, Source *out) {
    SvgImage svg;
    memset(&svg, 0, sizeof(svg));

    GError *err = NULL;
    svg.handle = rsvg_handle_new_from_file(path, &err);
    if (!svg.handle) {
        if (err) {
            fprintf(stderr, "%s: librsvg failed to load '%s': %s\n",
                    APP_NAME, path, err->message);
            g_error_free(err);
        }
        return false;
    }

    if (!svg_get_natural_size(svg.handle,
                              &svg.natural_w_f, &svg.natural_h_f)) {
        g_object_unref(svg.handle);
        return false;
    }
    svg.natural_w = (int)ceil(svg.natural_w_f);
    svg.natural_h = (int)ceil(svg.natural_h_f);
    if (svg.natural_w < 1) svg.natural_w = 1;
    if (svg.natural_h < 1) svg.natural_h = 1;

    /* Backdrop: one-shot low-res raster of the whole image. Always shown
       beneath the sharp clipped raster so panning never reveals black. */
    if (!svg_rasterize_backdrop(&svg)) {
        g_object_unref(svg.handle);
        return false;
    }

    svg.mu = SDL_CreateMutex();
    svg.cv = SDL_CreateCondition();
    if (!svg.mu || !svg.cv) {
        if (svg.cv) SDL_DestroyCondition(svg.cv);
        if (svg.mu) SDL_DestroyMutex(svg.mu);
        free(svg.backdrop);
        g_object_unref(svg.handle);
        return false;
    }

    out->kind = IMG_SVG;
    out->width = svg.natural_w;
    out->height = svg.natural_h;
    out->v.svg = svg;

    /* Worker is started AFTER the SvgImage is stored in *out, because we
       must hand it the pointer that lives in the viewer struct (not a
       stack copy). The caller (create_sdl) starts the worker. */
    return true;
}

static bool svg_start_worker(SvgImage *svg) {
    svg->worker = SDL_CreateThread(svg_worker_main, "gimp-open-svg", svg);
    if (!svg->worker) {
        fprintf(stderr, "%s: failed to start SVG worker thread: %s\n",
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

/* Main-thread API: queue a job. Overwrites any pending job (latest wins).
   Returns true if the request was newly different from the in-progress one. */
static void svg_request(SvgImage *svg, const SvgPlan *plan) {
    SDL_LockMutex(svg->mu);
    svg->pending_plan = *plan;
    svg->job_pending = true;
    svg->last_requested_plan = *plan;
    SDL_SignalCondition(svg->cv);
    SDL_UnlockMutex(svg->mu);
}

/* Main-thread API: try to consume a completed job. If one is ready, swap
   the scratch buffer into shown and return true (caller should upload to
   GPU and redraw). Caller passes &out_plan to learn what was just adopted. */
static bool svg_try_consume(SvgImage *svg, SvgPlan *out_plan) {
    bool consumed = false;
    SDL_LockMutex(svg->mu);
    if (svg->job_done) {
        /* Swap buffers AND their capacities so each member's *_cap stays
           consistent with the buffer it currently names. */
        unsigned char *tb = svg->shown;
        size_t         tc = svg->shown_cap;
        svg->shown      = svg->scratch;
        svg->shown_cap  = svg->scratch_cap;
        svg->scratch    = tb;
        svg->scratch_cap = tc;
        svg->shown_plan = svg->scratch_plan;
        svg->job_done = false;
        consumed = true;
        if (out_plan) *out_plan = svg->shown_plan;
    }
    SDL_UnlockMutex(svg->mu);
    return consumed;
}

static void free_svg(SvgImage *svg) {
    svg_stop_worker(svg);
    if (svg->backdrop_tex) SDL_DestroyTexture(svg->backdrop_tex);
    if (svg->cv) SDL_DestroyCondition(svg->cv);
    if (svg->mu) SDL_DestroyMutex(svg->mu);
    if (svg->handle) g_object_unref(svg->handle);
    free(svg->backdrop);
    free(svg->shown);
    free(svg->scratch);
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

    /* For static and GIF: create a texture at natural size and upload the
       first frame. For SVG: defer; the texture is recreated lazily at
       the actually-rendered size on each render. With no image loaded,
       skip texture setup; the blank window simply paints the background
       color. */
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
        SvgImage *svg = &viewer->src.v.svg;
        /* Upload the one-shot backdrop into its own texture. The sharp
           texture (viewer->texture) is created lazily in render() once the
           worker produces a buffer. */
        svg->backdrop_tex = SDL_CreateTexture(viewer->renderer,
                                              SDL_PIXELFORMAT_RGBA32,
                                              SDL_TEXTUREACCESS_STATIC,
                                              svg->backdrop_w,
                                              svg->backdrop_h);
        if (!svg->backdrop_tex) {
            die_sdl("SDL_CreateTexture (backdrop)");
            return false;
        }
        SDL_SetTextureScaleMode(svg->backdrop_tex, SDL_SCALEMODE_LINEAR);
        if (!SDL_UpdateTexture(svg->backdrop_tex, NULL,
                               svg->backdrop, svg->backdrop_w * 4)) {
            die_sdl("SDL_UpdateTexture (backdrop)");
            return false;
        }
        if (!svg_start_worker(svg)) {
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
        "%s/icons/hicolor/scalable/apps/cimg.svg",
        /* PNGs in descending size order. */
        "%s/icons/hicolor/512x512/apps/cimg.png",
        "%s/icons/hicolor/256x256/apps/cimg.png",
        "%s/icons/hicolor/128x128/apps/cimg.png",
        "%s/icons/hicolor/64x64/apps/cimg.png",
        "%s/icons/hicolor/48x48/apps/cimg.png",
        "%s/icons/hicolor/32x32/apps/cimg.png",
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

/* Drop GPU resources tied to the current Source (textures + SVG bits).
   Used internally by reopen_image and during teardown. */
static void release_source_gpu(Viewer *viewer) {
    if (viewer->src.kind == IMG_SVG) {
        svg_stop_worker(&viewer->src.v.svg);
        if (viewer->src.v.svg.backdrop_tex) {
            SDL_DestroyTexture(viewer->src.v.svg.backdrop_tex);
            viewer->src.v.svg.backdrop_tex = NULL;
        }
    }
    if (viewer->texture) {
        SDL_DestroyTexture(viewer->texture);
        viewer->texture = NULL;
        viewer->texture_w = 0;
        viewer->texture_h = 0;
    }
}

/* Open a new image file in-place, preserving the existing window/renderer.
   Used by drag-and-drop and directory navigation. Returns true on success;
   on failure the previous image (if any) is restored. */
static bool reopen_image(Viewer *viewer, const char *path) {
    release_source_gpu(viewer);
    free_frame_cache(viewer);
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

    /* Update window title and aspect lock for the new image. */
    SDL_SetWindowTitle(viewer->window,
                       (viewer->opts && viewer->opts->title)
                         ? viewer->opts->title : path);
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
        svg->backdrop_tex = SDL_CreateTexture(viewer->renderer,
                                              SDL_PIXELFORMAT_RGBA32,
                                              SDL_TEXTUREACCESS_STATIC,
                                              svg->backdrop_w,
                                              svg->backdrop_h);
        if (svg->backdrop_tex) {
            SDL_SetTextureScaleMode(svg->backdrop_tex, SDL_SCALEMODE_LINEAR);
            SDL_UpdateTexture(svg->backdrop_tex, NULL,
                              svg->backdrop, svg->backdrop_w * 4);
        }
        svg_start_worker(svg);
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

/* ---------- per-frame geometry helpers ---------- */

static void compute_draw_size(const Viewer *viewer,
                              int window_w, int window_h,
                              double *out_draw_w, double *out_draw_h) {
    double image_aspect = (double)viewer->src.width / (double)viewer->src.height;
    double window_aspect = (double)window_w / (double)window_h;

    double fit_w, fit_h;
    if (window_aspect > image_aspect) {
        fit_h = (double)window_h;
        fit_w = (double)window_h * image_aspect;
    } else {
        fit_w = (double)window_w;
        fit_h = (double)window_w / image_aspect;
    }

    double zoom = viewer->zoom;
    if (zoom <= 0.0 || !isfinite(zoom)) zoom = 1.0;

    double dw = fit_w * zoom;
    double dh = fit_h * zoom;
    if (dw < 1.0) dw = 1.0;
    if (dh < 1.0) dh = 1.0;
    *out_draw_w = dw;
    *out_draw_h = dh;
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

/* Compute the on-screen rectangle that the full image would occupy at the
   current view (window dimensions, zoom, anchor, pan offset). Equivalent
   to the dst rect the previous render used, but separated so SVG can map
   its sub-region to screen coordinates within this same rect. */
static void compute_full_image_dst(const Viewer *viewer,
                                   int window_w, int window_h,
                                   double *dx, double *dy,
                                   double *dw, double *dh) {
    double draw_w, draw_h;
    compute_draw_size(viewer, window_w, window_h, &draw_w, &draw_h);
    double sx, sy;
    window_to_pixel_scale(viewer, &sx, &sy);
    *dw = draw_w;
    *dh = draw_h;
    *dx = (double)window_w * 0.5 - viewer->anchor_x * draw_w
        + viewer->pan_off_x * sx;
    *dy = (double)window_h * 0.5 - viewer->anchor_y * draw_h
        + viewer->pan_off_y * sy;
}

/* Compute the SVG plan we WANT for the current view: rasterize only the
   visible part of the image, at a scale equal to the current on-screen
   pixels-per-image-pixel, capped at SVG_MAX_RASTER_DIM. */
static SvgPlan svg_plan_for_view(const Viewer *viewer,
                                 int window_w, int window_h) {
    const SvgImage *svg = &viewer->src.v.svg;
    double full_dx, full_dy, full_dw, full_dh;
    compute_full_image_dst(viewer, window_w, window_h,
                           &full_dx, &full_dy, &full_dw, &full_dh);

    /* Intersect the full image's on-screen rect with the window viewport
       to get the visible portion in screen coords. */
    double vis_x0 = full_dx > 0.0 ? full_dx : 0.0;
    double vis_y0 = full_dy > 0.0 ? full_dy : 0.0;
    double vis_x1 = full_dx + full_dw;
    double vis_y1 = full_dy + full_dh;
    if (vis_x1 > (double)window_w) vis_x1 = (double)window_w;
    if (vis_y1 > (double)window_h) vis_y1 = (double)window_h;
    if (vis_x1 < vis_x0) vis_x1 = vis_x0;
    if (vis_y1 < vis_y0) vis_y1 = vis_y0;

    /* Map visible screen rect → SVG user-unit rect. */
    double pixels_per_unit_x = full_dw / svg->natural_w_f;
    double pixels_per_unit_y = full_dh / svg->natural_h_f;
    /* Aspect is locked when rendering, so the two should agree very closely;
       pick the average for the scale used by the rasterizer. */
    double pixels_per_unit = 0.5 * (pixels_per_unit_x + pixels_per_unit_y);
    if (pixels_per_unit <= 0.0 || !isfinite(pixels_per_unit)) pixels_per_unit = 1.0;
    if (pixels_per_unit_x <= 0.0) pixels_per_unit_x = pixels_per_unit;
    if (pixels_per_unit_y <= 0.0) pixels_per_unit_y = pixels_per_unit;

    double ix0 = (vis_x0 - full_dx) / pixels_per_unit_x;
    double iy0 = (vis_y0 - full_dy) / pixels_per_unit_y;
    double ix1 = (vis_x1 - full_dx) / pixels_per_unit_x;
    double iy1 = (vis_y1 - full_dy) / pixels_per_unit_y;

    /* Slight padding (1px in image units) so panning a little doesn't
       expose unrasterized edges between debounced raster jobs. */
    double pad = 1.0;
    ix0 -= pad; iy0 -= pad;
    ix1 += pad; iy1 += pad;
    if (ix0 < 0.0) ix0 = 0.0;
    if (iy0 < 0.0) iy0 = 0.0;
    if (ix1 > svg->natural_w_f) ix1 = svg->natural_w_f;
    if (iy1 > svg->natural_h_f) iy1 = svg->natural_h_f;

    double iw = ix1 - ix0;
    double ih = iy1 - iy0;
    if (iw < 1.0) iw = 1.0;
    if (ih < 1.0) ih = 1.0;

    /* Target output pixels for this region. */
    double out_w_d = iw * pixels_per_unit;
    double out_h_d = ih * pixels_per_unit;

    /* Apply the raster cap by reducing the effective scale, not by
       clipping further. This makes the cap behave as "after this many
       pixels, fall back to GPU bilinear scaling". */
    double max_out = (double)SVG_MAX_RASTER_DIM;
    double cap_scale = 1.0;
    if (out_w_d > max_out) cap_scale = max_out / out_w_d;
    if (out_h_d > max_out) {
        double s2 = max_out / out_h_d;
        if (s2 < cap_scale) cap_scale = s2;
    }
    double scale = pixels_per_unit * cap_scale;
    int out_w = (int)floor(iw * scale + 0.5);
    int out_h = (int)floor(ih * scale + 0.5);
    if (out_w < 1) out_w = 1;
    if (out_h < 1) out_h = 1;
    if (out_w > SVG_MAX_RASTER_DIM) out_w = SVG_MAX_RASTER_DIM;
    if (out_h > SVG_MAX_RASTER_DIM) out_h = SVG_MAX_RASTER_DIM;

    SvgPlan plan;
    plan.ix = (float)ix0;
    plan.iy = (float)iy0;
    plan.scale = (float)scale;
    plan.out_w = out_w;
    plan.out_h = out_h;
    return plan;
}

/* Build a plan that renders the WHOLE SVG at the given per-tile output
   pixel size. Used in tile mode so each tile shows the entire image
   rasterized sharply at the current per-tile scale, then SDL just blits
   that single texture N times across the grid. The cap behavior matches
   svg_plan_for_view: above SVG_MAX_RASTER_DIM, fall back to GPU scaling. */
static SvgPlan svg_plan_for_tile(const Viewer *viewer,
                                 double per_tile_w, double per_tile_h) {
    const SvgImage *svg = &viewer->src.v.svg;

    /* Cap to the maximum raster dim; above that we GPU-upscale. */
    double out_w_d = per_tile_w;
    double out_h_d = per_tile_h;
    double max_out = (double)SVG_MAX_RASTER_DIM;
    if (out_w_d > max_out) out_w_d = max_out;
    if (out_h_d > max_out) out_h_d = max_out;

    /* Choose a scale such that out_w_d / scale == natural_w_f, i.e.
       scale == out_w_d / natural_w_f, but compute it from whichever
       dimension is more constrained so we stay within the cap on both. */
    double sx = (svg->natural_w_f > 0.0) ? out_w_d / svg->natural_w_f : 1.0;
    double sy = (svg->natural_h_f > 0.0) ? out_h_d / svg->natural_h_f : 1.0;
    double scale = sx < sy ? sx : sy;
    if (scale <= 0.0 || !isfinite(scale)) scale = 1.0;

    int out_w = (int)floor(svg->natural_w_f * scale + 0.5);
    int out_h = (int)floor(svg->natural_h_f * scale + 0.5);
    if (out_w < 1) out_w = 1;
    if (out_h < 1) out_h = 1;
    if (out_w > SVG_MAX_RASTER_DIM) out_w = SVG_MAX_RASTER_DIM;
    if (out_h > SVG_MAX_RASTER_DIM) out_h = SVG_MAX_RASTER_DIM;

    SvgPlan plan;
    plan.ix = 0.0f;
    plan.iy = 0.0f;
    plan.scale = (float)scale;
    plan.out_w = out_w;
    plan.out_h = out_h;
    return plan;
}

/* Compute the per-tile on-screen size for SVG layout. Same math as the
   non-SVG renderer uses for draw_w/draw_h: fit to window aspect, swap
   axes for quarter rotation, multiply by zoom. */
static void svg_compute_tile_size(const Viewer *viewer,
                                  int window_w, int window_h,
                                  double *out_w, double *out_h) {
    int eff_w = viewer->src.width;
    int eff_h = viewer->src.height;
    bool quarter = (viewer->rotation_steps == 1 || viewer->rotation_steps == 3);
    if (quarter) { int tmp = eff_w; eff_w = eff_h; eff_h = tmp; }
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
    double zoom_v = viewer->zoom;
    if (zoom_v <= 0.0 || !isfinite(zoom_v)) zoom_v = 1.0;
    double tw = fit_w * zoom_v;
    double th = fit_h * zoom_v;
    if (tw < 1.0) tw = 1.0;
    if (th < 1.0) th = 1.0;
    *out_w = tw;
    *out_h = th;
}

/* Pick the plan kind that best matches the current viewer state. */
static SvgPlan svg_pick_plan(const Viewer *viewer,
                             int window_w, int window_h) {
    bool tiled = (viewer->tile_on && viewer->tile_radius > 0);
    bool transformed = (viewer->rotation_steps != 0 ||
                        viewer->flip_h || viewer->flip_v);
    if (tiled || transformed) {
        double tw, th;
        svg_compute_tile_size(viewer, window_w, window_h, &tw, &th);
        bool quarter = (viewer->rotation_steps == 1 || viewer->rotation_steps == 3);
        double per_w = quarter ? th : tw;
        double per_h = quarter ? tw : th;
        return svg_plan_for_tile(viewer, per_w, per_h);
    }
    return svg_plan_for_view(viewer, window_w, window_h);
}

/* Decide whether the requested plan differs enough from what's currently
   queued/showing to justify a new raster. Even a small zoom change can
   produce a different out_w/h; require some change to avoid thrashing
   when nothing meaningful happened (e.g., redraw on EXPOSED). */
static bool svg_plan_meaningfully_different(const SvgPlan *a, const SvgPlan *b) {
    if (a->out_w != b->out_w || a->out_h != b->out_h) return true;
    /* compare floats with a small tolerance */
    float ds = fabsf(a->scale - b->scale) / fmaxf(a->scale, 1e-6f);
    float dix = fabsf(a->ix - b->ix);
    float diy = fabsf(a->iy - b->iy);
    if (ds > 0.001f) return true;
    if (dix > 0.5f || diy > 0.5f) return true;
    return false;
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
    {"r",         "Reset zoom, pan, flips, rotation, window size"},
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
    {", / .",     "Previous / next image in directory"},
    {"Home / End", "First / last image in directory"},
    {"Wheel",     "Zoom in/out"},
    {"Drag",      "Pan"},
    {"q / Esc",   "Quit"},
};
static const int BINDS_COUNT = (int)(sizeof(BINDS) / sizeof(BINDS[0]));

static void render(Viewer *viewer) {
    int window_w = 1, window_h = 1;
    SDL_GetCurrentRenderOutputSize(viewer->renderer, &window_w, &window_h);

    Uint8 br = viewer->opts ? viewer->opts->bg_r : 0;
    Uint8 bg = viewer->opts ? viewer->opts->bg_g : 0;
    Uint8 bb = viewer->opts ? viewer->opts->bg_b : 0;
    SDL_SetRenderDrawColor(viewer->renderer, br, bg, bb, 255);
    SDL_RenderClear(viewer->renderer);

    if (viewer->src.kind == IMG_SVG) {
        SvgImage *svg = &viewer->src.v.svg;

        /* Common flip/rotation state. Rotation swaps the on-screen
           footprint for 90/270 degrees. Same conventions as the non-SVG
           path so behavior is consistent. */
        SDL_FlipMode base_flip =
            (viewer->flip_h && viewer->flip_v) ? SDL_FLIP_HORIZONTAL_AND_VERTICAL :
            viewer->flip_h ? SDL_FLIP_HORIZONTAL :
            viewer->flip_v ? SDL_FLIP_VERTICAL :
            SDL_FLIP_NONE;
        double angle = (double)viewer->rotation_steps * 90.0;
        bool quarter = (viewer->rotation_steps == 1 || viewer->rotation_steps == 3);

        /* Drag pans in window-unit screen coordinates. Convert to pixel
           units for compositing. */
        double w2p_sx, w2p_sy;
        window_to_pixel_scale(viewer, &w2p_sx, &w2p_sy);
        double pan_px_x = viewer->pan_off_x * w2p_sx;
        double pan_px_y = viewer->pan_off_y * w2p_sy;

        /* Pick up any completed worker raster. */
        SvgPlan adopted;
        if (svg_try_consume(svg, &adopted)) {
            if (adopted.out_w > 0 && adopted.out_h > 0) {
                if (ensure_texture(viewer, adopted.out_w, adopted.out_h)) {
                    SDL_UpdateTexture(viewer->texture, NULL,
                                      svg->shown, adopted.out_w * 4);
                }
            }
        }

        /* In tile mode we want each tile to be the whole SVG, rasterized
           sharp at per-tile pixel size. In single-image mode we normally
           keep the "render only the visible portion" optimization — but
           when the image is rotated or flipped, sub-region geometry on a
           rotated quad gets fiddly, so we switch to whole-image raster
           there too. Slightly more raster work, much simpler math. */
        bool tiled = (viewer->tile_on && viewer->tile_radius > 0);
        bool transformed = (viewer->rotation_steps != 0 ||
                            viewer->flip_h || viewer->flip_v);
        bool whole_image_mode = tiled || transformed;

        /* Per-tile on-screen size, derived identically to the non-SVG
           draw_w/draw_h math. */
        double tile_w, tile_h;
        svg_compute_tile_size(viewer, window_w, window_h, &tile_w, &tile_h);

        /* Build the desired plan and decide whether to request a re-raster. */
        SvgPlan want = svg_pick_plan(viewer, window_w, window_h);

        /* If we don't have any raster yet, force a synchronous request and
           wait briefly so the first frame isn't blank. */
        if (!viewer->texture) {
            svg_request(svg, &want);
            for (int spin = 0; spin < 100; spin++) {
                SDL_Delay(2);
                if (svg_try_consume(svg, &adopted)) {
                    if (adopted.out_w > 0 && adopted.out_h > 0 &&
                        ensure_texture(viewer, adopted.out_w, adopted.out_h)) {
                        SDL_UpdateTexture(viewer->texture, NULL,
                                          svg->shown, adopted.out_w * 4);
                    }
                    break;
                }
            }
        } else if (svg_plan_meaningfully_different(&want, &svg->last_requested_plan)) {
            svg->dirty = true;
            svg->dirty_since_ns = SDL_GetTicksNS();
        }

        /* Helper: produce the dst rect for one tile at grid (gx, gy)
           centered on window center, with anchor + pan applied.
           Identical layout math to the non-SVG path. */
        double center_x = (double)window_w * 0.5;
        double center_y = (double)window_h * 0.5;

        int radius = tiled ? viewer->tile_radius : 0;

        for (int gy = -radius; gy <= radius; gy++) {
            for (int gx = -radius; gx <= radius; gx++) {
                /* Per-tile flip if mirrored tiling is on. */
                SDL_FlipMode flip = base_flip;
                if (tiled && viewer->tile_mirror) {
                    bool fh = (gx & 1) != 0;
                    bool fv = (gy & 1) != 0;
                    bool h = ((flip & SDL_FLIP_HORIZONTAL) != 0) ^ fh;
                    bool v = ((flip & SDL_FLIP_VERTICAL)   != 0) ^ fv;
                    flip = (h && v) ? SDL_FLIP_HORIZONTAL_AND_VERTICAL :
                           h ? SDL_FLIP_HORIZONTAL :
                           v ? SDL_FLIP_VERTICAL :
                           SDL_FLIP_NONE;
                }

                double tile_cx = center_x - viewer->anchor_x * tile_w + tile_w * 0.5
                               + (double)gx * tile_w + pan_px_x;
                double tile_cy = center_y - viewer->anchor_y * tile_h + tile_h * 0.5
                               + (double)gy * tile_h + pan_px_y;

                /* (1) Backdrop. For SDL_RenderTextureRotated, the dst rect
                   must use the pre-rotation footprint shape; quarter
                   rotations swap dims. */
                if (svg->backdrop_tex) {
                    SDL_FRect bdst;
                    if (quarter) {
                        bdst.w = (float)tile_h;
                        bdst.h = (float)tile_w;
                    } else {
                        bdst.w = (float)tile_w;
                        bdst.h = (float)tile_h;
                    }
                    bdst.x = (float)(tile_cx - (double)bdst.w * 0.5);
                    bdst.y = (float)(tile_cy - (double)bdst.h * 0.5);
                    if (angle != 0.0 || flip != SDL_FLIP_NONE) {
                        SDL_RenderTextureRotated(viewer->renderer,
                                                 svg->backdrop_tex,
                                                 NULL, &bdst,
                                                 angle, NULL, flip);
                    } else {
                        SDL_RenderTexture(viewer->renderer,
                                          svg->backdrop_tex, NULL, &bdst);
                    }
                }

                /* (2) Sharp clipped raster. When we're in whole-image
                   mode (tile or rotated/flipped), the shown buffer
                   represents the WHOLE SVG and we draw it filling each
                   tile's footprint. Otherwise it represents a sub-region
                   and we map it inside the tile rect. */
                if (viewer->texture && svg->shown_plan.scale > 0.0f) {
                    if (whole_image_mode) {
                        SDL_FRect dst;
                        if (quarter) {
                            dst.w = (float)tile_h;
                            dst.h = (float)tile_w;
                        } else {
                            dst.w = (float)tile_w;
                            dst.h = (float)tile_h;
                        }
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
                    } else {
                        /* Untransformed single-image SVG: shown buffer is
                           a sub-region in image space. Map it inside the
                           (unrotated) tile rect. No rotation/flip in this
                           branch — those route through whole-image mode. */
                        double ppu_x = tile_w / svg->natural_w_f;
                        double ppu_y = tile_h / svg->natural_h_f;
                        double sw = (double)svg->shown_plan.out_w /
                                    (double)svg->shown_plan.scale;
                        double sh = (double)svg->shown_plan.out_h /
                                    (double)svg->shown_plan.scale;
                        double sx0 = (double)svg->shown_plan.ix;
                        double sy0 = (double)svg->shown_plan.iy;

                        SDL_FRect dst;
                        dst.x = (float)(tile_cx - tile_w * 0.5 + sx0 * ppu_x);
                        dst.y = (float)(tile_cy - tile_h * 0.5 + sy0 * ppu_y);
                        dst.w = (float)(sw * ppu_x);
                        dst.h = (float)(sh * ppu_y);
                        SDL_RenderTexture(viewer->renderer,
                                          viewer->texture, NULL, &dst);
                    }
                }
            }
        }
    } else {
        /* Non-SVG path: classic full-image bilinear scale, with optional
           flip / rotation / tile. Rotation by 90 or 270 degrees swaps
           the on-screen aspect; we honor that so the rotated image fits
           the same way the natural image would. */
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

/* If an SVG is dirty and the debounce window has elapsed since the last
   dirty event, kick off a new raster job. Returns true if a job was
   requested. */
static bool maybe_request_svg_raster(Viewer *viewer) {
    if (viewer->src.kind != IMG_SVG) return false;
    SvgImage *svg = &viewer->src.v.svg;
    if (!svg->dirty) return false;
    Uint64 now = SDL_GetTicksNS();
    if (now - svg->dirty_since_ns < SVG_DEBOUNCE_NS) return false;

    int window_w = 1, window_h = 1;
    SDL_GetCurrentRenderOutputSize(viewer->renderer, &window_w, &window_h);
    SvgPlan want = svg_pick_plan(viewer, window_w, window_h);
    svg_request(svg, &want);
    svg->dirty = false;
    return true;
}

/* Time (ms) until the next thing the event loop should wake up for:
   - next GIF frame
   - SVG debounce expiry (if dirty)
   - SVG worker job completion poll (if a job is in flight; this is just a
     short ceiling so we don't sleep through a completion under low load)
   Returns -1 if there's nothing to wait for. */
static Sint32 time_until_next_wake_ms(const Viewer *viewer) {
    Sint32 best = -1;

    Sint32 anim_ms = time_until_next_anim_frame_ms(viewer);
    if (anim_ms >= 0 && (best < 0 || anim_ms < best)) best = anim_ms;

    if (viewer->src.kind == IMG_SVG) {
        const SvgImage *svg = &viewer->src.v.svg;
        if (svg->dirty) {
            Uint64 now = SDL_GetTicksNS();
            Uint64 due = svg->dirty_since_ns + SVG_DEBOUNCE_NS;
            Sint32 ms = (due > now)
                ? (Sint32)((due - now + 999999) / 1000000)
                : 0;
            if (best < 0 || ms < best) best = ms;
        }
        /* If a worker job is in flight or completed, poll every ~16ms so
           we redraw shortly after it finishes. */
        SDL_LockMutex(svg->mu);
        bool in_flight = svg->job_pending || svg->job_done;
        SDL_UnlockMutex(svg->mu);
        if (in_flight) {
            if (best < 0 || best > 16) best = 16;
        }
    }
    return best;
}

static void event_loop(Viewer *viewer) {
    bool running = true;
    render(viewer);

    while (running) {
        bool redraw = false;

        /* Advance any due GIF frames first so the timeout below is honest. */
        maybe_advance_anim(viewer, &redraw);

        /* Kick off any due SVG raster job. */
        if (maybe_request_svg_raster(viewer)) {
            /* Don't redraw here; render() will pick up the result via
               svg_try_consume on the next pass. */
        }

        /* Check for a freshly completed SVG raster regardless of events. */
        if (viewer->src.kind == IMG_SVG) {
            SDL_LockMutex(viewer->src.v.svg.mu);
            bool ready = viewer->src.v.svg.job_done;
            SDL_UnlockMutex(viewer->src.v.svg.mu);
            if (ready) redraw = true;
        }

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

                case SDL_EVENT_DROP_FILE:
                    if (event.drop.data) {
                        /* event.drop.data is owned by SDL and only valid
                           during the event. open_path() handles both
                           single files (opens directly) and directories
                           (populates the nav list and opens the first). */
                        if (open_path(viewer, event.drop.data)) {
                            redraw = true;
                        } else {
                            fprintf(stderr,
                                    "%s: failed to open dropped path '%s'\n",
                                    APP_NAME, event.drop.data);
                        }
                    }
                    break;

                default:
                    break;
                }
            } while (running && SDL_PollEvent(&event));
        }

        if (redraw && running) render(viewer);
    }
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

    /* If a path was given on the command line, load it eagerly (so we
       size the initial window to the image). For a directory, we'll
       defer to open_path() after the window exists, since that path
       populates the nav list and only opens the first image. */
    bool is_dir_arg = false;
    if (opts.image_path) {
        if (is_directory(opts.image_path)) {
            is_dir_arg = true;
            /* Don't load via load_source yet; need the window & dirlist. */
        } else if (!load_source(opts.image_path, &viewer.src)) {
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
        : (opts.image_path ? opts.image_path : APP_TITLE);
    if (!create_sdl(&viewer, window_title)) {
        destroy_viewer(&viewer);
        SDL_Quit();
        gegl_exit();
        return 1;
    }

    /* Populate current_path / current_size_bytes for the eagerly-loaded
       file case (the directory case goes through open_path below, which
       sets these via reopen_image). */
    if (opts.image_path && !is_dir_arg && viewer.src.width > 0) {
        viewer.current_path = strdup(opts.image_path);
        struct stat st;
        viewer.current_size_bytes =
            (stat(opts.image_path, &st) == 0) ? (size_t)st.st_size : 0;
        rebuild_info_lines(&viewer);
    }

    /* If launched with a directory, do that now (window exists, so any
       failures can be reported but the window stays up). */
    if (is_dir_arg) {
        if (!open_path(&viewer, opts.image_path)) {
            fprintf(stderr,
                    "%s: directory '%s' had no openable images; "
                    "starting blank — drop a file to begin\n",
                    APP_NAME, opts.image_path);
        }
    }

    event_loop(&viewer);

    destroy_viewer(&viewer);
    SDL_Quit();
    gegl_exit();
    return 0;
}
