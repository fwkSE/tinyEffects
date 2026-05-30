#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#define MAX_BANDS 64
#define MIN_BANDS 4
#define DEFAULT_BANDS 24
#define DEFAULT_RATE 11025U
#define DEFAULT_FPS 15
#define DEFAULT_FRAMES 512
#define MAX_BAR_COLS 120
#define MAX_BAR_ROWS 40
#define GLYPH_HEIGHT 7
#define GLYPH_WIDTH 5
#define SPACE_WIDTH 3
#define OUTPUT_BUFFER_SIZE (256 * 1024)
#define MAX_PALETTE_STOPS 8
#define PI 3.14159265358979323846

#define SND_PCM_STREAM_CAPTURE 1
#define SND_PCM_NONBLOCK 1
#define SND_PCM_ACCESS_RW_INTERLEAVED 3
#define SND_PCM_FORMAT_S16_LE 2

typedef struct _snd_pcm snd_pcm_t;
typedef struct _snd_pcm_hw_params snd_pcm_hw_params_t;
typedef long snd_pcm_sframes_t;
typedef unsigned long snd_pcm_uframes_t;

typedef struct {
    void *handle;
    int (*pcm_open)(snd_pcm_t **, const char *, int, int);
    int (*pcm_close)(snd_pcm_t *);
    int (*hw_params_malloc)(snd_pcm_hw_params_t **);
    void (*hw_params_free)(snd_pcm_hw_params_t *);
    int (*hw_params_any)(snd_pcm_t *, snd_pcm_hw_params_t *);
    int (*hw_params_set_access)(snd_pcm_t *, snd_pcm_hw_params_t *, int);
    int (*hw_params_set_format)(snd_pcm_t *, snd_pcm_hw_params_t *, int);
    int (*hw_params_set_channels)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned int);
    int (*hw_params_set_rate_near)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned int *, int *);
    int (*hw_params_set_period_size_near)(snd_pcm_t *, snd_pcm_hw_params_t *, snd_pcm_uframes_t *, int *);
    int (*hw_params_set_buffer_size_near)(snd_pcm_t *, snd_pcm_hw_params_t *, snd_pcm_uframes_t *);
    int (*hw_params)(snd_pcm_t *, snd_pcm_hw_params_t *);
    int (*pcm_prepare)(snd_pcm_t *);
    snd_pcm_sframes_t (*pcm_readi)(snd_pcm_t *, void *, snd_pcm_uframes_t);
    int (*pcm_resume)(snd_pcm_t *);
    int (*pcm_wait)(snd_pcm_t *, int);
    const char *(*strerror_fn)(int);
} AlsaApi;

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} Rgb;

typedef struct {
    const char *name;
    int count;
    Rgb stops[MAX_PALETTE_STOPS];
} Palette;

typedef struct {
    const char *device;
    const char *text;
    unsigned int rate;
    int fps;
    int bands;
    int demo;
    int terminal;
    int fullscreen;
    Palette palette;
    snd_pcm_uframes_t frames;
} Config;

typedef struct {
    double coeff[MAX_BANDS];
    double level[MAX_BANDS];
    double peak[MAX_BANDS];
    int bands;
} Analyzer;

typedef struct {
    int rows;
    int cols;
} TermSize;

typedef struct {
    Display *display;
    int screen;
    Window window;
    GC gc;
    Atom wm_delete;
    Atom net_wm_state;
    Atom net_wm_state_fullscreen;
    XImage *image;
    uint32_t *pixels;
    int width;
    int height;
    int fullscreen;
} Graphics;

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_resized = 1;
static struct termios g_old_termios;
static int g_have_termios = 0;
static int g_old_stdin_flags = -1;
static AlsaApi g_alsa;
static char g_stdout_buffer[OUTPUT_BUFFER_SIZE];

static const Palette PALETTE_CLASSIC = {
    "classic", 6,
    {{50, 180, 255}, {50, 220, 255}, {80, 255, 110},
     {255, 235, 70}, {255, 140, 40}, {255, 50, 45}}
};
static const Palette PALETTE_FIRE = {
    "fire", 5,
    {{30, 0, 0}, {130, 10, 0}, {230, 55, 0}, {255, 190, 30}, {255, 255, 210}}
};
static const Palette PALETTE_ICE = {
    "ice", 5,
    {{0, 12, 45}, {0, 65, 135}, {0, 180, 235}, {160, 240, 255}, {255, 255, 255}}
};
static const Palette PALETTE_MATRIX = {
    "matrix", 4,
    {{0, 18, 0}, {0, 95, 20}, {40, 210, 65}, {190, 255, 190}}
};
static const Palette PALETTE_MONO = {
    "mono", 3,
    {{20, 20, 20}, {150, 150, 150}, {255, 255, 255}}
};
static const Palette PALETTE_RAINBOW = {
    "rainbow", 6,
    {{90, 60, 255}, {0, 190, 255}, {30, 240, 80},
     {255, 230, 40}, {255, 120, 20}, {255, 45, 120}}
};
static const Palette PALETTE_NEON = {
    "neon", 6,
    {{5, 5, 18}, {0, 200, 255}, {235, 245, 255},
     {255, 90, 230}, {255, 0, 125}, {120, 40, 255}}
};

static void unload_alsa(void);

static void *load_symbol(void *handle, const char *name) {
    void *symbol = dlsym(handle, name);

    if (!symbol) {
        fprintf(stderr, "Cannot load ALSA symbol %s: %s\n", name, dlerror());
    }
    return symbol;
}

