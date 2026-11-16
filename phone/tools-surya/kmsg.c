/*
 * kmsg [PATTERN]: the kernel's message buffer (as macOS dmesg reads it: libproc proc_kmsgbuf), or only its
 * lines containing PATTERN.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

extern int proc_kmsgbuf(void *buffer, uint32_t buffersize);

int
main(int argc, char **argv)
{
	int size = 0;
	size_t sl = sizeof(size);
	if (sysctlbyname("kern.msgbuf", &size, &sl, NULL, 0) != 0 || size <= 0)
		size = 1 << 20;
	char *buf = malloc((size_t)size + 1);
	if (!buf)
		return 1;
	int n = proc_kmsgbuf(buf, (uint32_t)size);
	if (n <= 0) {
		perror("proc_kmsgbuf");
		return 1;
	}
	buf[n] = 0;
	for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n"))
		if (argc < 2 || strstr(line, argv[1]))
			puts(line);
	return 0;
}
