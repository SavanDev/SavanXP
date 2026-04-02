#pragma once

#include "sys/types.h"

#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGKILL 9
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 17
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22

#define NSIG 32

typedef int sig_atomic_t;
typedef unsigned long sigset_t;
typedef void (*sighandler_t)(int);

struct sigaction {
    sighandler_t sa_handler;
    sigset_t sa_mask;
    int sa_flags;
};

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)

#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define SA_RESTART 0x1000

#define kill(...) sx_kill(__VA_ARGS__)
#define signal(...) sx_signal(__VA_ARGS__)
#define sigaction(...) sx_sigaction(__VA_ARGS__)
#define sigemptyset(...) sx_sigemptyset(__VA_ARGS__)
#define sigfillset(...) sx_sigfillset(__VA_ARGS__)
#define sigaddset(...) sx_sigaddset(__VA_ARGS__)
#define sigdelset(...) sx_sigdelset(__VA_ARGS__)
#define sigismember(...) sx_sigismember(__VA_ARGS__)
#define sigprocmask(...) sx_sigprocmask(__VA_ARGS__)
#define sigsuspend(...) sx_sigsuspend(__VA_ARGS__)
#define raise(...) sx_raise(__VA_ARGS__)
#define strsignal sx_strsignal

int sx_kill(pid_t pid, int signal_number);
int sx_raise(int signal_number);
sighandler_t sx_signal(int signal_number, sighandler_t handler);
int sx_sigaction(int signal_number, const struct sigaction* action, struct sigaction* old_action);
int sx_sigemptyset(sigset_t* set);
int sx_sigfillset(sigset_t* set);
int sx_sigaddset(sigset_t* set, int signal_number);
int sx_sigdelset(sigset_t* set, int signal_number);
int sx_sigismember(const sigset_t* set, int signal_number);
int sx_sigprocmask(int how, const sigset_t* set, sigset_t* old_set);
int sx_sigsuspend(const sigset_t* mask);
char* sx_strsignal(int signal_number);
