/*
 * libpbhook: inserted into PosterBoard (DYLD_INSERT_LIBRARIES). PosterBoard aborts in
 * -[PBFHomeScreenPreviewImageBuilder _imageForView:] when +[NSDictionary dictionaryWithObjects:forKeys:count:]
 * is given a nil object. This says, in /var/tmp/pbhook.log:
 *   - whether Metal has a device here (MTLCreateSystemDefaultDevice, MTLCopyAllDevices) and a command queue;
 *   - every exception thrown (name, reason, return addresses), which names the nil object's index;
 *   - what _imageForView: was called with and what it came to.
 * pbhook=catch in /usr/local/etc/pbhook.conf (first line) also makes _imageForView: survive: an exception
 * in it is caught and an empty image is returned instead (an experiment: the preview is then blank).
 */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#import <mach-o/dyld.h>
#import <dlfcn.h>
#import <fcntl.h>
#import <unistd.h>
#import <stdarg.h>
#import <execinfo.h>
#import <stdio.h>
#import <alloca.h>
#import <string.h>

static void say(const char *fmt, ...)
{
	int fd = open("/var/tmp/pbhook.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
	char b[1500];
	va_list ap;
	int n;

	if (fd < 0) return;
	n = snprintf(b, sizeof b, "pbhook[%d]: ", getpid());
	va_start(ap, fmt);
	n += vsnprintf(b + n, sizeof b - n, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof b - 2) n = (int)sizeof b - 2;
	b[n++] = '\n';
	write(fd, b, n);
	close(fd);
}

static int catch_mode;

/* ---- exceptions ---- */
extern void objc_exception_throw(id exception);
static void my_throw(id exception)
{
	void *bt[12];
	int n = backtrace(bt, 12);
	NSString *name = @"?", *reason = @"?";

	@try {
		name = [exception name];
		reason = [exception reason];
	} @catch (id e) {}
	say("EXCEPTION %s: %s", [name UTF8String], [reason UTF8String]);
	for (int i = 0; i < n; i++) {
		Dl_info d;
		if (dladdr(bt[i], &d) && d.dli_fname) {
			say("   #%d %s + %lu (%s)", i, d.dli_sname ? d.dli_sname : "?", (unsigned long)((char *)bt[i] - (char *)(d.dli_saddr ? d.dli_saddr : d.dli_fbase)), d.dli_fname);
		}
	}
	objc_exception_throw(exception);
}
__attribute__((used)) static struct { const void *replacement, *replacee; } interposers[]
	__attribute__((section("__DATA,__interpose"))) = { { (const void *)my_throw, (const void *)objc_exception_throw } };

/* ---- +[NSDictionary dictionaryWithObjects:forKeys:count:] with a nil object ---- */
static id (*orig_dwo)(id, SEL, const id *, const id *, NSUInteger);
static int nil_logged;

static id my_dwo(id self, SEL _cmd, const id *objects, const id *keys, NSUInteger count)
{
	NSUInteger i, bad = 0;

	for (i = 0; i < count; i++) {
		if (objects[i] == nil) bad++;
	}
	if (bad) {
		if (nil_logged++ < 20) {
			void *bt[10];
			int n = backtrace(bt, 10);

			for (i = 0; i < count; i++) {
				if (objects[i] == nil) {
					say("NIL OBJECT at index %lu of %lu, key: %s", (unsigned long)i, (unsigned long)count,
					    keys[i] ? [[keys[i] description] UTF8String] : "(nil)");
				}
			}
			for (int k = 0; k < n; k++) {
				Dl_info d;
				if (dladdr(bt[k], &d) && d.dli_fname) {
					say("   #%d %s + %lu (%s)", k, d.dli_sname ? d.dli_sname : "?",
					    (unsigned long)((char *)bt[k] - (char *)(d.dli_saddr ? d.dli_saddr : d.dli_fbase)), d.dli_fname);
				}
			}
		}
		if (catch_mode) {   /* leave the pairs with a nil object out */
			id *o = (id *)alloca(count * sizeof(id));
			id *kk = (id *)alloca(count * sizeof(id));
			NSUInteger m = 0;

			for (i = 0; i < count; i++) {
				if (objects[i] != nil) { o[m] = objects[i]; kk[m] = keys[i]; m++; }
			}
			return orig_dwo(self, _cmd, o, kk, m);
		}
	}
	return orig_dwo(self, _cmd, objects, keys, count);
}

/* ---- _imageForView: ---- */
static id (*orig_imageForView)(id, SEL, id);

static id my_imageForView(id self, SEL _cmd, id view)
{
	id result = nil;

	say("_imageForView: view %s %s", view ? class_getName(object_getClass(view)) : "(nil)",
	    view ? [[view description] UTF8String] : "");
	if (catch_mode) {
		@try {
			result = orig_imageForView(self, _cmd, view);
		} @catch (id e) {
			say("_imageForView: caught %s: %s, answering with an empty image", [[e name] UTF8String], [[e reason] UTF8String]);
			Class img = objc_getClass("UIImage");
			result = img ? ((id (*)(id, SEL))objc_msgSend)((id)img, sel_registerName("new")) : nil;
		}
	} else {
		result = orig_imageForView(self, _cmd, view);
	}
	say("_imageForView: -> %s", result ? class_getName(object_getClass(result)) : "(nil)");
	return result;
}

static int swizzled;
static void try_swizzle(void)
{
	Class c;
	Method m;

	if (swizzled) return;
	c = objc_getClass("PBFHomeScreenPreviewImageBuilder");
	if (!c) return;
	m = class_getInstanceMethod(c, sel_registerName("_imageForView:"));
	if (!m) { say("PBFHomeScreenPreviewImageBuilder has no _imageForView:"); swizzled = 1; return; }
	orig_imageForView = (id (*)(id, SEL, id))method_getImplementation(m);
	method_setImplementation(m, (IMP)my_imageForView);
	swizzled = 1;
	say("hooked -[PBFHomeScreenPreviewImageBuilder _imageForView:]%s", catch_mode ? " (catching)" : "");
}

static void image_added(const struct mach_header *mh, intptr_t slide)
{
	(void)mh; (void)slide;
	try_swizzle();
}

/* ---- Metal ---- */
static void metal_report(void)
{
	void *h = dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_NOW);
	id (*create)(void);
	id (*copyAll)(void);

	if (!h) { say("Metal: dlopen failed: %s", dlerror()); return; }
	create = (id (*)(void))dlsym(h, "MTLCreateSystemDefaultDevice");
	copyAll = (id (*)(void))dlsym(h, "MTLCopyAllDevices");
	id dev = create ? create() : nil;
	id all = copyAll ? copyAll() : nil;

	say("Metal: MTLCreateSystemDefaultDevice -> %s", dev ? [[dev description] UTF8String] : "nil");
	say("Metal: MTLCopyAllDevices -> %lu device(s)", all ? (unsigned long)[(NSArray *)all count] : 0UL);
	if (dev) {
		id q = ((id (*)(id, SEL))objc_msgSend)(dev, sel_registerName("newCommandQueue"));
		say("Metal: newCommandQueue -> %s", q ? "a queue" : "nil");
	}
}

__attribute__((constructor)) static void init(void)
{
	FILE *f = fopen("/usr/local/etc/pbhook.conf", "r");
	char line[128] = "";

	if (f) {
		if (fgets(line, sizeof line, f)) catch_mode = strstr(line, "catch") != NULL;
		fclose(f);
	}
	say("loaded (catch %d)", catch_mode);
	metal_report();
	{
		Method m = class_getClassMethod(objc_getClass("NSDictionary"), sel_registerName("dictionaryWithObjects:forKeys:count:"));

		if (m) {
			orig_dwo = (id (*)(id, SEL, const id *, const id *, NSUInteger))method_getImplementation(m);
			method_setImplementation(m, (IMP)my_dwo);
			say("hooked +[NSDictionary dictionaryWithObjects:forKeys:count:]");
		}
	}
	_dyld_register_func_for_add_image(image_added);   /* (also called for the images already there) */
	try_swizzle();
}