static int load_alsa(void) {
    g_alsa.handle = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!g_alsa.handle) {
        fprintf(stderr, "Cannot load libasound.so.2: %s\n", dlerror());
        return -1;
    }

    g_alsa.pcm_open = load_symbol(g_alsa.handle, "snd_pcm_open");
    g_alsa.pcm_close = load_symbol(g_alsa.handle, "snd_pcm_close");
    g_alsa.hw_params_malloc = load_symbol(g_alsa.handle, "snd_pcm_hw_params_malloc");
    g_alsa.hw_params_free = load_symbol(g_alsa.handle, "snd_pcm_hw_params_free");
    g_alsa.hw_params_any = load_symbol(g_alsa.handle, "snd_pcm_hw_params_any");
    g_alsa.hw_params_set_access = load_symbol(g_alsa.handle, "snd_pcm_hw_params_set_access");
    g_alsa.hw_params_set_format = load_symbol(g_alsa.handle, "snd_pcm_hw_params_set_format");
    g_alsa.hw_params_set_channels = load_symbol(g_alsa.handle, "snd_pcm_hw_params_set_channels");
    g_alsa.hw_params_set_rate_near = load_symbol(g_alsa.handle, "snd_pcm_hw_params_set_rate_near");
    g_alsa.hw_params_set_period_size_near = load_symbol(g_alsa.handle, "snd_pcm_hw_params_set_period_size_near");
    g_alsa.hw_params_set_buffer_size_near = load_symbol(g_alsa.handle, "snd_pcm_hw_params_set_buffer_size_near");
    g_alsa.hw_params = load_symbol(g_alsa.handle, "snd_pcm_hw_params");
    g_alsa.pcm_prepare = load_symbol(g_alsa.handle, "snd_pcm_prepare");
    g_alsa.pcm_readi = load_symbol(g_alsa.handle, "snd_pcm_readi");
    g_alsa.pcm_resume = load_symbol(g_alsa.handle, "snd_pcm_resume");
    g_alsa.pcm_wait = load_symbol(g_alsa.handle, "snd_pcm_wait");
    g_alsa.strerror_fn = load_symbol(g_alsa.handle, "snd_strerror");

    if (!g_alsa.pcm_open || !g_alsa.pcm_close || !g_alsa.hw_params_malloc ||
        !g_alsa.hw_params_free || !g_alsa.hw_params_any ||
        !g_alsa.hw_params_set_access || !g_alsa.hw_params_set_format ||
        !g_alsa.hw_params_set_channels || !g_alsa.hw_params_set_rate_near ||
        !g_alsa.hw_params_set_period_size_near ||
        !g_alsa.hw_params_set_buffer_size_near || !g_alsa.hw_params ||
        !g_alsa.pcm_prepare || !g_alsa.pcm_readi || !g_alsa.pcm_resume ||
        !g_alsa.pcm_wait || !g_alsa.strerror_fn) {
        unload_alsa();
        return -1;
    }

    return 0;
}

static void unload_alsa(void) {
    if (g_alsa.handle) {
        dlclose(g_alsa.handle);
        g_alsa.handle = NULL;
    }
}

static const char *alsa_error(int err) {
    return g_alsa.strerror_fn ? g_alsa.strerror_fn(err) : "unknown ALSA error";
}

static void handle_signal(int sig) {
    if (sig == SIGWINCH) {
        g_resized = 1;
        return;
    }
    g_running = 0;
}

static long parse_long(const char *text, long min, long max, const char *name) {
    char *end = NULL;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno || end == text || *end != '\0' || value < min || value > max) {
        fprintf(stderr, "Invalid %s: %s (expected %ld..%ld)\n", name, text, min, max);
        exit(2);
    }
    return value;
}

static int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static int parse_hex_color(const char *text, Rgb *out) {
    int r1;
    int r2;
    int g1;
    int g2;
    int b1;
    int b2;

    if (strlen(text) != 7 || text[0] != '#') {
        return -1;
    }

    r1 = hex_value(text[1]);
    r2 = hex_value(text[2]);
    g1 = hex_value(text[3]);
    g2 = hex_value(text[4]);
    b1 = hex_value(text[5]);
    b2 = hex_value(text[6]);
    if (r1 < 0 || r2 < 0 || g1 < 0 || g2 < 0 || b1 < 0 || b2 < 0) {
        return -1;
    }

    out->r = (uint8_t)(r1 * 16 + r2);
    out->g = (uint8_t)(g1 * 16 + g2);
    out->b = (uint8_t)(b1 * 16 + b2);
    return 0;
}

static void set_palette_by_name(Config *cfg, const char *name) {
    const Palette *preset = NULL;

    if (strcmp(name, "classic") == 0) {
        preset = &PALETTE_CLASSIC;
    } else if (strcmp(name, "fire") == 0) {
        preset = &PALETTE_FIRE;
    } else if (strcmp(name, "ice") == 0) {
        preset = &PALETTE_ICE;
    } else if (strcmp(name, "matrix") == 0) {
        preset = &PALETTE_MATRIX;
    } else if (strcmp(name, "mono") == 0) {
        preset = &PALETTE_MONO;
    } else if (strcmp(name, "rainbow") == 0) {
        preset = &PALETTE_RAINBOW;
    } else if (strcmp(name, "neon") == 0 || strcmp(name, "soundsvall") == 0) {
        preset = &PALETTE_NEON;
    }

    if (!preset) {
        fprintf(stderr, "Unknown palette '%s' (expected classic, fire, ice, matrix, mono, rainbow, neon)\n", name);
        exit(2);
    }

    cfg->palette = *preset;
}

static void set_custom_palette(Config *cfg, const char *colors) {
    const char *start = colors;
    int count = 0;

    memset(&cfg->palette, 0, sizeof(cfg->palette));
    cfg->palette.name = "custom";

    while (*start) {
        char token[8];
        const char *comma = strchr(start, ',');
        size_t len = comma ? (size_t)(comma - start) : strlen(start);

        if (len >= sizeof(token) || count >= MAX_PALETTE_STOPS) {
            fprintf(stderr, "Invalid --colors value. Use 2-%d comma-separated #RRGGBB colors.\n", MAX_PALETTE_STOPS);
            exit(2);
        }

        memcpy(token, start, len);
        token[len] = '\0';
        if (parse_hex_color(token, &cfg->palette.stops[count]) < 0) {
            fprintf(stderr, "Invalid color '%s'. Use #RRGGBB.\n", token);
            exit(2);
        }
        count++;

        if (!comma) {
            break;
        }
        start = comma + 1;
    }

    if (count < 2) {
        fprintf(stderr, "--colors needs at least two #RRGGBB colors.\n");
        exit(2);
    }
    cfg->palette.count = count;
}

static void print_usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s [options]\n"
            "\n"
            "Options:\n"
            "  -d DEVICE  ALSA capture device (default: default)\n"
            "  -t TEXT    Render this exact text once with EQ color pulsing\n"
            "  -r RATE    Sample rate, Hz (default: %u)\n"
            "  -f FPS     Max redraws per second, 1..60 (default: %d)\n"
            "  -b BANDS   EQ bands, %d..%d (default: %d)\n"
            "  -m         Demo mode: animate without ALSA or music\n"
            "  -T         Terminal mode instead of graphical X11 mode\n"
            "  -F         Start graphical mode fullscreen\n"
            "  -p NAME    Palette: classic, fire, ice, matrix, mono, rainbow, neon\n"
            "  -C COLORS  Custom #RRGGBB colors, comma-separated\n"
            "  -h         Show this help\n"
            "\n"
            "Examples:\n"
            "  %s -m -t tinyEffects\n"
            "  %s -F -m -t tinyEffects --palette fire\n"
            "  %s -T -d hw:Loopback,1,0 -t \"Now playing\"\n",
            argv0, DEFAULT_RATE, DEFAULT_FPS, MIN_BANDS, MAX_BANDS,
            DEFAULT_BANDS, argv0, argv0, argv0);
}

