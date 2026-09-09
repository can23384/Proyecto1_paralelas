#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif
#ifndef HEADLESS_ONLY
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#endif
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <math.h>

#define PI 3.14159265358979323846
#define TRAIL_PAIRS 3

typedef enum { SEQUENTIAL, PAR_STATIC, PAR_DYNAMIC } Mode;
typedef struct { double x, y, angle; } FootprintPair;
typedef struct {
    double x, y, vx, vy, accumulator;
    unsigned char r, g, b;
    int trail_count;
    FootprintPair trail[TRAIL_PAIRS];
} Walker;
typedef struct {
    int n, width, height, threads, chunk, frames, warmup, repeats;
    int benchmark, headless, self_test;
    unsigned seed;
    double speed, radius, foot_size, foot_gap, step_interval, fps, dt;
    Mode mode;
    const char *csv;
} Config;
typedef struct { Walker *current, *next; } Simulation;
typedef struct { double compute, total; int threads; } Measurement;
typedef struct {
#ifndef HEADLESS_ONLY
    SDL_Window *window;
    SDL_Renderer *renderer;
#else
    int unused;
#endif
} Graphics;

/* Algunos GCC/MinGW antiguos usan un reloj OpenMP de baja resolucion. */
static double now_seconds(void) {
#ifdef _WIN32
    LARGE_INTEGER counter, frequency;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart/(double)frequency.QuadPart;
#else
    return omp_get_wtime();
#endif
}

static const char *mode_name(Mode mode) {
    return mode == SEQUENTIAL ? "seq" :
           mode == PAR_STATIC ? "static" : "dynamic";
}

static void usage(const char *program) {
    printf("Uso: %s [opciones]\n"
           "  --n N                  Caminantes (1..100000)\n"
           "  --mode seq|static|dynamic\n"
           "  --threads N            Hilos OpenMP (1..1024)\n"
           "  --width N --height N   Canvas minimo 640x480\n"
           "  --speed X              Rapidez maxima, pixeles/segundo\n"
           "  --radius X             Radio de colision\n"
           "  --foot-size X --foot-gap X\n"
           "  --step-interval X      Segundos entre pares de huellas\n"
           "  --fps X                Limite interactivo; 0 = sin limite\n"
           "  --seed N               Semilla reproducible\n"
           "  --chunk N              Grupo del reparto dynamic\n"
           "  --benchmark            Comparar las tres versiones\n"
           "  --headless             Benchmark solo CPU, sin ventana\n"
           "  --frames N --warmup N --repeats N (minimo 10 repeticiones)\n"
           "  --dt X                 Paso fijo del benchmark\n"
           "  --csv ruta.csv         Resultados (no sobrescribe archivos)\n"
           "  --self-test            Verificar equivalencia y colisiones\n"
           "  --help                 Ayuda\n"
           "Durante la animacion: ESC o cerrar ventana para salir.\n",
           program);
}

static int integer(const char *text, long minimum, long maximum, int *out) {
    char *end;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno || !*text || *end || value < minimum || value > maximum)
        return 0;
    *out = (int)value;
    return 1;
}

static int real_number(const char *text, double minimum,
                       double maximum, double *out) {
    char *end;
    errno = 0;
    double value = strtod(text, &end);
    if (errno || !*text || *end || !isfinite(value) ||
        value < minimum || value > maximum) return 0;
    *out = value;
    return 1;
}

