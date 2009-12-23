#ifndef __ERROR_H
#define __ERROR_H

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

void err_exit(int err, char *fmt, ...);
void err_msg(char *fmt, ...);
void err_msg_n(int err, char *fmt, ...);
void err_quit(char *fmt, ...);
void err_doit(int err, const char *fmt, va_list ap);

#endif	/* __ERROR_H */
