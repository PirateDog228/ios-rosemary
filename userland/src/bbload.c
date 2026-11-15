/*
 * libbbload: inserted into backboardd (bbwrap). Loads OpenGLES first, so that the class EAGLContext is there,
 * and then libredwood-gl.dylib, whose constructor replaces EAGLContext's methods. Says what happened in
 * /var/tmp/bbload.log.
 */
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <objc/runtime.h>

static void say(const char *what, const char *detail)
{
	int fd = open("/var/tmp/bbload.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
	char b[600];
	int n;
	if (fd < 0) return;
	n = snprintf(b, sizeof b, "bbload[%d]: %s%s%s\n", getpid(), what, detail ? ": " : "", detail ? detail : "");
	write(fd, b, n);
	close(fd);
}

__attribute__((constructor)) static void load(void)
{
	void *h;
	say("start", NULL);
	h = dlopen("/System/Library/Frameworks/OpenGLES.framework/OpenGLES", RTLD_NOW);
	say(h ? "OpenGLES loaded" : "OpenGLES not loaded", h ? NULL : dlerror());
	say(objc_getClass("EAGLContext") ? "EAGLContext is there" : "EAGLContext is NOT there", NULL);
	h = dlopen("/redwood/lib/libredwood-gl.dylib", RTLD_NOW);
	say(h ? "libredwood-gl loaded" : "libredwood-gl not loaded", h ? NULL : dlerror());
}