/* Devuelve 1: valido, 0: ayuda, -1: error. */
static int arguments(int argc, char **argv, Config *c) {
    for (int i = 1; i < argc; ++i) {
        const char *key = argv[i];
        if (!strcmp(key, "--help")) { usage(argv[0]); return 0; }
        if (!strcmp(key, "--benchmark")) { c->benchmark = 1; continue; }
        if (!strcmp(key, "--headless")) { c->headless = 1; continue; }
        if (!strcmp(key, "--self-test")) { c->self_test = 1; continue; }
        if (++i >= argc) {
            fprintf(stderr, "Falta valor para %s\n", key); return -1;
        }
        const char *value = argv[i];
        int ok = 0;
        if (!strcmp(key, "--mode")) {
            if (!strcmp(value, "seq")) { c->mode = SEQUENTIAL; ok = 1; }
            if (!strcmp(value, "static")) { c->mode = PAR_STATIC; ok = 1; }
            if (!strcmp(value, "dynamic")) { c->mode = PAR_DYNAMIC; ok = 1; }
        }
#define INT_OPT(name, field, lo, hi) \
        else if (!strcmp(key, name)) ok = integer(value, lo, hi, &c->field)
#define REAL_OPT(name, field, lo, hi) \
        else if (!strcmp(key, name)) ok = real_number(value, lo, hi, &c->field)
        INT_OPT("--n", n, 1, 100000);
        INT_OPT("--width", width, 640, 16384);
        INT_OPT("--height", height, 480, 16384);
        INT_OPT("--threads", threads, 1, 1024);
        INT_OPT("--chunk", chunk, 1, 100000);
        INT_OPT("--frames", frames, 1, 1000000);
        INT_OPT("--warmup", warmup, 0, 1000000);
        INT_OPT("--repeats", repeats, 10, 1000);
        REAL_OPT("--speed", speed, 1, 2000);
        REAL_OPT("--radius", radius, 1, 100);
        REAL_OPT("--foot-size", foot_size, 1, 100);
        REAL_OPT("--foot-gap", foot_gap, 0, 100);
        REAL_OPT("--step-interval", step_interval, 0.001, 10);
        REAL_OPT("--fps", fps, 0, 1000);
        REAL_OPT("--dt", dt, 0.00001, 0.1);
        else if (!strcmp(key, "--seed")) {
            int seed;
            ok = integer(value, 0, 2147483647, &seed);
            if (ok) c->seed = (unsigned)seed;
        } else if (!strcmp(key, "--csv")) {
            ok = *value != '\0'; c->csv = value;
        }
#undef INT_OPT
#undef REAL_OPT
        if (!ok) {
            fprintf(stderr, "Opcion o valor invalido: %s %s\n", key, value);
            return -1;
        }
    }
    if (c->headless && !c->benchmark && !c->self_test) {
        fprintf(stderr, "--headless requiere --benchmark o --self-test\n");
        return -1;
    }
    return 1;
}

/* PRNG local: inicializacion serial reproducible, sin rand() compartido. */
static double random_unit(uint32_t *state) {
    *state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
    return (double)(*state >> 8) / 16777216.0;
}

static double margin(const Config *c) {
    return fmax(c->radius, c->foot_gap + c->foot_size * 0.5) + 2;
}

static void add_pair(Walker *w) {
    for (int k = TRAIL_PAIRS - 1; k > 0; --k) w->trail[k] = w->trail[k-1];
    w->trail[0] = (FootprintPair){w->x, w->y, atan2(w->vy, w->vx)};
    if (w->trail_count < TRAIL_PAIRS) ++w->trail_count;
}

static int allocate_simulation(Simulation *s, int n) {
    s->current = calloc((size_t)n, sizeof(Walker));
    s->next = calloc((size_t)n, sizeof(Walker));
    if (!s->current || !s->next) {
        free(s->current); free(s->next);
        s->current = s->next = NULL;
        fprintf(stderr, "Memoria insuficiente para %d caminantes\n", n);
        return 0;
    }
    return 1;
}

static void free_simulation(Simulation *s) {
    free(s->current); free(s->next);
    s->current = s->next = NULL;
}

static void reset_simulation(Simulation *s, const Config *c) {
    uint32_t rng = c->seed;
    double m = margin(c);
    memset(s->current, 0, (size_t)c->n * sizeof(Walker));
    memset(s->next, 0, (size_t)c->n * sizeof(Walker));
    for (int i = 0; i < c->n; ++i) {
        Walker *w = &s->current[i];
        w->x = m + random_unit(&rng) * (c->width - 2*m);
        w->y = m + random_unit(&rng) * (c->height - 2*m);
        double angle = random_unit(&rng) * 2 * PI;
        double speed = c->speed * (0.5 + 0.5 * random_unit(&rng));
        w->vx = cos(angle) * speed; w->vy = sin(angle) * speed;
        w->r = (unsigned char)(70 + 185 * random_unit(&rng));
        w->g = (unsigned char)(70 + 185 * random_unit(&rng));
        w->b = (unsigned char)(70 + 185 * random_unit(&rng));
        add_pair(w);
    }
}

/* Colision de discos de igual masa: impulso normal al aproximarse.
 * Las contribuciones de contactos simultaneos se promedian para evitar
 * impulsos excesivos. Es una aproximacion visual, no un solver rigido.
 * Cada par se visita desde ambos extremos, siempre con el mismo snapshot.
 */
