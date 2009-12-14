#define _STR(x) #x
#define STR(x) _STR(x)
#define MAX_PATH 256

int check_privs(void);
char *get_debugfileprefix(void);
