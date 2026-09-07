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