static void update_one(const Walker *old, Walker *next,
                       const Config *c, int i, double dt) {
    const Walker *a = &old[i];
    Walker result = *a;
    double dvx = 0, dvy = 0, dx_correction = 0, dy_correction = 0;
    int contacts = 0;
    double diameter = 2*c->radius;
    for (int j = 0; j < c->n; ++j) {
        if (j == i) continue;
        double dx = a->x - old[j].x, dy = a->y - old[j].y;
        double distance2 = dx*dx + dy*dy;
        if (distance2 >= diameter*diameter) continue;
        double distance = sqrt(distance2), nx, ny;
        if (distance > 1e-10) { nx = dx/distance; ny = dy/distance; }
        else {
            /* Normal determinista y opuesta incluso en superposicion exacta. */
            uint32_t lo = (uint32_t)(i < j ? i : j);
            uint32_t hi = (uint32_t)(i < j ? j : i);
            uint32_t hash = lo * UINT32_C(73856093) ^ hi * UINT32_C(19349663);
            double angle = (hash % 3600) * (2*PI/3600);
            double sign = i < j ? 1.0 : -1.0;
            nx = sign*cos(angle); ny = sign*sin(angle);
        }
        double approach = (a->vx-old[j].vx)*nx + (a->vy-old[j].vy)*ny;
        if (approach < 0) { dvx -= approach*nx; dvy -= approach*ny; }
        double correction = 0.5*(diameter-distance);
        dx_correction += correction*nx; dy_correction += correction*ny;
        ++contacts;
    }
    if (contacts) {
        result.vx += dvx/contacts; result.vy += dvy/contacts;
        result.x += dx_correction/contacts;
        result.y += dy_correction/contacts;
    }
    double speed = hypot(result.vx, result.vy);
    if (speed > c->speed) {
        result.vx *= c->speed/speed; result.vy *= c->speed/speed;
    }
    result.x += result.vx*dt; result.y += result.vy*dt;
    double m = margin(c);
    if (result.x < m) { result.x = m; result.vx = fabs(result.vx); }
    if (result.x > c->width-m) {
        result.x = c->width-m; result.vx = -fabs(result.vx);
    }
    if (result.y < m) { result.y = m; result.vy = fabs(result.vy); }
    if (result.y > c->height-m) {
        result.y = c->height-m; result.vy = -fabs(result.vy);
    }
    result.accumulator += dt;
    if (result.accumulator >= c->step_interval) {
        result.accumulator = fmod(result.accumulator, c->step_interval);
        add_pair(&result);
    }
    next[i] = result;
}

static void step(Simulation *s, const Config *c, Mode mode, double dt) {
    const Walker *old = s->current;
    Walker *next = s->next;
    int n = c->n;
    if (mode == SEQUENTIAL) {
        for (int i = 0; i < n; ++i) update_one(old, next, c, i, dt);
    } else if (mode == PAR_STATIC) {
        #pragma omp parallel for default(none) shared(old,next,c,n,dt) schedule(static)
        for (int i = 0; i < n; ++i) update_one(old, next, c, i, dt);
    } else {
        int chunk = c->chunk;
        #pragma omp parallel for default(none) shared(old,next,c,n,dt,chunk) schedule(dynamic,chunk)
        for (int i = 0; i < n; ++i) update_one(old, next, c, i, dt);
    }
    /* La barrera implicita anterior protege el intercambio de buffers. */
    s->current = next;
    s->next = (Walker *)old;
}

static void advance(Simulation *s, const Config *c, Mode mode, double dt) {
    /* Subpasos limitan desplazamiento y saltos a traves de otros discos. */
    double maximum_dt = fmin(c->radius/(2*c->speed), c->step_interval);
    int count = (int)ceil(dt/maximum_dt);
    if (count < 1) count = 1;
    for (int k = 0; k < count; ++k) step(s, c, mode, dt/count);
}

static int actual_threads(Mode mode) {
    int count = 1;
    if (mode != SEQUENTIAL) {
        #pragma omp parallel default(none) shared(count)
        {
            #pragma omp single
            count = omp_get_num_threads();
        }
    }
    return count;
}

#ifndef HEADLESS_ONLY
static int open_graphics(Graphics *g, const Config *c) {
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) goto error;
    g->window = SDL_CreateWindow("Huellas", SDL_WINDOWPOS_CENTERED,
                  SDL_WINDOWPOS_CENTERED, c->width, c->height, SDL_WINDOW_SHOWN);
    if (!g->window) goto error;
    /* Sin PRESENTVSYNC: no ocultar las diferencias de rendimiento. */
    g->renderer = SDL_CreateRenderer(g->window, -1, SDL_RENDERER_ACCELERATED);
    if (!g->renderer)
        g->renderer = SDL_CreateRenderer(g->window, -1, SDL_RENDERER_SOFTWARE);
    if (!g->renderer) goto error;
    SDL_RendererInfo info;
    if (SDL_GetRendererInfo(g->renderer, &info) == 0)
        printf("Renderer: %s\n", info.name);
    return 1;
error:
    fprintf(stderr, "SDL: %s\n", SDL_GetError());
    return 0;
}

static void close_graphics(Graphics *g) {
    if (g->renderer) SDL_DestroyRenderer(g->renderer);
    if (g->window) SDL_DestroyWindow(g->window);
    SDL_Quit();
}

