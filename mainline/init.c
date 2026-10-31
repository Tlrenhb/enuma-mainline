// SPDX-License-Identifier: MIT
/*
 * enuma-mainline-probe: PID1 for a throwaway initramfs.
 *
 * Output channels, most reliable first:
 *   1. decodable colour bands painted into the bottom quarter of the panel
 *   2. the complete kernel ring buffer forwarded to a USB CDC-ACM gadget
 *   3. the same text on /dev/tty0, visible when fbcon came up
 *
 * PID1 must never exit, so every failure path falls through to the idle loop.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <linux/fb.h>

#define KMSG_LIMIT (512 * 1024)

static int tty0 = -1, ttygs = -1;
static char kbuf[KMSG_LIMIT];
static size_t klen;

static void emit(const char *b, size_t n)
{
	if (tty0 >= 0) {
		ssize_t r = write(tty0, b, n);
		(void)r;
	}
	if (ttygs >= 0) {
		ssize_t r = write(ttygs, b, n);
		(void)r;
	}
}

static void say(const char *fmt, ...)
{
	char b[1024];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	if (n > 0)
		emit(b, (size_t)(n < (int)sizeof b ? n : (int)sizeof b - 1));
}

static int wr(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	size_t len = strlen(val);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = write(fd, val, len);
	close(fd);
	return n == (ssize_t)len ? 0 : -1;
}

static int mkdir_p(const char *path)
{
	if (mkdir(path, 0755) == 0 || errno == EEXIST)
		return 0;
	return -1;
}

static int mount_if(const char *src, const char *tgt, const char *type)
{
	if (mount(src, tgt, type, 0, NULL) == 0 || errno == EBUSY)
		return 0;
	return -1;
}

/* ---- output channel 3: /dev/tty0 ------------------------------------- */

