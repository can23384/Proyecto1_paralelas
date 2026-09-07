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
