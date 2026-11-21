// Shared helpers: QoS, timing, stats, CSV/JSON output.
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <sys/sysctl.h>

#ifndef RESULTS_DIR
#define RESULTS_DIR "results"
#endif

static inline uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

static inline void set_qos(void) {
    int r = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    if (r) fprintf(stderr, "warn: pthread_set_qos_class_self_np=%d\n", r);
}

static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

typedef struct { double min, median, p95, mean, stddev, max; int n; } stats_t;

static inline stats_t compute_stats(const double *v, int n) {
    stats_t s = {0};
    double *t = malloc(sizeof(double) * n);
    memcpy(t, v, sizeof(double) * n);
    qsort(t, n, sizeof(double), cmp_d);
    s.n = n; s.min = t[0]; s.max = t[n - 1];
    s.median = (n & 1) ? t[n / 2] : 0.5 * (t[n / 2 - 1] + t[n / 2]);
    int i95 = (int)ceil(0.95 * n) - 1; if (i95 < 0) i95 = 0;
    s.p95 = t[i95];
    double sum = 0; for (int i = 0; i < n; i++) sum += t[i];
    s.mean = sum / n;
    double sq = 0; for (int i = 0; i < n; i++) sq += (t[i] - s.mean) * (t[i] - s.mean);
    s.stddev = n > 1 ? sqrt(sq / (n - 1)) : 0;
    free(t);
    return s;
}

static inline int is_translated(void) {
    int v = 0; size_t sz = sizeof v;
    if (sysctlbyname("sysctl.proc_translated", &v, &sz, NULL, 0) < 0) return 0;
    return v;
}

// Appends one JSON line per metric to results/<bench>.jsonl and writes raw samples to results/<bench>__<metric>.csv
static inline void report(const char *bench, const char *metric, const char *unit, const double *v, int n, const char *note) {
    stats_t s = compute_stats(v, n);
    char path[512];
    snprintf(path, sizeof path, RESULTS_DIR "/%s__%s.csv", bench, metric);
    FILE *f = fopen(path, "w");
    if (f) { fprintf(f, "run,%s\n", unit); for (int i = 0; i < n; i++) fprintf(f, "%d,%.4f\n", i, v[i]); fclose(f); }
    snprintf(path, sizeof path, RESULTS_DIR "/%s.jsonl", bench);
    f = fopen(path, "a");
    if (f) {
        fprintf(f, "{\"bench\":\"%s\",\"metric\":\"%s\",\"unit\":\"%s\",\"n\":%d,\"min\":%.4f,\"median\":%.4f,\"p95\":%.4f,"
                   "\"mean\":%.4f,\"stddev\":%.4f,\"max\":%.4f,\"qos\":\"USER_INTERACTIVE\",\"translated\":%d,\"note\":\"%s\"}\n",
                bench, metric, unit, s.n, s.min, s.median, s.p95, s.mean, s.stddev, s.max, is_translated(), note ? note : "");
        fclose(f);
    }
    printf("%-28s %-34s n=%-3d min=%10.3f med=%10.3f p95=%10.3f sd=%9.3f %s %s\n",
           bench, metric, s.n, s.min, s.median, s.p95, s.stddev, unit, note ? note : "");
}

static inline void reset_jsonl(const char *bench) {
    char path[512]; snprintf(path, sizeof path, RESULTS_DIR "/%s.jsonl", bench);
    remove(path);
}

#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); exit(1); } } while (0)

// macOS has no pthread_barrier_t; minimal generation-counted replacement.
typedef struct { pthread_mutex_t m; pthread_cond_t c; int n, count, gen; } hbarrier_t;
static inline void hbarrier_init(hbarrier_t *b, int n) { pthread_mutex_init(&b->m, NULL); pthread_cond_init(&b->c, NULL); b->n = n; b->count = 0; b->gen = 0; }
static inline void hbarrier_wait(hbarrier_t *b) {
    pthread_mutex_lock(&b->m);
    int g = b->gen;
    if (++b->count == b->n) { b->count = 0; b->gen++; pthread_cond_broadcast(&b->c); }
    else while (g == b->gen) pthread_cond_wait(&b->c, &b->m);
    pthread_mutex_unlock(&b->m);
}
