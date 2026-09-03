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


int kill(pid_t pid, int signal_number);
int raise(int signal_number);
sighandler_t signal(int signal_number, sighandler_t handler);
int sigaction(int signal_number, const struct sigaction* action, struct sigaction* old_action);
int sigemptyset(sigset_t* set);
int sigfillset(sigset_t* set);
int sigaddset(sigset_t* set, int signal_number);
int sigdelset(sigset_t* set, int signal_number);
int sigismember(const sigset_t* set, int signal_number);
int sigprocmask(int how, const sigset_t* set, sigset_t* old_set);
int sigsuspend(const sigset_t* mask);
char* strsignal(int signal_number);
