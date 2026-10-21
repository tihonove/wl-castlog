// curlog — logs pointer movement as JSONL until stopped (SIGINT/SIGTERM).
//
//   curlog -o DP-2 [-f out.jsonl]          whole output
//   curlog -g "1920,0 800x600" [-f ...]    region in layout coordinates, as from slurp
//
// The position comes from the compositor itself through an ext-image-copy-capture
// cursor session, which needs no access to /dev/input. Wayland doesn't show clicks
// to other clients, so those are read from /dev/input — from whichever devices we
// can open (needs access: the input group or a uaccess rule for mice). Touchpad
// taps don't show up here: libinput turns them into clicks inside the compositor.
//
// Time is CLOCK_REALTIME in microseconds: the same clock as evdev and bash's
// $EPOCHREALTIME. Coordinates are pixels of the recorded region (as in a
// wf-recorder video made with the same -o/-g).
//
// Format: the first line is a header, then one event per line:
//   {"v":1,"clock":"realtime_us","t":…,"output":"DP-2","scale":1,"region":[x,y,w,h],"click_devices":2}
//   {"t":…,"x":812,"y":440}
//   {"t":…,"ev":"down","btn":"left"} / {"t":…,"ev":"up","btn":"left"}
//   {"t":…,"ev":"enter"} / {"t":…,"ev":"leave"} — the pointer moved to another output
//   {"t":…,"ev":"hotspot","x":…,"y":…}
//   {"t":…,"ev":"stop"}

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <string.h>
#include <sys/signalfd.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "ext-image-capture-source-v1.h"
#include "ext-image-copy-capture-v1.h"
#include "xdg-output-unstable-v1.h"

#define MAX_OUTPUTS 8
#define MAX_INPUTS 32

struct output {
	struct wl_output *wl;
	char name[64];
	int32_t x, y, w, h; // logical, from xdg-output
	int32_t scale;
};

static struct output outputs[MAX_OUTPUTS];
static int noutputs;
static struct wl_seat *seat;
static struct zxdg_output_manager_v1 *xdg_output_manager;
static struct ext_output_image_capture_source_manager_v1 *source_manager;
static struct ext_image_copy_capture_manager_v1 *capture_manager;

static FILE *out;
// Region offset within the output, in buffer pixels — subtracted from positions.
static int32_t off_x, off_y;

static int64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void die(const char *msg)
{
	fprintf(stderr, "curlog: %s\n", msg);
	exit(1);
}

static void output_scale(void *data, struct wl_output *wl, int32_t scale)
{
	((struct output *)data)->scale = scale;
}

static void output_name(void *data, struct wl_output *wl, const char *name)
{
	snprintf(((struct output *)data)->name, sizeof ((struct output *)data)->name, "%s", name);
}

static void noop() {}

static const struct wl_output_listener output_listener = {
	.geometry = (void *)noop,
	.mode = (void *)noop,
	.done = (void *)noop,
	.scale = output_scale,
	.name = output_name,
	.description = (void *)noop,
};

static void xdg_output_position(void *data, struct zxdg_output_v1 *xo, int32_t x, int32_t y)
{
	((struct output *)data)->x = x;
	((struct output *)data)->y = y;
}

static void xdg_output_size(void *data, struct zxdg_output_v1 *xo, int32_t w, int32_t h)
{
	((struct output *)data)->w = w;
	((struct output *)data)->h = h;
}

static const struct zxdg_output_v1_listener xdg_output_listener = {
	.logical_position = xdg_output_position,
	.logical_size = xdg_output_size,
	.done = (void *)noop,
	.name = (void *)noop,
	.description = (void *)noop,
};

