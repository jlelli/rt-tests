#include <stdio.h>
#include <string.h>
#include <sched.h>
#include "rt-utils.h"

static char debugfileprefix[MAX_PATH];

/*
 * Finds the tracing directory in a mounted debugfs
 */
char *get_debugfileprefix(void)
{
	char type[100];
	FILE *fp;
	int size;

	if (debugfileprefix[0] != '\0')
		return debugfileprefix;

	if ((fp = fopen("/proc/mounts","r")) == NULL)
		return debugfileprefix;

	while (fscanf(fp, "%*s %"
		      STR(MAX_PATH)
		      "s %99s %*s %*d %*d\n",
		      debugfileprefix, type) == 2) {
		if (strcmp(type, "debugfs") == 0)
			break;
	}
	fclose(fp);

	if (strcmp(type, "debugfs") != 0) {
		debugfileprefix[0] = '\0';
		return debugfileprefix;
	}

	size = sizeof(debugfileprefix) - strlen(debugfileprefix);
	strncat(debugfileprefix, "/tracing/", size);

	return debugfileprefix;
}

int check_privs(void)
{
	int policy = sched_getscheduler(0);
	struct sched_param param;

	/* if we're already running a realtime scheduler
	 * then we *should* be able to change things later
	 */
	if (policy == SCHED_FIFO || policy == SCHED_RR)
		return 0;

	/* try to change to SCHED_FIFO */
	param.sched_priority = 1;
	if (sched_setscheduler(0, SCHED_FIFO, &param)) {
		fprintf(stderr, "Unable to change scheduling policy!\n");
		fprintf(stderr, "either run as root or join realtime group\n");
		return 1;
	}

	/* we're good; change back and return success */
	param.sched_priority = 0;
	sched_setscheduler(0, policy, NULL);
	return 0;
}
