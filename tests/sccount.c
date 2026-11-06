/* tests/sccount.c - count the system calls of a command (benchmark helper).
 *
 * Uses ptrace(PTRACE_SYSCALL) on its own child, so no root is needed and it
 * works where strace or /proc/PID/io are unavailable.  The report goes to
 * stderr after the command exits:  "sccount: total N" followed by per-
 * syscall-number counts (x86-64 numbers; see `ausyscall --dump`).
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
    unsigned long total = 0;
    int st, in_call = 0;
    waitpid(pid, &st, 0);
    ptrace(PTRACE_SETOPTIONS, pid, NULL, (void *)(long)(PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL));
    int sig = 0;
    for (;;) {
        if (ptrace(PTRACE_SYSCALL, pid, NULL, (void *)(long)sig) != 0)
            break;
        sig = 0;
        if (waitpid(pid, &st, 0) < 0 || WIFEXITED(st) || WIFSIGNALED(st))
            break;
        if (!WIFSTOPPED(st))
            continue;
        if (WSTOPSIG(st) == (SIGTRAP | 0x80)) { /* syscall entry or exit */
            if (!in_call) {
#if defined(__x86_64__)
                struct user_regs_struct r;
                ptrace(PTRACE_GETREGS, pid, NULL, &r);
                unsigned long nr = r.orig_rax;
                if (nr < MAXNR)
                    cnt[nr]++;
#endif
                total++;
            }
            in_call = !in_call;
        } else if (WSTOPSIG(st) != SIGTRAP) {
            sig = WSTOPSIG(st); /* pass real signals (SIGWINCH, ...) through */
        }
    }
    fprintf(stderr, "sccount: total %lu\n", total);
    for (int i = 0; i < MAXNR; i++)
        if (cnt[i])
            fprintf(stderr, "sccount: nr %d %lu\n", i, cnt[i]);
    return 0;
}
