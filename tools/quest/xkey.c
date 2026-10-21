// xkey: minimal xdotool replacement for headless tests.
// usage: xkey down|up|tap KEYSYM [holdsecs]
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: xkey down|up|tap KEYSYM [secs]\n"); return 2; }
	Display *d = XOpenDisplay(NULL);
	if (!d) { fprintf(stderr, "no display\n"); return 1; }
	if (!strcmp(argv[1], "move") || !strcmp(argv[1], "click"))
	{
		XTestFakeMotionEvent(d, -1, atoi(argv[2]), argc > 3 ? atoi(argv[3]) : 0, 0);
		if (!strcmp(argv[1], "click")) { XTestFakeButtonEvent(d, 1, True, 0); XFlush(d); usleep(80000); XTestFakeButtonEvent(d, 1, False, 0); }
		XFlush(d); XCloseDisplay(d); return 0;
	}
	KeySym ks = XStringToKeysym(argv[2]);
	if (ks == NoSymbol) { fprintf(stderr, "bad keysym %s\n", argv[2]); return 1; }
	KeyCode kc = XKeysymToKeycode(d, ks);
	double hold = argc > 3 ? atof(argv[3]) : 0.1;
	if (!strcmp(argv[1], "down") || !strcmp(argv[1], "tap")) { XTestFakeKeyEvent(d, kc, True, 0); XFlush(d); }
	if (!strcmp(argv[1], "tap")) usleep((useconds_t)(hold * 1e6));
	if (!strcmp(argv[1], "up") || !strcmp(argv[1], "tap")) { XTestFakeKeyEvent(d, kc, False, 0); XFlush(d); }
	XCloseDisplay(d);
	return 0;
}
