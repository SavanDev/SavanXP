/* L5 fixture "xclient": the first, small X client, against the pinned
 * distribution's Xlib.
 *
 * Opens a 200x150 window titled "xclient", fills a rectangle and a circle on
 * every Expose, reports the pointer's root position whenever it changes
 * after the first Expose (polled with XQueryPointer, as Xeyes does without
 * XInput 2), reports
 * resizes, and exits cleanly when the window manager sends WM_DELETE_WINDOW.
 * Every line it prints is a fact the reference run can compare.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <poll.h>

int main(void)
{
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "xclient: cannot open display\n");
        return 1;
    }
    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);
    Window win = XCreateSimpleWindow(dpy, root, 0, 0, 200, 150, 0,
                                     BlackPixel(dpy, screen), WhitePixel(dpy, screen));
    XStoreName(dpy, win, "xclient");
    Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);
    XSelectInput(dpy, win, ExposureMask | StructureNotifyMask);
    GC gc = XCreateGC(dpy, win, 0, NULL);
    XSetForeground(dpy, gc, BlackPixel(dpy, screen));
    XMapWindow(dpy, win);
    XFlush(dpy);
    printf("window mapped\n");
    fflush(stdout);

    int width = 200, height = 150;
    int last_x = -100000, last_y = -100000;
    int exposed = 0;
    int fd = ConnectionNumber(dpy);
    for (;;) {
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.type == Expose && ev.xexpose.count == 0) {
                XFillRectangle(dpy, win, gc, 10, 10, 60, 40);
                XFillArc(dpy, win, gc, width - 80, height - 80, 60, 60, 0, 360 * 64);
                printf("expose %dx%d\n", width, height);
                exposed = 1;
            } else if (ev.type == ConfigureNotify &&
                       (ev.xconfigure.width != width || ev.xconfigure.height != height)) {
                width = ev.xconfigure.width;
                height = ev.xconfigure.height;
                printf("resize %dx%d\n", width, height);
            } else if (ev.type == ClientMessage &&
                       (Atom)ev.xclient.data.l[0] == wm_delete) {
                printf("delete requested\n");
                XFreeGC(dpy, gc);
                XDestroyWindow(dpy, win);
                XCloseDisplay(dpy);
                printf("closed\n");
                return 0;
            }
            fflush(stdout);
        }

        Window r, child;
        int rx, ry, wx, wy;
        unsigned int mask;
        /* The pointer is reported once the window is drawn, so the order
         * of the lines does not depend on timing. */
        if (exposed && XQueryPointer(dpy, win, &r, &child, &rx, &ry, &wx, &wy, &mask) &&
            (rx != last_x || ry != last_y)) {
            last_x = rx;
            last_y = ry;
            printf("pointer %d %d window %d %d\n", rx, ry, wx, wy);
            fflush(stdout);
        }

        /* poll, as Xlib itself uses, so the fixture adds no call of its own. */
        struct pollfd pfd = {fd, POLLIN, 0};
        poll(&pfd, 1, 50);
    }
}
