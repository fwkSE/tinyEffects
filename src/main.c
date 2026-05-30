#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
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
    const char *device;
    const char *text;
    unsigned int rate;
    int fps;
    int bands;
    int demo;
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

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_resized = 1;
static struct termios g_old_termios;
static int g_have_termios = 0;
static int g_old_stdin_flags = -1;
static AlsaApi g_alsa;
static char g_stdout_buffer[OUTPUT_BUFFER_SIZE];

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
            "  -h         Show this help\n"
            "\n"
            "Examples:\n"
            "  %s -d default -t tinyEffects\n"
            "  %s -m -t tinyEffects\n"
            "  %s -d hw:Loopback,1,0 -t \"Now playing\"\n",
            argv0, DEFAULT_RATE, DEFAULT_FPS, MIN_BANDS, MAX_BANDS,
            DEFAULT_BANDS, argv0, argv0, argv0);
}

static Config parse_args(int argc, char **argv) {
    Config cfg;
    int opt;

    cfg.device = "default";
    cfg.text = NULL;
    cfg.rate = DEFAULT_RATE;
    cfg.fps = DEFAULT_FPS;
    cfg.bands = DEFAULT_BANDS;
    cfg.demo = 0;
    cfg.frames = DEFAULT_FRAMES;

    while ((opt = getopt(argc, argv, "d:t:r:f:b:mh")) != -1) {
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

static int color_for(double pos, double level, int bright) {
    static const int dim_palette[] = {24, 25, 28, 58, 94, 88};
    static const int mid_palette[] = {39, 33, 40, 118, 214, 202};
    static const int hot_palette[] = {45, 51, 46, 226, 208, 196};
    int index = (int)(pos * 5.999);

    if (index < 0) {
        index = 0;
    } else if (index > 5) {
        index = 5;
    }

    if (level > 0.72 || bright) {
        return hot_palette[index];
    }
    if (level > 0.28) {
        return mid_palette[index];
    }
    return dim_palette[index];
}

static void write_repeat(char ch, int count) {
    while (count-- > 0) {
        fputc(ch, stdout);
    }
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
                int color = color_for(pos, level, bright);
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

static void draw_bar_mode(const Analyzer *analyzer, TermSize size, int clear) {
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
            int color = filled ? color_for(pos, level, height == usable_rows) : -1;
            int run = 1;

            while (col + run < draw_cols) {
                double next_pos = draw_cols <= 1 ? 0.0 : (double)(col + run) / (double)(draw_cols - 1);
                double next_level = level_at_position(analyzer, next_pos);
                int next_height = (int)(next_level * usable_rows + 0.5);
                int next_filled = next_height >= threshold;
                int next_color = next_filled ? color_for(next_pos, next_level, next_height == usable_rows) : -1;

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

static void render_frame(const Config *cfg, const Analyzer *analyzer) {
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
        draw_bar_mode(analyzer, draw_size, clear);
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
    setvbuf(stdout, g_stdout_buffer, _IOFBF, sizeof(g_stdout_buffer));
    setup_terminal();

    render_interval_ms = 1000 / cfg.fps;
    if (render_interval_ms < 1) {
        render_interval_ms = 1;
    }
    next_render_ms = monotonic_ms();

    while (g_running) {
        long now = monotonic_ms();

        if (cfg.demo) {
            if (now >= next_render_ms) {
                animate_demo(&analyzer, now);
                render_frame(&cfg, &analyzer);
                next_render_ms = now + render_interval_ms;
            }

            poll_keyboard();
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
                poll_keyboard();
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
            poll_keyboard();
            wait_for_audio(pcm, 20);
            continue;
        }

        if (got > 0) {
            analyze_samples(&analyzer, samples, (int)got);
        }

        if (now >= next_render_ms) {
            render_frame(&cfg, &analyzer);
            next_render_ms = now + render_interval_ms;
        }

        poll_keyboard();
    }

    free(samples);
    if (pcm) {
        g_alsa.pcm_close(pcm);
    }
    return 0;
}