static int events(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_QUIT ||
            (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)) return 0;
    return 1;
}

/* Fuente bitmap integrada: no requiere SDL_ttf ni archivos externos. */
static void text(SDL_Renderer *r, int x, int y, const char *message) {
    static const char chars[] = "0123456789FPSN:.- ";
    static const unsigned char glyphs[][7] = {
        {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},
        {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30},
        {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
        {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8},
        {14,17,17,14,17,17,14}, {14,17,17,15,1,1,14},
        {31,16,16,30,16,16,16}, {30,17,17,30,16,16,16},
        {15,16,16,14,1,1,30}, {17,25,25,21,19,19,17},
        {0,4,4,0,4,4,0}, {0,0,0,0,0,12,12},
        {0,0,0,31,0,0,0}, {0,0,0,0,0,0,0}
    };
    SDL_SetRenderDrawColor(r, 235, 240, 250, 255);
    for (; *message; ++message, x += 12) {
        const char *p = strchr(chars, *message);
        if (!p) continue;
        for (int row = 0; row < 7; ++row)
            for (int col = 0; col < 5; ++col)
                if (glyphs[p-chars][row] & (1 << (4-col))) {
                    SDL_Rect pixel = {x+2*col, y+2*row, 2, 2};
                    SDL_RenderFillRect(r, &pixel);
                }
    }
}

static void render(Graphics *g, const Simulation *s, const Config *c,
                   Mode mode, double fps) {
    SDL_Renderer *r = g->renderer;
    SDL_SetRenderDrawColor(r, 5, 5, 12, 255);
    SDL_RenderClear(r);
    for (int i = 0; i < c->n; ++i) {
        const Walker *w = &s->current[i];
        for (int k = w->trail_count-1; k >= 0; --k) {
            const FootprintPair *p = &w->trail[k];
            double px = -sin(p->angle)*c->foot_gap;
            double py = cos(p->angle)*c->foot_gap;
            double brightness = 1.0 - 0.27*k;
            SDL_SetRenderDrawColor(r, (Uint8)(w->r*brightness),
                                      (Uint8)(w->g*brightness),
                                      (Uint8)(w->b*brightness), 255);
            SDL_Rect feet[2];
            for (int f = 0; f < 2; ++f) {
                double sign = f ? 1 : -1;
                feet[f] = (SDL_Rect){
                    (int)(p->x+sign*px-c->foot_size/2),
                    (int)(p->y+sign*py-c->foot_size/2),
                    (int)c->foot_size, (int)c->foot_size};
            }
            SDL_RenderFillRects(r, feet, 2);
        }
    }
    SDL_Rect background = {6, 6, 440, 30};
    SDL_SetRenderDrawColor(r, 12, 18, 28, 255);
    SDL_RenderFillRect(r, &background);
    char hud[96];
    snprintf(hud, sizeof(hud), "N:%d FPS:%.1f", c->n, fps);
    text(r, 14, 14, hud);
    SDL_RenderPresent(r);
    (void)mode;
}
#else
static int open_graphics(Graphics *g, const Config *c) {
    (void)g; (void)c;
    fprintf(stderr, "Esta compilacion no incluye SDL2. Use --headless --benchmark.\n");
    return 0;
}
static void close_graphics(Graphics *g) { (void)g; }
static int events(void) { return 1; }
static void render(Graphics *g, const Simulation *s, const Config *c,
                   Mode mode, double fps) {
    (void)g; (void)s; (void)c; (void)mode; (void)fps;
}
#endif

static int interactive(Graphics *g, Simulation *s, const Config *c) {
    reset_simulation(s, c);
    double previous = now_seconds(), window_start = previous, fps = 0;
    int frames = 0;
    while (events()) {
        double start = now_seconds();
        double dt = fmin(start-previous, 0.05);
        previous = start;
        advance(s, c, c->mode, dt);
        render(g, s, c, c->mode, fps);
#ifndef HEADLESS_ONLY
        if (c->fps > 0) {
            double remaining = 1/c->fps - (now_seconds()-start);
            if (remaining > 0) SDL_Delay((Uint32)(remaining*1000));
        }
#endif
        ++frames;
        double now = now_seconds();
        /* FPS usa tiempo real, nunca el dt recortado de la simulacion. */
        if (now-window_start >= 0.5) {
            fps = frames/(now-window_start);
#ifndef HEADLESS_ONLY
            char title[160];
            snprintf(title, sizeof(title), "Huellas | %s | N=%d | hilos=%d | FPS=%.1f",
                     mode_name(c->mode), c->n, actual_threads(c->mode), fps);
            SDL_SetWindowTitle(g->window, title);
#endif
            window_start = now; frames = 0;
        }
    }
    return 1;
}
