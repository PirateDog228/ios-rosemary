/*
 * libsbhook: inserted into SpringBoard. It does two things.
 *
 * 1. A picture for the wallpaper. PosterBoard's posters draw black here (there is no Metal), so the space behind the
 *    icons is black. /var/mobile/wallpaper.jpg (or wallpaper=PATH in /usr/local/etc/sbhook.conf) is put as an image
 *    view into every SpringBoard window whose class name has "Wallpaper" in it (the windows are listed in
 *    /var/tmp/sbhook.log, so that the right one can be picked: wallpaper_class=WORD in the conf takes another word).
 *
 * 2. Diagnostics for the SBApplication assertion. SpringBoard aborts in -[SBMainDisplaySceneManager _addScene:]
 * ("Unable to fetch SBApplication for scene: ...PosterBoard-<uuid>"). This says, in /var/tmp/sbhook.log:
 *   - for each scene given to _addScene: its identifier, the bundle identifier of its client process, and what
 *     -[SBApplicationController applicationWithBundleIdentifier:] makes of that identifier;
 *   - once, the bundle identifiers SBApplicationController knows (is com.apple.PosterBoard among them?);
 *   - every application lookup of a "Poster" identifier and what it gave.
 * sbhook=catch in /usr/local/etc/sbhook.conf (first line) makes _addScene: survive: the exception is caught and the
 * scene is left out (an experiment: SpringBoard stays up, the scene is not shown).
 */
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#import <objc/message.h>
#import <mach-o/dyld.h>
#import <dispatch/dispatch.h>
#import <dlfcn.h>
#import <fcntl.h>
#import <unistd.h>
#import <stdarg.h>
#import <stdio.h>
#import <string.h>

static int catch_mode;

static void say(const char *fmt, ...)
{
	int fd = open("/var/tmp/sbhook.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
	char b[2000];
	va_list ap;
	int n;

	if (fd < 0) return;
	n = snprintf(b, sizeof b, "sbhook[%d]: ", getpid());
	va_start(ap, fmt);
	n += vsnprintf(b + n, sizeof b - n, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof b - 2) n = (int)sizeof b - 2;
	b[n++] = '\n';
	write(fd, b, n);
	close(fd);
}

static const char *cs(id o)
{
	const char *r = "(nil)";

	if (o) {
		@try { r = [[o description] UTF8String]; } @catch (id e) { r = "(description threw)"; }
	}
	return r ? r : "(null)";
}

static id send0(id o, const char *sel)
{
	SEL s = sel_registerName(sel);

	if (!o || ![o respondsToSelector:s]) return nil;
	@try { return ((id (*)(id, SEL))objc_msgSend)(o, s); } @catch (id e) { return nil; }
}

static id app_controller(void)
{
	Class c = objc_getClass("SBApplicationController");
	return c ? send0((id)c, "sharedInstance") : nil;
}

static id lookup_app(id ctl, id bid)
{
	SEL s = sel_registerName("applicationWithBundleIdentifier:");

	if (!ctl || !bid || ![ctl respondsToSelector:s]) return nil;
	@try { return ((id (*)(id, SEL, id))objc_msgSend)(ctl, s, bid); } @catch (id e) { return nil; }
}

static int listed;
static void list_apps_once(id ctl)
{
	id apps, ids;

	if (listed || !ctl) return;
	listed = 1;
	apps = send0(ctl, "allApplications");
	if (!apps) { say("applications: SBApplicationController has no allApplications (%s)", cs(ctl)); return; }
	@try {
		ids = [apps valueForKey:@"bundleIdentifier"];
		say("applications: %lu known; PosterBoard among them: %s", (unsigned long)[ids count],
		    [ids containsObject:@"com.apple.PosterBoard"] ? "yes" : "NO");
		NSString *joined = [[ids sortedArrayUsingSelector:@selector(compare:)] componentsJoinedByString:@" "];
		for (NSUInteger i = 0; i < [joined length]; i += 900) {
			say("applications[%lu]: %s", (unsigned long)i,
			    [[joined substringWithRange:NSMakeRange(i, MIN((NSUInteger)900, [joined length] - i))] UTF8String]);
		}
	} @catch (id e) {
		say("applications: listing threw %s", cs(e));
	}
}

/* ---- -[SBApplicationController applicationWithBundleIdentifier:] ---- */
static id (*orig_lookup)(id, SEL, id);
static id my_lookup(id self, SEL _cmd, id bid)
{
	id r = orig_lookup(self, _cmd, bid);

	if (bid && strstr(cs(bid), "Poster")) {
		static int n;
		if (n++ < 30) say("applicationWithBundleIdentifier:%s -> %s", cs(bid), r ? "an SBApplication" : "nil");
	}
	return r;
}

/* ---- -[SBMainDisplaySceneManager _addScene:] ---- */
static void (*orig_addScene)(id, SEL, id);
static void my_addScene(id self, SEL _cmd, id scene)
{
	id ctl = app_controller();
	id proc = send0(scene, "clientProcess");
	id bid = send0(proc, "bundleIdentifier");
	id sid = send0(scene, "identifier");
	id app = lookup_app(ctl, bid);

	say("_addScene: scene %s, client process %s (%s), bundle id %s -> application %s", cs(sid), proc ? class_getName(object_getClass(proc)) : "(nil)",
	    cs(proc), cs(bid), app ? "found" : "NOT FOUND");
	list_apps_once(ctl);
	if (catch_mode) {
		@try {
			orig_addScene(self, _cmd, scene);
		} @catch (id e) {
			say("_addScene: caught %s: %s; scene left out", [[e name] UTF8String], [[e reason] UTF8String]);
		}
	} else {
		orig_addScene(self, _cmd, scene);
	}
}

/* ---- the wallpaper picture ---- */
static char wp_path[512] = "/var/mobile/wallpaper.jpg";
static char wp_word[64] = "Wallpaper";
static UIImage *wp_image;
static int wp_passes;
#define WP_TAG 0x57504b52

static void wp_pass(void)
{
	NSArray *wins = nil;
	SEL all = NSSelectorFromString(@"allWindowsIncludingInternalWindows:onlyVisibleWindows:");

	wp_passes++;
	if ([UIWindow respondsToSelector:all]) {
		@try { wins = ((id (*)(id, SEL, BOOL, BOOL))objc_msgSend)((id)[UIWindow class], all, YES, NO); } @catch (id e) {}
	}
	if (!wins) wins = [[UIApplication sharedApplication] windows];
	if (wp_passes <= 3) say("pass %d: %lu windows", wp_passes, (unsigned long)[wins count]);
	for (UIWindow *w in wins) {
		const char *cn = class_getName(object_getClass(w));
		BOOL isWall = strstr(cn, wp_word) != NULL;

		if (wp_passes <= 3) {
			CGRect f = w.frame;
			say("   window %s level %.1f frame %.0fx%.0f hidden %d alpha %.2f subviews %lu%s", cn, (double)w.windowLevel,
			    (double)f.size.width, (double)f.size.height, (int)w.hidden, (double)w.alpha, (unsigned long)[w.subviews count],
			    isWall ? "  <- picture goes here" : "");
		}
		if (!isWall || !wp_image) continue;
		UIView *have = [w viewWithTag:WP_TAG];
		if (have) {
			if (have.superview == w && [w.subviews lastObject] != have) [w bringSubviewToFront:have];
			continue;
		}
		UIImageView *iv = [[UIImageView alloc] initWithImage:wp_image];
		iv.tag = WP_TAG;
		iv.frame = w.bounds;
		iv.contentMode = UIViewContentModeScaleAspectFill;
		iv.clipsToBounds = YES;
		iv.userInteractionEnabled = NO;
		iv.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
		[w addSubview:iv];
		[iv release];
		say("picture put into %s (%.0fx%.0f)", cn, (double)w.bounds.size.width, (double)w.bounds.size.height);
	}
}

static void wp_start(void)
{
	wp_image = [[UIImage imageWithContentsOfFile:[NSString stringWithUTF8String:wp_path]] retain];
	if (!wp_image) {
		say("wallpaper: cannot read %s (put a picture there)", wp_path);
	} else {
		say("wallpaper: %s is %.0fx%.0f", wp_path, (double)wp_image.size.width, (double)wp_image.size.height);
	}
	dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
	dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), 2 * NSEC_PER_SEC, NSEC_PER_SEC / 2);
	dispatch_source_set_event_handler(timer, ^{ @try { wp_pass(); } @catch (id e) { say("wallpaper pass threw %s", cs(e)); } });
	dispatch_resume(timer);
}

