/*
 * mvf FROM TO: rename(2) (this iOS has no mv). For tools/surya-run.py, e.g. names with spaces that the USB
 * file server's PUT cannot carry: upload under a plain name, then mvf it into place.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: mvf FROM TO\n");
		return 2;
	}
	if (rename(argv[1], argv[2]) != 0) {
		fprintf(stderr, "mvf: %s -> %s: %s\n", argv[1], argv[2], strerror(errno));
		return 1;
	}
	return 0;
}
