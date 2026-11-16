/*
 * bbwrap: stand-in for /usr/libexec/backboardd in its launchd plist. It sets DYLD_INSERT_LIBRARIES (read
 * from /usr/local/etc/bbwrap.conf, one line: the dylib paths separated by ':'; nothing if the file is
 * missing) and runs the real backboardd, which it keeps at /usr/libexec/backboardd.real. What it did goes
 * to /var/tmp/bbwrap.log, where a launchd job can write; backboardd's stderr goes to /var/tmp/redwood-gl.log.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

static void say(const char *fmt, const char *a, int e)
{
	int fd = open("/var/tmp/bbwrap.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
	char b[512];
	int n;
	if (fd < 0) return;
	n = snprintf(b, sizeof b, fmt, a, e);
	write(fd, b, n);
	close(fd);
}

int main(int argc, char **argv)
{
	char line[1024] = "";
	FILE *f = fopen("/usr/local/etc/bbwrap.conf", "r");
	(void)argc;
	if (f) {
		if (fgets(line, sizeof line, f)) line[strcspn(line, "\r\n")] = 0;
		fclose(f);
	}
	/* libredwood-gl and the drm layer write what they think to stderr: here, a file, for the run to be read afterwards */
	{
		int fd = open("/var/tmp/redwood-gl.log", O_WRONLY | O_CREAT | O_APPEND, 0666);

		if (fd >= 0) {
			dup2(fd, 2);
			if (fd != 2) close(fd);
		}
	}
	say("bbwrap: wants insert '%s', uid %d\n", line, (int)getuid());
	if (line[0]) setenv("DYLD_INSERT_LIBRARIES", line, 1);
	argv[0] = "/usr/libexec/backboardd.real";
	execv(argv[0], argv);
	say("bbwrap: execv failed: %s (errno %d)\n", strerror(errno), errno);
	return 1;
}
