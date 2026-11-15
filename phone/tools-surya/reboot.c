/*
 * reboot [-h] [-k]: restart (or with -h power off) the phone.
 * Default: through launchd (reboot3), the orderly path SpringBoard's Restart takes.
 * -k: straight to the kernel (reboot(2)), skipping launchd's shutdown of services.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RB_AUTOBOOT	0
#define RB_HALT		0x08
#define RB2_FULLREBOOT	0x8000000000000000ULL

extern int reboot(int howto);
extern int reboot3(uint64_t flags, ...);

int
main(int argc, char **argv)
{
	int halt = 0, kernel = 0;
	for (int i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "-h")) {
			halt = 1;
		} else if (!strcmp(argv[i], "-k")) {
			kernel = 1;
		} else {
			fprintf(stderr, "usage: reboot [-h] [-k]\n");
			return 2;
		}
	}
	printf("%s via %s\n", halt ? "power off" : "restart", kernel ? "kernel" : "launchd");
	fflush(stdout);
	int rc = kernel ? reboot(halt ? RB_HALT : RB_AUTOBOOT)
	    : reboot3(halt ? (uint64_t)RB_HALT : RB2_FULLREBOOT);
	perror("reboot");
	return rc ? 1 : 0;
}