static Config parse_args(int argc, char **argv) {
    Config cfg;
    int opt;
    static const struct option long_options[] = {
        {"device", required_argument, NULL, 'd'},
        {"text", required_argument, NULL, 't'},
        {"rate", required_argument, NULL, 'r'},
        {"fps", required_argument, NULL, 'f'},
        {"bands", required_argument, NULL, 'b'},
        {"demo", no_argument, NULL, 'm'},
        {"terminal", no_argument, NULL, 'T'},
        {"fullscreen", no_argument, NULL, 'F'},
        {"palette", required_argument, NULL, 'p'},
        {"colors", required_argument, NULL, 'C'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    cfg.device = "default";
    cfg.text = NULL;
    cfg.rate = DEFAULT_RATE;
    cfg.fps = DEFAULT_FPS;
    cfg.bands = DEFAULT_BANDS;
    cfg.demo = 0;
    cfg.terminal = 0;
    cfg.fullscreen = 0;
    cfg.palette = PALETTE_CLASSIC;
    cfg.frames = DEFAULT_FRAMES;

    while ((opt = getopt_long(argc, argv, "d:t:r:f:b:mTFp:C:h", long_options, NULL)) != -1) {
        switch (opt) {
        case 'd':
            cfg.device = optarg;
            break;
        case 't':
            cfg.text = optarg;
            break;
        case 'r':
            cfg.rate = (unsigned int)parse_long(optarg, 8000, 48000, "rate");
            break;
        case 'f':
            cfg.fps = (int)parse_long(optarg, 1, 60, "fps");
            break;
        case 'b':
            cfg.bands = (int)parse_long(optarg, MIN_BANDS, MAX_BANDS, "bands");
            break;
        case 'm':
            cfg.demo = 1;
            break;
        case 'T':
            cfg.terminal = 1;
            break;
        case 'F':
            cfg.fullscreen = 1;
            break;
        case 'p':
            set_palette_by_name(&cfg, optarg);
            break;
        case 'C':
            set_custom_palette(&cfg, optarg);
            break;
        case 'h':
            print_usage(argv[0]);
            exit(0);
        default:
            print_usage(argv[0]);
            exit(2);
        }
    }

    if (optind < argc && cfg.text == NULL) {
        cfg.text = argv[optind];
    }

    return cfg;
}

static void restore_terminal(void) {
    if (g_have_termios) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_old_termios);
    }
    if (g_old_stdin_flags >= 0) {
        fcntl(STDIN_FILENO, F_SETFL, g_old_stdin_flags);
    }
    fputs("\033[0m\033[?7h\033[?25h\033[?1049l", stdout);
    fflush(stdout);
}

static void setup_terminal(void) {
    struct termios raw;

    if (tcgetattr(STDIN_FILENO, &g_old_termios) == 0) {
        g_have_termios = 1;
        raw = g_old_termios;
        raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }

    g_old_stdin_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (g_old_stdin_flags >= 0) {
        fcntl(STDIN_FILENO, F_SETFL, g_old_stdin_flags | O_NONBLOCK);
    }

    atexit(restore_terminal);
    fputs("\033[?1049h\033[?25l\033[?7l\033[2J\033[H", stdout);
    fflush(stdout);
}

static TermSize read_term_size(void) {
    struct winsize ws;
    TermSize size;

    size.rows = 24;
    size.cols = 80;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_row > 0) {
            size.rows = ws.ws_row;
        }
        if (ws.ws_col > 0) {
            size.cols = ws.ws_col;
        }
    }
    return size;
}