static void registry_global(void *data, struct wl_registry *reg, uint32_t id,
		const char *iface, uint32_t version)
{
	if (!strcmp(iface, wl_output_interface.name) && noutputs < MAX_OUTPUTS) {
		struct output *o = &outputs[noutputs++];
		o->scale = 1;
		o->wl = wl_registry_bind(reg, id, &wl_output_interface, 4);
		wl_output_add_listener(o->wl, &output_listener, o);
	} else if (!strcmp(iface, wl_seat_interface.name)) {
		seat = wl_registry_bind(reg, id, &wl_seat_interface, 1);
	} else if (!strcmp(iface, zxdg_output_manager_v1_interface.name)) {
		xdg_output_manager = wl_registry_bind(reg, id, &zxdg_output_manager_v1_interface, 2);
	} else if (!strcmp(iface, ext_output_image_capture_source_manager_v1_interface.name)) {
		source_manager = wl_registry_bind(reg, id,
				&ext_output_image_capture_source_manager_v1_interface, 1);
	} else if (!strcmp(iface, ext_image_copy_capture_manager_v1_interface.name)) {
		capture_manager = wl_registry_bind(reg, id,
				&ext_image_copy_capture_manager_v1_interface, 1);
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = (void *)noop,
};

static void cursor_enter(void *data, struct ext_image_copy_capture_cursor_session_v1 *cs)
{
	fprintf(out, "{\"t\":%ld,\"ev\":\"enter\"}\n", now_us());
}

static void cursor_leave(void *data, struct ext_image_copy_capture_cursor_session_v1 *cs)
{
	fprintf(out, "{\"t\":%ld,\"ev\":\"leave\"}\n", now_us());
}

static void cursor_position(void *data, struct ext_image_copy_capture_cursor_session_v1 *cs,
		int32_t x, int32_t y)
{
	fprintf(out, "{\"t\":%ld,\"x\":%d,\"y\":%d}\n", now_us(), x - off_x, y - off_y);
}

static void cursor_hotspot(void *data, struct ext_image_copy_capture_cursor_session_v1 *cs,
		int32_t x, int32_t y)
{
	fprintf(out, "{\"t\":%ld,\"ev\":\"hotspot\",\"x\":%d,\"y\":%d}\n", now_us(), x, y);
}

static const struct ext_image_copy_capture_cursor_session_v1_listener cursor_listener = {
	.enter = cursor_enter,
	.leave = cursor_leave,
	.position = cursor_position,
	.hotspot = cursor_hotspot,
};

// Devices with a left button: mice, touchpad and trackpoint buttons.
static int open_click_devices(int *fds)
{
	int n = 0;
	DIR *dir = opendir("/dev/input");
	if (!dir)
		return 0;
	struct dirent *de;
	while ((de = readdir(dir)) && n < MAX_INPUTS) {
		if (strncmp(de->d_name, "event", 5))
			continue;
		char path[300];
		snprintf(path, sizeof path, "/dev/input/%s", de->d_name);
		int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		unsigned long keys[KEY_MAX / (8 * sizeof(long)) + 1] = { 0 };
		int clock = CLOCK_REALTIME;
		if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) < 0
				|| !(keys[BTN_LEFT / (8 * sizeof(long))] >> (BTN_LEFT % (8 * sizeof(long))) & 1)
				|| ioctl(fd, EVIOCSCLOCKID, &clock) < 0) {
			close(fd);
			continue;
		}
		fds[n++] = fd;
	}
	closedir(dir);
	return n;
}

static void read_clicks(int fd)
{
	struct input_event ev[64];
	ssize_t len;
	while ((len = read(fd, ev, sizeof ev)) > 0) {
		for (size_t i = 0; i < len / sizeof *ev; i++) {
			const char *btn;
			if (ev[i].type != EV_KEY || ev[i].value > 1)
				continue;
			switch (ev[i].code) {
			case BTN_LEFT: btn = "left"; break;
			case BTN_RIGHT: btn = "right"; break;
			case BTN_MIDDLE: btn = "middle"; break;
			default: continue;
			}
			fprintf(out, "{\"t\":%ld,\"ev\":\"%s\",\"btn\":\"%s\"}\n",
					(int64_t)ev[i].input_event_sec * 1000000 + ev[i].input_event_usec,
					ev[i].value ? "down" : "up", btn);
		}
	}
}

static const struct ext_image_copy_capture_session_v1_listener capture_session_listener = {
	.buffer_size = (void *)noop,
	.shm_format = (void *)noop,
	.dmabuf_device = (void *)noop,
	.dmabuf_format = (void *)noop,
	.done = (void *)noop,
	.stopped = (void *)noop,
};

static void usage(void)
{
	fputs("usage: curlog (-o OUTPUT | -g \"X,Y WxH\") [-f FILE]\n", stderr);
	exit(2);
}

