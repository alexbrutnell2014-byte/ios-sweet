/*
 * untar: extract a plain ustar archive (no compression) on the phone, which has no tar.
 *   untar ARCHIVE DESTDIR
 * Regular files, directories and symlinks; modes kept; GNU long names (type L) supported.
 * Refuses absolute paths and "..".
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned long oct(const char *p, int n)
{
	unsigned long v = 0;
	for (int i = 0; i < n && p[i]; i++)
		if (p[i] >= '0' && p[i] <= '7')
			v = v * 8 + (p[i] - '0');
	return v;
}

static void mkparents(char *path)
{
	for (char *s = path + 1; *s; s++)
		if (*s == '/') {
			*s = 0;
			mkdir(path, 0755);
			*s = '/';
		}
}

static int bad(const char *name)
{
	return name[0] == '/' || !strcmp(name, "..") || !strncmp(name, "../", 3) || strstr(name, "/../") ||
	       (strlen(name) >= 3 && !strcmp(name + strlen(name) - 3, "/.."));
}

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: untar ARCHIVE DESTDIR\n");
		return 2;
	}
	FILE *f = fopen(argv[1], "rb");
	if (!f) {
		perror(argv[1]);
		return 1;
	}
	mkdir(argv[2], 0755);
	char h[512], longname[4096] = "";
	static char buf[1 << 16];
	int files = 0;
	while (fread(h, 1, 512, f) == 512) {
		if (!h[0])
			break;
		unsigned long size = oct(h + 124, 12), mode = oct(h + 100, 8) & 07777;
		char type = h[156], name[4096];
		if (longname[0]) {
			strlcpy(name, longname, sizeof(name));
			longname[0] = 0;
		} else if (!memcmp(h + 257, "ustar", 5) && h[345]) {
			snprintf(name, sizeof(name), "%.155s/%.100s", h + 345, h);
		} else {
			snprintf(name, sizeof(name), "%.100s", h);
		}
		unsigned long pad = (512 - size % 512) % 512;
		if (type == 'L') {	/* GNU long name for the next entry */
			unsigned long n = size < sizeof(longname) - 1 ? size : sizeof(longname) - 1;
			if (fread(longname, 1, n, f) != n)
				break;
			longname[n] = 0;
			fseek(f, (long)(size - n + pad), SEEK_CUR);
			continue;
		}
		if (type == 'x' || type == 'g') {	/* pax headers: skip */
			fseek(f, (long)(size + pad), SEEK_CUR);
			continue;
		}
		while (!strncmp(name, "./", 2))
			memmove(name, name + 2, strlen(name + 2) + 1);
		if (!name[0] || bad(name)) {
			fprintf(stderr, "skip %s\n", name);
			fseek(f, (long)(size + pad), SEEK_CUR);
			continue;
		}
		char out[8192];
		snprintf(out, sizeof(out), "%s/%s", argv[2], name);
		size_t ol = strlen(out);
		if (ol > 1 && out[ol - 1] == '/')
			out[ol - 1] = 0;
		mkparents(out);
		if (type == '5') {
			mkdir(out, mode ? mode : 0755);
			chmod(out, mode ? mode : 0755);
		} else if (type == '2') {
			char tgt[101];
			snprintf(tgt, sizeof(tgt), "%.100s", h + 157);
			unlink(out);
			if (symlink(tgt, out))
				perror(out);
		} else if (type == '0' || type == 0) {
			unlink(out);
			int o = open(out, O_WRONLY | O_CREAT | O_TRUNC, mode ? mode : 0644);
			if (o < 0) {
				perror(out);
				return 1;
			}
			unsigned long left = size;
			while (left) {
				size_t n = left < sizeof(buf) ? left : sizeof(buf);
				if (fread(buf, 1, n, f) != n || write(o, buf, n) != (ssize_t)n) {
					fprintf(stderr, "short %s\n", out);
					return 1;
				}
				left -= n;
			}
			fchmod(o, mode ? mode : 0644);
			close(o);
			fseek(f, (long)pad, SEEK_CUR);
			files++;
			continue;
		}
		fseek(f, (long)(size + pad), SEEK_CUR);
	}
	printf("untar: %d files -> %s\n", files, argv[2]);
	return 0;
}
