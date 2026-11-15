/*
 * cmdd: run commands handed to the phone as files (there is no shell or ssh on this iOS).
 *
 * launchd keeps it running as root (job org.iosonpixel.cmdd). Every second it looks in /var/local/cmd for
 * NAME.cmd files (the PC writes them over the USB file server, tools/surya-files.py), one per command:
 *   one argument per line; "ENV KEY=VALUE" lines set the environment; "UID N" runs as user N;
 *   "TIMEOUT S" kills it after S seconds (default 300).
 * The command's stdout+stderr go to NAME.out, its wait status to NAME.rc (written last: the PC polls for it).
 * NAME.cmd is renamed NAME.run while it runs, then removed. /var/local/cmd is root-only (0700).
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define DIR_CMD "/var/local/cmd"
#define MAXARG 64

static void run(const char *name)
{
	char path[512], run[512], out[512], rc[512], tmp[512];
	snprintf(path, sizeof(path), DIR_CMD "/%s.cmd", name);
	snprintf(run, sizeof(run), DIR_CMD "/%s.run", name);
	snprintf(out, sizeof(out), DIR_CMD "/%s.out", name);
	snprintf(rc, sizeof(rc), DIR_CMD "/%s.rc", name);
	if (rename(path, run) != 0)
		return;
	FILE *f = fopen(run, "r");
	if (!f)
		return;
	static char lines[MAXARG][1024];
	char *argv[MAXARG + 1];
	char *envp[MAXARG + 64];
	int argc = 0, envc = 0, uid = -1, timeout = 300;
	for (char **e = environ; *e && envc < MAXARG; e++)
		envp[envc++] = *e;
	int n = 0;
	while (n < MAXARG && fgets(lines[n], sizeof(lines[n]), f)) {
		char *l = lines[n];
		l[strcspn(l, "\r\n")] = 0;
		if (!strncmp(l, "ENV ", 4) && envc < MAXARG + 63)
			envp[envc++] = l + 4;
		else if (!strncmp(l, "UID ", 4))
			uid = atoi(l + 4);
		else if (!strncmp(l, "TIMEOUT ", 8))
			timeout = atoi(l + 8);
		else if (argc < MAXARG)
			argv[argc++] = l;
		n++;
	}
	fclose(f);
	argv[argc] = NULL;
	envp[envc] = NULL;
	int status = -1;
	unlink(out);
	if (argc > 0) {
		posix_spawn_file_actions_t fa;
		posix_spawn_file_actions_init(&fa);
		posix_spawn_file_actions_addopen(&fa, 1, out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		posix_spawn_file_actions_adddup2(&fa, 1, 2);
		posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
		pid_t pid = -1;
		int err;
		if (uid >= 0) {
			/* drop privileges in a child first (posix_spawn has no uid attribute on iOS) */
			pid = fork();
			if (pid == 0) {
				setgid((gid_t)uid);
				setuid((uid_t)uid);
				int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
				if (fd >= 0) {
					dup2(fd, 1);
					dup2(fd, 2);
				}
				execve(argv[0], argv, envp);
				_exit(127);
			}
			err = pid < 0 ? errno : 0;
		} else {
			err = posix_spawn(&pid, argv[0], &fa, NULL, argv, envp);
		}
		posix_spawn_file_actions_destroy(&fa);
		if (err) {
			FILE *o = fopen(out, "a");
			if (o) {
				fprintf(o, "cmdd: cannot run %s: %s\n", argv[0], strerror(err));
				fclose(o);
			}
			status = 127 << 8;
		} else {
			time_t t0 = time(NULL);
			for (;;) {
				pid_t r = waitpid(pid, &status, WNOHANG);
				if (r == pid || (r < 0 && errno != EINTR))
					break;
				if (time(NULL) - t0 > timeout) {
					kill(pid, SIGKILL);
					waitpid(pid, &status, 0);
					break;
				}
				usleep(50000);
			}
		}
	}
	snprintf(tmp, sizeof(tmp), DIR_CMD "/%s.rc.tmp", name);
	FILE *r = fopen(tmp, "w");
	if (r) {
		fprintf(r, "%d\n", WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0));
		fclose(r);
		rename(tmp, rc);
	}
	unlink(run);
}

/*
 * The kernel's message buffer, 90 s into each boot, to /var/local/dmesg-boot.txt (the previous boot's to
 * dmesg-boot.prev.txt): the USB live log is gone when USB does not come up, and the on-disk ring only
 * holds the current boot.
 */
#include <sys/sysctl.h>
static void save_msgbuf(void)
{
	sleep(90);
	size_t len = 0;
	if (sysctlbyname("kern.msgbuf", NULL, &len, NULL, 0) != 0 || len == 0)
		return;
	char *buf = malloc(len + 1);
	if (!buf || sysctlbyname("kern.msgbuf", buf, &len, NULL, 0) != 0)
		return;
	rename("/var/local/dmesg-boot.txt", "/var/local/dmesg-boot.prev.txt");
	FILE *f = fopen("/var/local/dmesg-boot.txt", "w");
	if (f) {
		/* the buffer is a ring with NUL padding: write every non-NUL byte */
		for (size_t i = 0; i < len; i++)
			if (buf[i])
				fputc(buf[i], f);
		fclose(f);
	}
	free(buf);
}

int main(void)
{
	if (fork() == 0) {
		save_msgbuf();
		_exit(0);
	}
	mkdir(DIR_CMD, 0700);
	chmod(DIR_CMD, 0700);
	signal(SIGPIPE, SIG_IGN);
	for (;;) {
		DIR *d = opendir(DIR_CMD);
		if (d) {
			struct dirent *e;
			char names[32][256];
			int k = 0;
			while ((e = readdir(d)) && k < 32) {
				size_t l = strlen(e->d_name);
				if (l > 4 && l < 250 && !strcmp(e->d_name + l - 4, ".cmd")) {
					memcpy(names[k], e->d_name, l - 4);
					names[k][l - 4] = 0;
					k++;
				}
			}
			closedir(d);
			for (int i = 0; i < k; i++)
				run(names[i]);
		}
		sleep(1);
	}
}
