#pragma once

#include "sys/types.h"

typedef unsigned int tcflag_t;
typedef unsigned char cc_t;

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t c_cc[8];
};

#define VINTR 0
#define VQUIT 1
#define VERASE 2
#define VKILL 3
#define VEOF 4
#define VMIN 5
#define VTIME 6

#define ICANON 0x0002u
#define ECHO 0x0008u
#define ECHOK 0x0020u
#define ECHONL 0x0040u

#define TCSANOW 0

#define tcgetattr sx_tcgetattr
#define tcsetattr sx_tcsetattr
#define tcgetpgrp sx_tcgetpgrp
#define tcsetpgrp sx_tcsetpgrp

int sx_tcgetattr(int fd, struct termios* value);
int sx_tcsetattr(int fd, int optional_actions, const struct termios* value);
pid_t sx_tcgetpgrp(int fd);
int sx_tcsetpgrp(int fd, pid_t pgrp);
