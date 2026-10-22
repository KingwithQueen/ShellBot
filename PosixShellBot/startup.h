#ifndef SB_POSIX_STARTUP_H
#define SB_POSIX_STARTUP_H
#include "posix.h"
bool sb_startup_option(const char *option);
int sb_startup_cli(int argc, char **argv);
#endif