int main(int argc, char **argv)
{
	const char *want_output = NULL, *geometry = NULL, *path = NULL;
	int opt;
	while ((opt = getopt(argc, argv, "o:g:f:h")) != -1) {
		switch (opt) {
		case 'o': want_output = optarg; break;
		case 'g': geometry = optarg; break;
		case 'f': path = optarg; break;
		default: usage();
		}
	}
	if (!want_output == !geometry)
		usage();

	int32_t gx = 0, gy = 0, gw = 0, gh = 0;
	if (geometry && sscanf(geometry, "%d,%d %dx%d", &gx, &gy, &gw, &gh) != 4)
		die("bad geometry, expected \"X,Y WxH\"");

	struct wl_display *display = wl_display_connect(NULL);
	if (!display)
		die("cannot connect to Wayland");
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (!seat || !xdg_output_manager || !source_manager || !capture_manager)
		die("compositor lacks wl_seat, xdg-output or ext-image-copy-capture");
	for (int i = 0; i < noutputs; i++) {
		struct zxdg_output_v1 *xo =
			zxdg_output_manager_v1_get_xdg_output(xdg_output_manager, outputs[i].wl);
		zxdg_output_v1_add_listener(xo, &xdg_output_listener, &outputs[i]);
	}
	wl_display_roundtrip(display);

	// Output: by name, or the one containing the region's top-left corner.
	struct output *o = NULL;
	for (int i = 0; i < noutputs && !o; i++) {
		struct output *c = &outputs[i];
		if (want_output ? !strcmp(c->name, want_output)
				: gx >= c->x && gx < c->x + c->w && gy >= c->y && gy < c->y + c->h)
			o = c;
	}
	if (!o)
		die(want_output ? "no such output" : "geometry is not on any output");
	if (want_output) {
		gx = o->x; gy = o->y; gw = o->w; gh = o->h;
	}
	off_x = (gx - o->x) * o->scale;
	off_y = (gy - o->y) * o->scale;

	out = path ? fopen(path, "w") : stdout;
	if (!out)
		die("cannot open output file");
	setvbuf(out, NULL, _IOLBF, 0);

	// Signals via signalfd: wl_display_dispatch restarts poll on EINTR by itself.
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	sigprocmask(SIG_BLOCK, &mask, NULL);
	int sfd = signalfd(-1, &mask, SFD_CLOEXEC);

	struct ext_image_capture_source_v1 *source =
		ext_output_image_capture_source_manager_v1_create_source(source_manager, o->wl);
	struct ext_image_copy_capture_cursor_session_v1 *cs =
		ext_image_copy_capture_manager_v1_create_pointer_cursor_session(capture_manager,
				source, wl_seat_get_pointer(seat));
	ext_image_copy_capture_cursor_session_v1_add_listener(cs, &cursor_listener, NULL);
	// Without a capture session for the cursor image wlroots treats the source as
	// inactive: it sends leave right away and no more positions. We don't take
	// cursor frames — the session only keeps the source alive.
	ext_image_copy_capture_session_v1_add_listener(
			ext_image_copy_capture_cursor_session_v1_get_capture_session(cs),
			&capture_session_listener, NULL);

	int input_fds[MAX_INPUTS];
	int ninputs = open_click_devices(input_fds);
	if (!ninputs)
		fputs("curlog: no readable mouse in /dev/input, clicks are not logged\n", stderr);

	fprintf(out, "{\"v\":1,\"clock\":\"realtime_us\",\"t\":%ld,\"output\":\"%s\","
			"\"scale\":%d,\"region\":[%d,%d,%d,%d],\"click_devices\":%d}\n",
			now_us(), o->name, o->scale, off_x, off_y, gw * o->scale, gh * o->scale, ninputs);

	struct pollfd fds[2 + MAX_INPUTS] = {
		{ .fd = wl_display_get_fd(display), .events = POLLIN },
		{ .fd = sfd, .events = POLLIN },
	};
	for (int i = 0; i < ninputs; i++)
		fds[2 + i] = (struct pollfd){ .fd = input_fds[i], .events = POLLIN };
	for (;;) {
		while (wl_display_prepare_read(display) != 0)
			wl_display_dispatch_pending(display);
		wl_display_flush(display);
		if (poll(fds, 2 + ninputs, -1) < 0 && errno != EINTR) {
			wl_display_cancel_read(display);
			die("poll failed");
		}
		if (fds[1].revents) {
			wl_display_cancel_read(display);
			break;
		}
		if (fds[0].revents & POLLIN) {
			if (wl_display_read_events(display) < 0)
				die("lost connection to compositor");
		} else {
			wl_display_cancel_read(display);
		}
		if (fds[0].revents & (POLLHUP | POLLERR))
			die("lost connection to compositor");
		wl_display_dispatch_pending(display);
		for (int i = 0; i < ninputs; i++)
			if (fds[2 + i].revents & POLLIN)
				read_clicks(fds[2 + i].fd);
	}

	fprintf(out, "{\"t\":%ld,\"ev\":\"stop\"}\n", now_us());
	fclose(out);
	return 0;
}
