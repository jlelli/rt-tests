#ifndef __RT_UTILS_H
#define __RT_UTILS_H


#define _STR(x) #x
#define STR(x) _STR(x)
#define MAX_PATH 256

int check_privs(void);
char *get_debugfileprefix(void);

void warn(char *fmt, ...);
void fatal(char *fmt, ...);

#endif	/* __RT_UTILS.H */
