/*
 * Copyright (C) 2009 John Kacur <jkacur@redhat.com>
 *
 * error routines, similar to those found in
 * Advanced Programming in the UNIX Environment 2nd ed.
 */
#include "error.h"

/* Print an error message, plus a message for err and exit with error err */
void err_exit(int err, char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	err_doit(err, fmt, ap);
	va_end(ap);
	exit(err);
}

/* print an error message and return */
void err_msg(char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	err_doit(0, fmt, ap);
	va_end(ap);
	return;
}

/* Print an error message, plus a message for err, and return */
void err_msg_n(int err, char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	err_doit(err, fmt, ap);
	va_end(ap);
	return;
}

/* print an error message and quit */
void err_quit(char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	err_doit(0, fmt, ap);
	va_end(ap);
	exit(1);
}

void err_doit(int err, const char *fmt, va_list ap)
{
	if (err)
		fprintf(stderr, "%s\n", strerror(err));
	vfprintf(stderr, fmt, ap);
	return;
}
