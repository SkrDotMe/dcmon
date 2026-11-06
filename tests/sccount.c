/* tests/sccount.c - count the system calls of a command (benchmark helper).
 *
 * Uses ptrace(PTRACE_SYSCALL) on its own child, so no root is needed and it
 * works where strace or /proc/PID/io are unavailable.  All threads are
 * followed (PTRACE_O_TRACECLONE): libraries such as NVML start their own
 * threads, and their system calls count too.  Report on stderr:
 *   sccount: total N
 *   sccount: main N / other-threads N (threads seen T)
 *   sccount: nr <syscall number> <count>     (x86-64 numbers)
 *
 *   build/sccount ./dcmon --duration 30 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAXNR 512
#define MAXTH 256

static struct {
    pid_t tid;
    int   in_call;
} th[MAXTH];
static int nth;

static int *in_call(pid_t t)
{
    for (int i = 0; i < nth; i++)
        if (th[i].tid == t)
            return &th[i].in_call;
    if (nth < MAXTH) {
        th[nth].tid = t;
        th[nth].in_call = 0;
        return &th[nth++].in_call;
    }
    static int dummy;
    return &dummy;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s COMMAND [ARGS...]\n", argv[0]);
        return 2;
    }
    pid_t pid = fork();
    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
        raise(SIGSTOP);
        execvp(argv[1], argv + 1);
        _exit(127);
    }
    static unsigned long cnt[MAXNR];
    unsigned long total = 0, main_total = 0;
    int st;
    waitpid(pid, &st, 0);
    ptrace(PTRACE_SETOPTIONS, pid, NULL,
           (void *)(long)(PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL | PTRACE_O_TRACECLONE));
    ptrace(PTRACE_SYSCALL, pid, NULL, NULL);
    for (;;) {
        pid_t t = waitpid(-1, &st, __WALL);
        if (t < 0)
            break;
        if (WIFEXITED(st) || WIFSIGNALED(st)) {
            if (t == pid)
                break;
            continue;
        }
        int sig = 0, s = WSTOPSIG(st);
        if (s == (SIGTRAP | 0x80)) { /* syscall entry or exit */
            int *ic = in_call(t);
            if (!*ic) {
#if defined(__x86_64__)
                struct user_regs_struct r;
                ptrace(PTRACE_GETREGS, t, NULL, &r);
                unsigned long nr = r.orig_rax;
                if (nr < MAXNR)
                    cnt[nr]++;
#endif
                total++;
                if (t == pid)
                    main_total++;
            }
            *ic = !*ic;
        } else if (s == SIGTRAP || s == SIGSTOP) {
            /* clone events and the initial stop of new threads */
        } else {
            sig = s; /* real signals (SIGWINCH, SIGTERM, ...) pass through */
        }
        ptrace(PTRACE_SYSCALL, t, NULL, (void *)(long)sig);
    }
    fprintf(stderr, "sccount: total %lu\n", total);
    fprintf(stderr, "sccount: main %lu / other-threads %lu (threads seen %d)\n", main_total,
            total - main_total, nth);
    for (int i = 0; i < MAXNR; i++)
        if (cnt[i])
            fprintf(stderr, "sccount: nr %d %lu\n", i, cnt[i]);
    return 0;
}
