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
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <ucontext.h>

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

extern const void *_dyld_get_shared_cache_range(size_t *length);

static void where(const char *label, uintptr_t a)
{
	Dl_info i;
	char b[500];

	if (dladdr((void *)a, &i) && i.dli_fname) {
		snprintf(b, sizeof b, "%s %#lx = %s + %#lx (%s %#lx)", label, (unsigned long)a, i.dli_sname ? i.dli_sname : "?",
		    (unsigned long)(a - (uintptr_t)i.dli_saddr), i.dli_fname, (unsigned long)(a - (uintptr_t)i.dli_fbase));
	} else {
		snprintf(b, sizeof b, "%s %#lx = unknown", label, (unsigned long)a);
	}
	say("crash", b);
}

/* a fault in backboardd: say where, and let the default action go on (SA_RESETHAND) */
static void crashed(int sig, siginfo_t *info, void *ctx)
{
	ucontext_t *uc = ctx;
	uintptr_t *fp;
	char b[100];

	snprintf(b, sizeof b, "signal %d, address %p", sig, info->si_addr);
	say("crash", b);
	where("pc", (uintptr_t)__darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss));
	where("lr", (uintptr_t)__darwin_arm_thread_state64_get_lr(uc->uc_mcontext->__ss));
	fp = (uintptr_t *)__darwin_arm_thread_state64_get_fp(uc->uc_mcontext->__ss);
	for (int n = 0; n < 14 && fp != NULL && ((uintptr_t)fp & 7) == 0 && (uintptr_t)fp > 0x100000000UL; n++) {
		where("frame", fp[1]);
		fp = (uintptr_t *)fp[0];
	}
}

__attribute__((constructor)) static void load(void)
{
	void *h;
	size_t length = 0;
	const void *cache = _dyld_get_shared_cache_range(&length);
	struct sigaction sa;
	char b[100];

	say("start", NULL);
	snprintf(b, sizeof b, "shared cache at %p, %zu bytes", cache, length);
	say(b, NULL);
	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = crashed;
	sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	h = dlopen("/System/Library/Frameworks/OpenGLES.framework/OpenGLES", RTLD_NOW);
	say(h ? "OpenGLES loaded" : "OpenGLES not loaded", h ? NULL : dlerror());
	say(objc_getClass("EAGLContext") ? "EAGLContext is there" : "EAGLContext is NOT there", NULL);
	h = dlopen("/redwood/lib/libredwood-gl.dylib", RTLD_NOW);
	say(h ? "libredwood-gl loaded" : "libredwood-gl not loaded", h ? NULL : dlerror());
}