static void open_tty0(void)
{
	tty0 = open("/dev/tty0", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (tty0 < 0)
		tty0 = open("/dev/console", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
}

/* ---- output channel 2: USB CDC-ACM gadget ---------------------------- */

static const char *first_udc(void)
{
	static char name[128];
	DIR *d = opendir("/sys/class/udc");
	struct dirent *de;

	if (!d)
		return NULL;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		snprintf(name, sizeof name, "%.64s", de->d_name);
		closedir(d);
		return name;
	}
	closedir(d);
	return NULL;
}

static void setup_gadget(void)
{
	const char *udc;
	char p[256];

	mkdir_p("/sys/kernel/config");
	if (mount_if("configfs", "/sys/kernel/config", "configfs") < 0) {
		say("probe: configfs unavailable: %s\n", strerror(errno));
		return;
	}
	if (mkdir_p("/sys/kernel/config/usb_gadget/enuma") < 0) {
		say("probe: gadget dir failed: %s\n", strerror(errno));
		return;
	}
#define GP(fmt, ...) (snprintf(p, sizeof p, fmt, ##__VA_ARGS__), p)
	wr(GP("/sys/kernel/config/usb_gadget/enuma/idVendor"), "0x1d6b");
	wr(GP("/sys/kernel/config/usb_gadget/enuma/idProduct"), "0x0130");
	wr(GP("/sys/kernel/config/usb_gadget/enuma/bcdUSB"), "0x0200");
	wr(GP("/sys/kernel/config/usb_gadget/enuma/bcdDevice"), "0x0100");
	mkdir_p("/sys/kernel/config/usb_gadget/enuma/strings/0x409");
	wr(GP("/sys/kernel/config/usb_gadget/enuma/strings/0x409/serialnumber"),
	   "enuma-probe");
	wr(GP("/sys/kernel/config/usb_gadget/enuma/strings/0x409/manufacturer"),
	   "enuma");
	wr(GP("/sys/kernel/config/usb_gadget/enuma/strings/0x409/product"),
	   "enuma mainline probe");
	mkdir_p("/sys/kernel/config/usb_gadget/enuma/configs/c.1");
	mkdir_p("/sys/kernel/config/usb_gadget/enuma/configs/c.1/strings/0x409");
	wr(GP("/sys/kernel/config/usb_gadget/enuma/configs/c.1/strings/0x409/configuration"),
	   "acm");
	mkdir_p("/sys/kernel/config/usb_gadget/enuma/functions/acm.usb0");
	{
		const char *link = "/sys/kernel/config/usb_gadget/enuma/configs/c.1/acm.usb0";
		if (symlink("/sys/kernel/config/usb_gadget/enuma/functions/acm.usb0", link) < 0 &&
		    errno != EEXIST) {
			say("probe: acm link failed: %s\n", strerror(errno));
			return;
		}
	}
	udc = first_udc();
	if (!udc) {
		say("probe: no UDC registered\n");
		return;
	}
	if (wr(GP("/sys/kernel/config/usb_gadget/enuma/UDC"), udc) < 0) {
		say("probe: UDC bind failed: %s\n", strerror(errno));
		return;
	}
	say("probe: gadget bound to %s\n", udc);
#undef GP
}

static void open_ttygs(void)
{
	int i;

	for (i = 0; i < 50; i++) {
		ttygs = open("/dev/ttyGS0", O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (ttygs >= 0)
			return;
		usleep(100 * 1000);
	}
}

/* ---- output channel 1: framebuffer bands ----------------------------- */

static const uint32_t band_colour[] = {
	0xffffff, /* 0 white   */
	0xff00ff, /* 1 magenta */
	0xffff00, /* 2 yellow  */
	0x00ffff, /* 3 cyan    */
	0x00ff00, /* 4 green   */
	0xff0000, /* 5 red     */
	0x0000ff, /* 6 blue    */
	0x808080, /* 7 grey    */
};

static void fill_rect(uint8_t *base, const struct fb_fix_screeninfo *fi,
		      const struct fb_var_screeninfo *vi, unsigned x0, unsigned y0,
		      unsigned w, unsigned h, uint32_t rgb)
{
	unsigned y, x;
	uint32_t px;

	if (vi->bits_per_pixel == 32)
		px = (vi->red.offset == 16 ? rgb : ((rgb & 0xff) << 16) |
						      (rgb & 0xff00) |
						      ((rgb >> 16) & 0xff));
	else if (vi->bits_per_pixel == 16)
		px = 0;
	else
		return;

	for (y = y0; y < y0 + h && y < vi->yres; y++) {
		uint8_t *row = base + (size_t)y * fi->line_length;
		for (x = x0; x < x0 + w && x < vi->xres; x++) {
			if (vi->bits_per_pixel == 32)
				*(uint32_t *)(row + (size_t)x * 4) = px;
			else
				*(uint16_t *)(row + (size_t)x * 2) =
					(uint16_t)(((rgb >> 19) & 0x1f) << 11 |
						   ((rgb >> 10) & 0x3f) << 5 |
						   ((rgb >> 3) & 0x1f));
		}
	}
}

static int paint_fb(void)
{
	struct fb_var_screeninfo vi;
	struct fb_fix_screeninfo fi;
	size_t len;
	uint8_t *map;
	unsigned i, bands = (unsigned)(sizeof band_colour / sizeof band_colour[0]);
	unsigned top, bh, bw;
	int fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);

	if (fd < 0)
		return -1;
	if (ioctl(fd, FBIOGET_VSCREENINFO, &vi) < 0 ||
	    ioctl(fd, FBIOGET_FSCREENINFO, &fi) < 0) {
		say("probe: fb ioctl failed: %s\n", strerror(errno));
		close(fd);
		return -1;
	}
	len = (size_t)fi.line_length * vi.yres_virtual;
	if (!len || (map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) ==
			    MAP_FAILED) {
		say("probe: fb mmap failed: %s\n", strerror(errno));
		close(fd);
		return -1;
	}

	/* Orientation marker: solid white square in the top-left corner. */
	bw = vi.xres / 16;
	if (bw < 64)
		bw = 64;
	fill_rect(map, &fi, &vi, 0, 0, bw, bw, 0xffffff);

	/* Bottom quarter: eight equal horizontal bands, top-to-bottom. */
	top = vi.yres * 3 / 4;
	bh = (vi.yres - top) / bands;
	for (i = 0; i < bands; i++)
		fill_rect(map, &fi, &vi, 0, top + i * bh, vi.xres, bh,
			  band_colour[i]);

	msync(map, len, MS_SYNC);
	munmap(map, len);
	close(fd);

	say("probe: fb %ux%u bpp=%u stride=%u bands=%u from y=%u h=%u\n", vi.xres,
	    vi.yres, vi.bits_per_pixel, fi.line_length, bands, top, bh);
	return 0;
}

/* ---- kernel ring buffer --------------------------------------------- */

static void drain_kmsg(void)
{
	int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	char tmp[8192];
	size_t start = klen;
	ssize_t n;

	if (fd < 0)
		return;
	while (klen < KMSG_LIMIT) {
		size_t room = KMSG_LIMIT - klen;
		n = read(fd, tmp, room < sizeof tmp ? room : sizeof tmp);
		if (n <= 0)
			break;
		memcpy(kbuf + klen, tmp, (size_t)n);
		klen += (size_t)n;
	}
	close(fd);
	if (klen > start)
		emit(kbuf + start, klen - start);
}

/* The host may enumerate the gadget after the first writes are already
 * queued, and CDC-ACM drops anything sent before it opens the port.  Replay
 * the whole ring buffer a few times so a late listener still sees the boot.
 */
static void redump(int rounds)
{
	int i;
	char tag[] = "\n--- replay ---\n";

	for (i = 0; i < rounds; i++) {
		sleep(6);
		drain_kmsg();
		emit(tag, sizeof tag - 1);
		emit(kbuf, klen);
	}
}

/* ---- setup ---------------------------------------------------------- */

static void banner(void)
{
	char cmd[512] = "";
	int fd = open("/proc/cmdline", O_RDONLY | O_CLOEXEC);

	if (fd >= 0) {
		ssize_t n = read(fd, cmd, sizeof cmd - 1);
		if (n > 0) {
			cmd[n] = 0;
			cmd[strcspn(cmd, "\n")] = 0;
		}
		close(fd);
	}
	say("\n=== enuma-mainline-probe ===\nframebuffer: %s\ncmdline: %s\n",
	    access("/dev/fb0", F_OK) == 0 ? "present" : "MISSING", cmd);
}

int main(void)
{
	int i;

	mkdir_p("/proc");
	mkdir_p("/sys");
	mkdir_p("/dev");
	mount_if("devtmpfs", "/dev", "devtmpfs");
	mount_if("proc", "/proc", "proc");
	mount_if("sysfs", "/sys", "sysfs");

	open_tty0();

	/* The panel can appear a little after init starts; retry briefly. */
	for (i = 0; i < 100; i++) {
		if (paint_fb() == 0)
			break;
		usleep(200 * 1000);
	}

	setup_gadget();
	open_ttygs();

	/* Give the host time to enumerate the ACM port before the first write. */
	sleep(3);

	banner();
	drain_kmsg();
	redump(6);

	for (;;) {
		sleep(5);
		drain_kmsg();
	}
}