static int did_scene, did_lookup;
static void try_hooks(void)
{
	Class c;
	Method m;

	if (!did_scene && (c = objc_getClass("SBMainDisplaySceneManager"))) {
		m = class_getInstanceMethod(c, sel_registerName("_addScene:"));
		if (m) {
			orig_addScene = (void (*)(id, SEL, id))method_getImplementation(m);
			method_setImplementation(m, (IMP)my_addScene);
			say("hooked -[SBMainDisplaySceneManager _addScene:]%s", catch_mode ? " (catching)" : "");
		} else {
			say("SBMainDisplaySceneManager has no _addScene:");
		}
		did_scene = 1;
	}
	if (!did_lookup && (c = objc_getClass("SBApplicationController"))) {
		m = class_getInstanceMethod(c, sel_registerName("applicationWithBundleIdentifier:"));
		if (m) {
			orig_lookup = (id (*)(id, SEL, id))method_getImplementation(m);
			method_setImplementation(m, (IMP)my_lookup);
			say("hooked -[SBApplicationController applicationWithBundleIdentifier:]");
		}
		did_lookup = 1;
	}
}

static void image_added(const struct mach_header *mh, intptr_t slide)
{
	(void)mh; (void)slide;
	try_hooks();
}

__attribute__((constructor)) static void init(void)
{
	FILE *f = fopen("/usr/local/etc/sbhook.conf", "r");
	char line[128] = "";

	if (f) {
		while (fgets(line, sizeof line, f)) {
			line[strcspn(line, "\r\n")] = 0;
			if (strstr(line, "catch")) catch_mode = 1;
			if (!strncmp(line, "wallpaper=", 10)) snprintf(wp_path, sizeof wp_path, "%s", line + 10);
			if (!strncmp(line, "wallpaper_class=", 16)) snprintf(wp_word, sizeof wp_word, "%s", line + 16);
		}
		fclose(f);
	}
	say("loaded (catch %d, picture %s, windows with \"%s\")", catch_mode, wp_path, wp_word);
	dispatch_async(dispatch_get_main_queue(), ^{ wp_start(); });
	_dyld_register_func_for_add_image(image_added);
	try_hooks();
	for (int i = 1; i <= 3; i++) {
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)i * 4 * NSEC_PER_SEC), dispatch_get_global_queue(0, 0), ^{ try_hooks(); });
	}
}
