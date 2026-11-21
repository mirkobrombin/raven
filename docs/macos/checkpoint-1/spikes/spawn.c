// Item 11: posix_spawn + waitpid wall time for trivial native and Rosetta binaries.
// usage: spawn <label> <binary> [<binary> ...]   one timing per listed binary, each run once per round
#include "bench.h"
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
static double spawn_once(const char *path) {
    pid_t pid; char *argv[] = { (char *)path, NULL };
    uint64_t a = now_ns();
    int r = posix_spawn(&pid, path, NULL, NULL, argv, environ);
    CHECK(r == 0, "posix_spawn %s: %d", path, r);
    int st; waitpid(pid, &st, 0);
    uint64_t b = now_ns();
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "child status %d", st);
    return (double)(b - a) / 1000.0;
}
int main(int argc, char **argv) {
    set_qos();
    const char *label = argv[1];
    int n = argc - 2;
    if (n == 1) {   // warm: 3 untimed + 30 timed runs of the same binary
        double s[30];
        for (int i = 0; i < 3; i++) spawn_once(argv[2]);
        for (int i = 0; i < 30; i++) s[i] = spawn_once(argv[2]);
        report("spawn", label, "us", s, 30, argv[2]);
    } else {        // cold: first-ever spawn of each distinct binary
        double *s = malloc(sizeof(double) * n);
        for (int i = 0; i < n; i++) s[i] = spawn_once(argv[2 + i]);
        report("spawn", label, "us", s, n, "first spawn of distinct, never-run binaries");
    }
    return 0;
}