static int open_capture(Config *cfg, snd_pcm_t **pcm_out) {
    snd_pcm_t *pcm = NULL;
    snd_pcm_hw_params_t *params = NULL;
    unsigned int rate = cfg->rate;
    snd_pcm_uframes_t period = cfg->frames;
    snd_pcm_uframes_t buffer = cfg->frames * 4;
    int err;

    err = g_alsa.pcm_open(&pcm, cfg->device, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
    if (err < 0) {
        fprintf(stderr, "Cannot open ALSA capture device '%s': %s\n",
                cfg->device, alsa_error(err));
        return err;
    }

    if ((err = g_alsa.hw_params_malloc(&params)) < 0 ||
        (err = g_alsa.hw_params_any(pcm, params)) < 0 ||
        (err = g_alsa.hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
        (err = g_alsa.hw_params_set_format(pcm, params, SND_PCM_FORMAT_S16_LE)) < 0 ||
        (err = g_alsa.hw_params_set_channels(pcm, params, 1)) < 0 ||
        (err = g_alsa.hw_params_set_rate_near(pcm, params, &rate, NULL)) < 0 ||
        (err = g_alsa.hw_params_set_period_size_near(pcm, params, &period, NULL)) < 0 ||
        (err = g_alsa.hw_params_set_buffer_size_near(pcm, params, &buffer)) < 0 ||
        (err = g_alsa.hw_params(pcm, params)) < 0) {
        fprintf(stderr, "Cannot configure ALSA device '%s': %s\n",
                cfg->device, alsa_error(err));
        if (params) {
            g_alsa.hw_params_free(params);
        }
        g_alsa.pcm_close(pcm);
        return err;
    }
    g_alsa.hw_params_free(params);
    cfg->rate = rate;

    if ((err = g_alsa.pcm_prepare(pcm)) < 0) {
        fprintf(stderr, "Cannot prepare ALSA device '%s': %s\n",
                cfg->device, alsa_error(err));
        g_alsa.pcm_close(pcm);
        return err;
    }

    *pcm_out = pcm;
    return 0;
}

static void init_analyzer(Analyzer *analyzer, int bands, unsigned int rate) {
    double min_freq = 45.0;
    double max_freq = (double)rate * 0.45;
    double ratio;
    int i;

    if (max_freq > 5500.0) {
        max_freq = 5500.0;
    }
    if (max_freq <= min_freq) {
        max_freq = min_freq * 2.0;
    }

    memset(analyzer, 0, sizeof(*analyzer));
    analyzer->bands = bands;
    ratio = max_freq / min_freq;

    for (i = 0; i < bands; i++) {
        double pos = bands == 1 ? 0.0 : (double)i / (double)(bands - 1);
        double freq = min_freq * pow(ratio, pos);
        double omega = 2.0 * PI * freq / (double)rate;

        analyzer->coeff[i] = 2.0 * cos(omega);
        analyzer->peak[i] = 1.0e-9;
    }
}

static void analyze_samples(Analyzer *analyzer, const int16_t *samples, int count) {
    int band;

    for (band = 0; band < analyzer->bands; band++) {
        double q0 = 0.0;
        double q1 = 0.0;
        double q2 = 0.0;
        double power;
        double normalized;
        int i;

        for (i = 0; i < count; i++) {
            double sample = (double)samples[i] / 32768.0;

            q0 = analyzer->coeff[band] * q1 - q2 + sample;
            q2 = q1;
            q1 = q0;
        }

        power = q1 * q1 + q2 * q2 - analyzer->coeff[band] * q1 * q2;
        if (power < 0.0) {
            power = 0.0;
        }

        power = sqrt(power) / (double)count;
        analyzer->peak[band] *= 0.995;
        if (power > analyzer->peak[band]) {
            analyzer->peak[band] = power;
        }

        normalized = analyzer->peak[band] > 1.0e-9 ? power / analyzer->peak[band] : 0.0;
        if (normalized > 1.0) {
            normalized = 1.0;
        }

        if (normalized > analyzer->level[band]) {
            analyzer->level[band] = analyzer->level[band] * 0.55 + normalized * 0.45;
        } else {
            analyzer->level[band] = analyzer->level[band] * 0.84 + normalized * 0.16;
        }

        if (analyzer->level[band] < 0.01) {
            analyzer->level[band] = 0.0;
        }
    }
}

static void animate_demo(Analyzer *analyzer, long now_ms) {
    double t = (double)now_ms / 1000.0;
    double sweep_left = 0.5 + 0.5 * sin(t * 0.75);
    double sweep_right = 0.5 + 0.5 * sin(t * 1.15 + 2.2);
    int band;

    for (band = 0; band < analyzer->bands; band++) {
        double pos = analyzer->bands == 1 ? 0.0 : (double)band / (double)(analyzer->bands - 1);
        double bass = exp(-80.0 * (pos - 0.12) * (pos - 0.12)) *
                      (0.35 + 0.35 * (0.5 + 0.5 * sin(t * 4.0)));
        double sweep_a = exp(-9.0 * fabs(pos - sweep_left)) * 0.70;
        double sweep_b = exp(-11.0 * fabs(pos - sweep_right)) * 0.45;
        double shimmer = 0.16 * (0.5 + 0.5 * sin(t * 2.5 + pos * PI * 8.0));
        double target = bass + sweep_a + sweep_b + shimmer;

        if (target > 1.0) {
            target = 1.0;
        }

        if (target > analyzer->level[band]) {
            analyzer->level[band] = analyzer->level[band] * 0.55 + target * 0.45;
        } else {
            analyzer->level[band] = analyzer->level[band] * 0.86 + target * 0.14;
        }
    }
}

static double level_at_position(const Analyzer *analyzer, double pos) {
    double scaled;
    int left;
    int right;
    double mix;

    if (pos < 0.0) {
        pos = 0.0;
    } else if (pos > 1.0) {
        pos = 1.0;
    }

    scaled = pos * (double)(analyzer->bands - 1);
    left = (int)scaled;
    right = left + 1;
    if (right >= analyzer->bands) {
        right = analyzer->bands - 1;
    }
    mix = scaled - (double)left;

    return analyzer->level[left] * (1.0 - mix) + analyzer->level[right] * mix;
}

static void write_repeat(char ch, int count) {
    while (count-- > 0) {
        fputc(ch, stdout);
    }
}

static Rgb palette_base_color(const Palette *palette, double pos) {
    double scaled;
    int left;
    int right;
    double mix;
    Rgb out;

    if (pos < 0.0) {
        pos = 0.0;
    } else if (pos > 1.0) {
        pos = 1.0;
    }

    if (palette->count <= 1) {
        return palette->stops[0];
    }

    scaled = pos * (double)(palette->count - 1);
    left = (int)scaled;
    right = left + 1;
    if (right >= palette->count) {
        right = palette->count - 1;
    }
    mix = scaled - (double)left;

    out.r = (uint8_t)((double)palette->stops[left].r * (1.0 - mix) +
                      (double)palette->stops[right].r * mix);
    out.g = (uint8_t)((double)palette->stops[left].g * (1.0 - mix) +
                      (double)palette->stops[right].g * mix);
    out.b = (uint8_t)((double)palette->stops[left].b * (1.0 - mix) +
                      (double)palette->stops[right].b * mix);
    return out;
}

static int rgb_max_channel(Rgb color) {
    int max = color.r;

    if (color.g > max) {
        max = color.g;
    }
    if (color.b > max) {
        max = color.b;
    }
    return max;
}

static Rgb blend_rgb(Rgb a, Rgb b, double mix) {
    Rgb out;

    out.r = (uint8_t)((double)a.r * (1.0 - mix) + (double)b.r * mix);
    out.g = (uint8_t)((double)a.g * (1.0 - mix) + (double)b.g * mix);
    out.b = (uint8_t)((double)a.b * (1.0 - mix) + (double)b.b * mix);
    return out;
}

static Rgb visible_palette_base_color(const Palette *palette, double pos) {
    Rgb base = palette_base_color(palette, pos);
    int max = rgb_max_channel(base);
    int i;

    if (max >= 42 || palette->count < 2) {
        return base;
    }

    if (pos <= 0.5) {
        for (i = 1; i < palette->count; i++) {
            if (rgb_max_channel(palette->stops[i]) >= 42) {
                return blend_rgb(base, palette->stops[i], 0.55);
            }
        }
    } else {
        for (i = palette->count - 2; i >= 0; i--) {
            if (rgb_max_channel(palette->stops[i]) >= 42) {
                return blend_rgb(base, palette->stops[i], 0.55);
            }
        }
    }

    return (Rgb){80, 80, 80};
}

static Rgb palette_color(const Palette *palette, double pos, double level) {
    Rgb base = visible_palette_base_color(palette, pos);
    double mix = level;
    double dim = 0.18;
    double gain;
    Rgb out;

    if (mix < 0.0) {
        mix = 0.0;
    } else if (mix > 1.0) {
        mix = 1.0;
    }

    gain = dim + (1.0 - dim) * mix;
    out.r = (uint8_t)((double)base.r * gain);
    out.g = (uint8_t)((double)base.g * gain);
    out.b = (uint8_t)((double)base.b * gain);
    return out;
}

static uint32_t rgb_to_u32(Rgb color) {
    return (uint32_t)((color.r << 16) | (color.g << 8) | color.b);
}

static int color_distance_sq(Rgb color, int r, int g, int b) {
    int dr = (int)color.r - r;
    int dg = (int)color.g - g;
    int db = (int)color.b - b;

    return dr * dr + dg * dg + db * db;
}

static int rgb_to_xterm256(Rgb color) {
    static const int levels[] = {0, 95, 135, 175, 215, 255};
    int r_index = (int)((color.r < 48) ? 0 : (color.r < 115 ? 1 : (color.r - 35) / 40));
    int g_index = (int)((color.g < 48) ? 0 : (color.g < 115 ? 1 : (color.g - 35) / 40));
    int b_index = (int)((color.b < 48) ? 0 : (color.b < 115 ? 1 : (color.b - 35) / 40));
    int cube_index;
    int cube_dist;
    int gray_avg;
    int gray_index;
    int gray_value;
    int gray_dist;

    if (r_index > 5) {
        r_index = 5;
    }
    if (g_index > 5) {
        g_index = 5;
    }
    if (b_index > 5) {
        b_index = 5;
    }

    cube_index = 16 + 36 * r_index + 6 * g_index + b_index;
    cube_dist = color_distance_sq(color, levels[r_index], levels[g_index], levels[b_index]);

    gray_avg = ((int)color.r + (int)color.g + (int)color.b) / 3;
    gray_index = (gray_avg - 8 + 5) / 10;
    if (gray_index < 0) {
        gray_index = 0;
    } else if (gray_index > 23) {
        gray_index = 23;
    }
    gray_value = 8 + gray_index * 10;
    gray_dist = color_distance_sq(color, gray_value, gray_value, gray_value);

    return gray_dist < cube_dist ? 232 + gray_index : cube_index;
}

static int terminal_color_for(const Palette *palette, double pos, double level, int bright) {
    double effective_level = bright && level < 0.85 ? 0.85 : level;

    return rgb_to_xterm256(palette_color(palette, pos, effective_level));
}

static const char **glyph_for(unsigned char ch) {
    static const char *glyph_a[] = {" ### ", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"};
    static const char *glyph_b[] = {"#### ", "#   #", "#   #", "#### ", "#   #", "#   #", "#### "};
    static const char *glyph_c[] = {" ### ", "#   #", "#    ", "#    ", "#    ", "#   #", " ### "};
    static const char *glyph_d[] = {"#### ", "#   #", "#   #", "#   #", "#   #", "#   #", "#### "};
    static const char *glyph_e[] = {"#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#####"};
    static const char *glyph_f[] = {"#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#    "};
    static const char *glyph_g[] = {" ### ", "#   #", "#    ", "#  ##", "#   #", "#   #", " ### "};
    static const char *glyph_h[] = {"#   #", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"};
    static const char *glyph_i[] = {"#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "#####"};
    static const char *glyph_j[] = {"#####", "   # ", "   # ", "   # ", "   # ", "#  # ", " ##  "};
    static const char *glyph_k[] = {"#   #", "#  # ", "# #  ", "##   ", "# #  ", "#  # ", "#   #"};
    static const char *glyph_l[] = {"#    ", "#    ", "#    ", "#    ", "#    ", "#    ", "#####"};
    static const char *glyph_m[] = {"#   #", "## ##", "# # #", "#   #", "#   #", "#   #", "#   #"};
    static const char *glyph_n[] = {"#   #", "##  #", "# # #", "#  ##", "#   #", "#   #", "#   #"};
    static const char *glyph_o[] = {" ### ", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "};
    static const char *glyph_p[] = {"#### ", "#   #", "#   #", "#### ", "#    ", "#    ", "#    "};
    static const char *glyph_q[] = {" ### ", "#   #", "#   #", "#   #", "# # #", "#  # ", " ## #"};
    static const char *glyph_r[] = {"#### ", "#   #", "#   #", "#### ", "# #  ", "#  # ", "#   #"};
    static const char *glyph_s[] = {" ####", "#    ", "#    ", " ### ", "    #", "    #", "#### "};
    static const char *glyph_t[] = {"#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  "};
    static const char *glyph_u[] = {"#   #", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "};
    static const char *glyph_v[] = {"#   #", "#   #", "#   #", "#   #", "#   #", " # # ", "  #  "};
    static const char *glyph_w[] = {"#   #", "#   #", "#   #", "# # #", "# # #", "## ##", "#   #"};
    static const char *glyph_x[] = {"#   #", "#   #", " # # ", "  #  ", " # # ", "#   #", "#   #"};
    static const char *glyph_y[] = {"#   #", "#   #", " # # ", "  #  ", "  #  ", "  #  ", "  #  "};
    static const char *glyph_z[] = {"#####", "    #", "   # ", "  #  ", " #   ", "#    ", "#####"};
    static const char *glyph_0[] = {" ### ", "#   #", "#  ##", "# # #", "##  #", "#   #", " ### "};
    static const char *glyph_1[] = {"  #  ", " ##  ", "# #  ", "  #  ", "  #  ", "  #  ", "#####"};
    static const char *glyph_2[] = {" ### ", "#   #", "    #", "   # ", "  #  ", " #   ", "#####"};
    static const char *glyph_3[] = {"#### ", "    #", "    #", " ### ", "    #", "    #", "#### "};
    static const char *glyph_4[] = {"#   #", "#   #", "#   #", "#####", "    #", "    #", "    #"};
    static const char *glyph_5[] = {"#####", "#    ", "#    ", "#### ", "    #", "    #", "#### "};
    static const char *glyph_6[] = {" ### ", "#    ", "#    ", "#### ", "#   #", "#   #", " ### "};
    static const char *glyph_7[] = {"#####", "    #", "   # ", "  #  ", " #   ", " #   ", " #   "};
    static const char *glyph_8[] = {" ### ", "#   #", "#   #", " ### ", "#   #", "#   #", " ### "};
    static const char *glyph_9[] = {" ### ", "#   #", "#   #", " ####", "    #", "    #", " ### "};
    static const char *glyph_dot[] = {"     ", "     ", "     ", "     ", "     ", " ##  ", " ##  "};
    static const char *glyph_dash[] = {"     ", "     ", "     ", "#####", "     ", "     ", "     "};
    static const char *glyph_bang[] = {"  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "     ", "  #  "};
    static const char *glyph_qmark[] = {" ### ", "#   #", "    #", "   # ", "  #  ", "     ", "  #  "};
    static const char *glyph_colon[] = {"     ", " ##  ", " ##  ", "     ", " ##  ", " ##  ", "     "};
    static const char *glyph_unknown[] = {" ### ", "#   #", "    #", "   # ", "  #  ", "     ", "  #  "};

    switch (toupper(ch)) {
    case 'A': return glyph_a;
    case 'B': return glyph_b;
    case 'C': return glyph_c;
    case 'D': return glyph_d;
    case 'E': return glyph_e;
    case 'F': return glyph_f;
    case 'G': return glyph_g;
    case 'H': return glyph_h;
    case 'I': return glyph_i;
    case 'J': return glyph_j;
    case 'K': return glyph_k;
    case 'L': return glyph_l;
    case 'M': return glyph_m;
    case 'N': return glyph_n;
    case 'O': return glyph_o;
    case 'P': return glyph_p;
    case 'Q': return glyph_q;
    case 'R': return glyph_r;
    case 'S': return glyph_s;
    case 'T': return glyph_t;
    case 'U': return glyph_u;
    case 'V': return glyph_v;
    case 'W': return glyph_w;
    case 'X': return glyph_x;
    case 'Y': return glyph_y;
    case 'Z': return glyph_z;
    case '0': return glyph_0;
    case '1': return glyph_1;
    case '2': return glyph_2;
    case '3': return glyph_3;
    case '4': return glyph_4;
    case '5': return glyph_5;
    case '6': return glyph_6;
    case '7': return glyph_7;
    case '8': return glyph_8;
    case '9': return glyph_9;
    case '.':
    case ',': return glyph_dot;
    case '-':
    case '_': return glyph_dash;
    case '!': return glyph_bang;
    case '?': return glyph_qmark;
    case ':':
    case ';': return glyph_colon;
    default: return glyph_unknown;
    }
}

static int text_base_width(const char *text) {
    int width = 0;
    size_t i;
    size_t len = strlen(text);

    for (i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)text[i];

        width += ch == ' ' ? SPACE_WIDTH : GLYPH_WIDTH;
        if (i + 1 < len) {
            width += 1;
        }
    }
    return width > 0 ? width : 1;
}

static void draw_text_mode(const Config *cfg, const Analyzer *analyzer, TermSize size, int clear) {
    size_t len = strlen(cfg->text);
    int base_width;
    int scale;
    int scale_x;
    int scale_y;
    int rendered_width;
    int rendered_height;
    int row_start;
    int col_start;
    int gy;
    int sy;
    size_t i;

    if (len == 0) {
        return;
    }

    base_width = text_base_width(cfg->text);
    scale = size.cols / base_width;
    if (size.rows / GLYPH_HEIGHT < scale) {
        scale = size.rows / GLYPH_HEIGHT;
    }
    if (scale < 1) {
        scale = 1;
    }
    scale_x = scale;
    scale_y = scale;

    rendered_width = base_width * scale_x;
    rendered_height = GLYPH_HEIGHT * scale_y;
    row_start = size.rows > rendered_height ? (size.rows - rendered_height) / 2 + 1 : 1;
    col_start = size.cols > rendered_width ? (size.cols - rendered_width) / 2 + 1 : 1;

    if (clear) {
        fputs("\033[H\033[2J", stdout);
    }

    for (gy = 0; gy < GLYPH_HEIGHT; gy++) {
        for (sy = 0; sy < scale_y; sy++) {
            int row = row_start + gy * scale_y + sy;
            int col = col_start;

            if (row > size.rows) {
                break;
            }

            printf("\033[%d;%dH", row, col);
            for (i = 0; i < len; i++) {
                unsigned char ch = (unsigned char)cfg->text[i];
                int glyph_width;
                double pos = len == 1 ? 0.0 : (double)i / (double)(len - 1);
                double level = level_at_position(analyzer, pos);
                int bright = level > 0.72;
                int color = terminal_color_for(&cfg->palette, pos, level, bright);
                unsigned char fill;
                const char **glyph;
                int gx;

                if (ch == '\n' || ch == '\r' || ch == '\t') {
                    ch = ' ';
                }
                glyph_width = ch == ' ' ? SPACE_WIDTH : GLYPH_WIDTH;
                fill = isgraph(ch) ? ch : '#';
                glyph = glyph_for(ch);

                for (gx = 0; gx < glyph_width;) {
                    int on = ch != ' ' && glyph[gy][gx] != ' ';
                    int run = 1;
                    int cells;

                    while (gx + run < glyph_width) {
                        int next_on = ch != ' ' && glyph[gy][gx + run] != ' ';

                        if (next_on != on) {
                            break;
                        }
                        run++;
                    }

                    cells = run * scale_x;
                    if (col + cells - 1 > size.cols) {
                        cells = size.cols - col + 1;
                    }
                    if (cells <= 0) {
                        break;
                    }

                    if (on) {
                        printf("\033[%d;38;5;%dm", bright ? 1 : 2, color);
                        write_repeat((char)fill, cells);
                        fputs("\033[0m", stdout);
                    } else {
                        write_repeat(' ', cells);
                    }

                    col += cells;
                    gx += run;
                }

                if (i + 1 < len) {
                    int cells = scale_x;

                    if (col + cells - 1 > size.cols) {
                        cells = size.cols - col + 1;
                    }
                    write_repeat(' ', cells);
                    col += cells;
                }
            }
        }
    }
}

static void draw_bar_mode(const Config *cfg, const Analyzer *analyzer, TermSize size, int clear) {
    int draw_cols = size.cols;
    int usable_rows = size.rows > 1 ? size.rows - 1 : 1;
    int row_start;
    int col_start;
    int row;
    int col;

    if (draw_cols > MAX_BAR_COLS) {
        draw_cols = MAX_BAR_COLS;
    }
    if (usable_rows > MAX_BAR_ROWS) {
        usable_rows = MAX_BAR_ROWS;
    }

    row_start = size.rows > usable_rows ? (size.rows - usable_rows) / 2 + 1 : 1;
    col_start = size.cols > draw_cols ? (size.cols - draw_cols) / 2 + 1 : 1;

    if (clear) {
        fputs("\033[H\033[2J", stdout);
    }

    for (row = 0; row < usable_rows; row++) {
        int threshold = usable_rows - row;

        printf("\033[%d;%dH", row_start + row, col_start);
        for (col = 0; col < draw_cols;) {
            double pos = draw_cols <= 1 ? 0.0 : (double)col / (double)(draw_cols - 1);
            double level = level_at_position(analyzer, pos);
            int height = (int)(level * usable_rows + 0.5);
            int filled = height >= threshold;
            int color = filled ? terminal_color_for(&cfg->palette, pos, level, height == usable_rows) : -1;
            int run = 1;

            while (col + run < draw_cols) {
                double next_pos = draw_cols <= 1 ? 0.0 : (double)(col + run) / (double)(draw_cols - 1);
                double next_level = level_at_position(analyzer, next_pos);
                int next_height = (int)(next_level * usable_rows + 0.5);
                int next_filled = next_height >= threshold;
                int next_color = next_filled ? terminal_color_for(&cfg->palette, next_pos, next_level, next_height == usable_rows) : -1;

                if (next_filled != filled || next_color != color) {
                    break;
                }
                run++;
            }

            if (filled) {
                printf("\033[38;5;%dm", color);
                write_repeat('#', run);
                fputs("\033[0m", stdout);
            } else {
                write_repeat(' ', run);
            }
            col += run;
        }
    }
}

static void graphics_destroy_image(Graphics *gfx) {
    if (gfx->image) {
        gfx->image->data = NULL;
        XDestroyImage(gfx->image);
        gfx->image = NULL;
    }
    free(gfx->pixels);
    gfx->pixels = NULL;
}

static int graphics_resize(Graphics *gfx, int width, int height) {
    if (width < 64) {
        width = 64;
    }
    if (height < 48) {
        height = 48;
    }
    if (gfx->pixels && gfx->width == width && gfx->height == height) {
        return 0;
    }

    graphics_destroy_image(gfx);
    gfx->pixels = calloc((size_t)width * (size_t)height, sizeof(*gfx->pixels));
    if (!gfx->pixels) {
        fprintf(stderr, "Out of memory for %dx%d graphics buffer\n", width, height);
        return -1;
    }

    gfx->image = XCreateImage(gfx->display, DefaultVisual(gfx->display, gfx->screen),
                              (unsigned int)DefaultDepth(gfx->display, gfx->screen),
                              ZPixmap, 0, (char *)gfx->pixels, (unsigned int)width,
                              (unsigned int)height, 32, 0);
    if (!gfx->image) {
        fprintf(stderr, "Cannot create X11 image\n");
        free(gfx->pixels);
        gfx->pixels = NULL;
        return -1;
    }

    gfx->width = width;
    gfx->height = height;
    return 0;
}

static void graphics_set_fullscreen(Graphics *gfx, int enabled) {
    XEvent event;

    memset(&event, 0, sizeof(event));
    event.xclient.type = ClientMessage;
    event.xclient.window = gfx->window;
    event.xclient.message_type = gfx->net_wm_state;
    event.xclient.format = 32;
    event.xclient.data.l[0] = enabled ? 1 : 0;
    event.xclient.data.l[1] = (long)gfx->net_wm_state_fullscreen;
    event.xclient.data.l[2] = 0;
    XSendEvent(gfx->display, DefaultRootWindow(gfx->display), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &event);
    gfx->fullscreen = enabled;
}

static int graphics_init(Graphics *gfx, const Config *cfg) {
    memset(gfx, 0, sizeof(*gfx));
    gfx->display = XOpenDisplay(NULL);
    if (!gfx->display) {
        fprintf(stderr, "Cannot open X11 display. Use -T for terminal mode.\n");
        return -1;
    }

    gfx->screen = DefaultScreen(gfx->display);
    gfx->width = 960;
    gfx->height = 540;
    gfx->window = XCreateSimpleWindow(gfx->display, RootWindow(gfx->display, gfx->screen),
                                      100, 100, (unsigned int)gfx->width,
                                      (unsigned int)gfx->height, 0,
                                      BlackPixel(gfx->display, gfx->screen),
                                      BlackPixel(gfx->display, gfx->screen));
    XStoreName(gfx->display, gfx->window, "tinyEffects");
    XSelectInput(gfx->display, gfx->window,
                 ExposureMask | KeyPressMask | StructureNotifyMask);
    gfx->gc = XCreateGC(gfx->display, gfx->window, 0, NULL);
    gfx->wm_delete = XInternAtom(gfx->display, "WM_DELETE_WINDOW", False);
    gfx->net_wm_state = XInternAtom(gfx->display, "_NET_WM_STATE", False);
    gfx->net_wm_state_fullscreen = XInternAtom(gfx->display, "_NET_WM_STATE_FULLSCREEN", False);
    XSetWMProtocols(gfx->display, gfx->window, &gfx->wm_delete, 1);

    if (graphics_resize(gfx, gfx->width, gfx->height) < 0) {
        XCloseDisplay(gfx->display);
        memset(gfx, 0, sizeof(*gfx));
        return -1;
    }

    XMapWindow(gfx->display, gfx->window);
    XFlush(gfx->display);
    if (cfg->fullscreen) {
        graphics_set_fullscreen(gfx, 1);
    }
    return 0;
}

static void graphics_close(Graphics *gfx) {
    graphics_destroy_image(gfx);
    if (gfx->gc) {
        XFreeGC(gfx->display, gfx->gc);
    }
    if (gfx->display) {
        XDestroyWindow(gfx->display, gfx->window);
        XCloseDisplay(gfx->display);
    }
    memset(gfx, 0, sizeof(*gfx));
}

static void graphics_events(Graphics *gfx) {
    while (gfx->display && XPending(gfx->display) > 0) {
        XEvent event;

        XNextEvent(gfx->display, &event);
        if (event.type == ClientMessage &&
            (Atom)event.xclient.data.l[0] == gfx->wm_delete) {
            g_running = 0;
        } else if (event.type == ConfigureNotify) {
            XConfigureEvent *cfg = &event.xconfigure;

            if (cfg->width != gfx->width || cfg->height != gfx->height) {
                if (graphics_resize(gfx, cfg->width, cfg->height) < 0) {
                    g_running = 0;
                }
            }
        } else if (event.type == KeyPress) {
            KeySym key = XLookupKeysym(&event.xkey, 0);

            if (key == XK_q || key == XK_Q || key == XK_Escape) {
                g_running = 0;
            } else if (key == XK_f || key == XK_F || key == XK_F11) {
                graphics_set_fullscreen(gfx, !gfx->fullscreen);
            }
        }
    }
}

static void put_rect(Graphics *gfx, int x, int y, int w, int h, uint32_t color) {
    int yy;

    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > gfx->width) {
        w = gfx->width - x;
    }
    if (y + h > gfx->height) {
        h = gfx->height - y;
    }
    if (w <= 0 || h <= 0) {
        return;
    }

    for (yy = y; yy < y + h; yy++) {
        uint32_t *row = gfx->pixels + (size_t)yy * (size_t)gfx->width + (size_t)x;
        int xx;

        for (xx = 0; xx < w; xx++) {
            row[xx] = color;
        }
    }
}

static void render_graphics_text(Graphics *gfx, const Config *cfg, const Analyzer *analyzer) {
    size_t len = strlen(cfg->text);
    int base_width = text_base_width(cfg->text);
    int scale = gfx->width / base_width;
    int yscale = gfx->height / GLYPH_HEIGHT;
    int rendered_width;
    int rendered_height;
    int x_start;
    int y_start;
    size_t i;
    int x;

    if (yscale < scale) {
        scale = yscale;
    }
    if (scale < 1) {
        scale = 1;
    }

    rendered_width = base_width * scale;
    rendered_height = GLYPH_HEIGHT * scale;
    x_start = gfx->width > rendered_width ? (gfx->width - rendered_width) / 2 : 0;
    y_start = gfx->height > rendered_height ? (gfx->height - rendered_height) / 2 : 0;
    x = x_start;

    for (i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)cfg->text[i];
        int glyph_width;
        double pos = len == 1 ? 0.0 : (double)i / (double)(len - 1);
        double level = level_at_position(analyzer, pos);
        uint32_t color = rgb_to_u32(palette_color(&cfg->palette, pos, level));
        const char **glyph;
        int gy;

        if (ch == '\n' || ch == '\r' || ch == '\t') {
            ch = ' ';
        }

        glyph_width = ch == ' ' ? SPACE_WIDTH : GLYPH_WIDTH;
        glyph = glyph_for(ch);

        for (gy = 0; gy < GLYPH_HEIGHT; gy++) {
            int gx;

            for (gx = 0; gx < glyph_width; gx++) {
                if (ch != ' ' && glyph[gy][gx] != ' ') {
                    put_rect(gfx, x + gx * scale, y_start + gy * scale,
                             scale, scale, color);
                }
            }
        }

        x += glyph_width * scale;
        if (i + 1 < len) {
            x += scale;
        }
    }
}

static void render_graphics_bars(Graphics *gfx, const Config *cfg, const Analyzer *analyzer) {
    int bars = analyzer->bands;
    int gap = gfx->width > 320 ? 2 : 1;
    int bar_w = (gfx->width - (bars + 1) * gap) / bars;
    int i;

    if (bar_w < 1) {
        bar_w = 1;
        gap = 0;
    }

    for (i = 0; i < bars; i++) {
        double pos = bars == 1 ? 0.0 : (double)i / (double)(bars - 1);
        double level = analyzer->level[i];
        int height = (int)(level * (double)(gfx->height - 12));
        int x = gap + i * (bar_w + gap);
        int y = gfx->height - height - 6;

        put_rect(gfx, x, y, bar_w, height, rgb_to_u32(palette_color(&cfg->palette, pos, level)));
    }
}

static void render_graphics_frame(Graphics *gfx, const Config *cfg, const Analyzer *analyzer) {
    if (!gfx->pixels || !gfx->image) {
        return;
    }

    memset(gfx->pixels, 0, (size_t)gfx->width * (size_t)gfx->height * sizeof(*gfx->pixels));
    if (cfg->text && cfg->text[0] != '\0') {
        render_graphics_text(gfx, cfg, analyzer);
    } else {
        render_graphics_bars(gfx, cfg, analyzer);
    }

    XPutImage(gfx->display, gfx->window, gfx->gc, gfx->image, 0, 0, 0, 0,
              (unsigned int)gfx->width, (unsigned int)gfx->height);
    XFlush(gfx->display);
}

static void render_terminal_frame(const Config *cfg, const Analyzer *analyzer) {
    static TermSize size = {24, 80};
    TermSize current_size;
    TermSize draw_size;
    int clear = 0;

    current_size = read_term_size();
    if (g_resized || current_size.rows != size.rows || current_size.cols != size.cols) {
        size = current_size;
        g_resized = 0;
        clear = 1;
    }

    draw_size = size;
    if (draw_size.cols > 1) {
        draw_size.cols--;
    }
    if (draw_size.rows > 1) {
        draw_size.rows--;
    }

    if (cfg->text && cfg->text[0] != '\0') {
        draw_text_mode(cfg, analyzer, draw_size, clear);
    } else {
        draw_bar_mode(cfg, analyzer, draw_size, clear);
    }

    fflush(stdout);
}

static long monotonic_ms(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

static void sleep_millis(long ms) {
    struct timespec ts;

    ts.tv_sec = ms / 1000L;
    ts.tv_nsec = (ms % 1000L) * 1000000L;
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {
    }
}

static void wait_for_audio(snd_pcm_t *pcm, int timeout_ms) {
    int ready = g_alsa.pcm_wait(pcm, timeout_ms);

    if (ready == 0) {
        return;
    }
    if (ready < 0 && ready != -EPIPE && ready != -ESTRPIPE) {
        sleep_millis(timeout_ms);
    }
}

static void poll_keyboard(void) {
    char ch;

    while (read(STDIN_FILENO, &ch, 1) == 1) {
        if (ch == 'q' || ch == 'Q' || ch == 27) {
            g_running = 0;
            return;
        }
    }
}

static int recover_pcm(snd_pcm_t *pcm, int err) {
    if (err == -EPIPE) {
        return g_alsa.pcm_prepare(pcm);
    }
    if (err == -ESTRPIPE) {
        while (g_running && (err = g_alsa.pcm_resume(pcm)) == -EAGAIN) {
            wait_for_audio(pcm, 10);
            poll_keyboard();
        }
        if (!g_running) {
            return 0;
        }
        if (err < 0) {
            return g_alsa.pcm_prepare(pcm);
        }
        return 0;
    }
    return err;
}

int main(int argc, char **argv) {
    Config cfg = parse_args(argc, argv);
    Analyzer analyzer;
    Graphics gfx;
    snd_pcm_t *pcm = NULL;
    int16_t *samples = NULL;
    long next_render_ms;
    int render_interval_ms;
    int err;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGWINCH, handle_signal);

    if (!cfg.demo) {
        if (load_alsa() < 0) {
            return 1;
        }
        atexit(unload_alsa);

        err = open_capture(&cfg, &pcm);
        if (err < 0) {
            return 1;
        }

        samples = calloc((size_t)cfg.frames, sizeof(*samples));
        if (!samples) {
            fprintf(stderr, "Out of memory\n");
            g_alsa.pcm_close(pcm);
            return 1;
        }
    }

    init_analyzer(&analyzer, cfg.bands, cfg.rate);
    memset(&gfx, 0, sizeof(gfx));
    if (cfg.terminal) {
        setvbuf(stdout, g_stdout_buffer, _IOFBF, sizeof(g_stdout_buffer));
        setup_terminal();
    } else if (graphics_init(&gfx, &cfg) < 0) {
        free(samples);
        if (pcm) {
            g_alsa.pcm_close(pcm);
        }
        return 1;
    }

    render_interval_ms = 1000 / cfg.fps;
    if (render_interval_ms < 1) {
        render_interval_ms = 1;
    }
    next_render_ms = monotonic_ms();

    while (g_running) {
        long now = monotonic_ms();

        if (cfg.demo) {
            if (!cfg.terminal) {
                graphics_events(&gfx);
            }
            if (now >= next_render_ms) {
                animate_demo(&analyzer, now);
                if (cfg.terminal) {
                    render_terminal_frame(&cfg, &analyzer);
                } else {
                    render_graphics_frame(&gfx, &cfg, &analyzer);
                }
                next_render_ms = now + render_interval_ms;
            }

            if (cfg.terminal) {
                poll_keyboard();
            }
            now = monotonic_ms();
            if (next_render_ms > now) {
                long sleep_for = next_render_ms - now;

                sleep_millis(sleep_for > 20 ? 20 : sleep_for);
            }
            continue;
        }

        snd_pcm_sframes_t got = g_alsa.pcm_readi(pcm, samples, cfg.frames);

        if (got < 0) {
            if (got == -EAGAIN) {
                if (cfg.terminal) {
                    poll_keyboard();
                } else {
                    graphics_events(&gfx);
                }
                wait_for_audio(pcm, 20);
                continue;
            }
            err = recover_pcm(pcm, (int)got);
            if (err < 0) {
                fprintf(stderr, "ALSA read failed: %s\n", alsa_error(err));
                break;
            }
            continue;
        }

        if (got == 0) {
            if (cfg.terminal) {
                poll_keyboard();
            } else {
                graphics_events(&gfx);
            }
            wait_for_audio(pcm, 20);
            continue;
        }

        if (got > 0) {
            analyze_samples(&analyzer, samples, (int)got);
        }

        if (now >= next_render_ms) {
            if (cfg.terminal) {
                render_terminal_frame(&cfg, &analyzer);
            } else {
                render_graphics_frame(&gfx, &cfg, &analyzer);
            }
            next_render_ms = now + render_interval_ms;
        }

        if (cfg.terminal) {
            poll_keyboard();
        } else {
            graphics_events(&gfx);
        }
    }

    free(samples);
    if (pcm) {
        g_alsa.pcm_close(pcm);
    }
    if (!cfg.terminal) {
        graphics_close(&gfx);
    }
    return 0;
}
