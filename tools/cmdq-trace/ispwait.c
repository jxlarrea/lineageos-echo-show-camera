/* Probe which ISP pass-1 interrupts are actually firing, from a second fd.
 *
 * ISP_WaitIrq works on driver-global state (g_IspInfo.IrqInfo), so any
 * process can open /dev/camera-isp and wait for an interrupt bit without
 * touching the HAL's session. Unlike reading CAM_CTL_INT_STATUS through
 * /proc/driver/isp_reg, this does not read-clear hardware status behind the
 * ISR's back - it rides the same accumulate path the HAL uses.
 *
 * This exists to answer the takePicture fork: when capture stalls with
 * IrqStatus(0), is the sensor still producing vsyncs (VS1 fires, TG config
 * problem downstream) or has it stopped outputting entirely (nothing fires,
 * sensor/seninf problem)?
 *
 * Env parameters (no argv - built -nostdlib, see build.sh):
 *   WAIT_MS=300        per-bit timeout in ms (default 300)
 *   WAIT_LOOPS=1       number of full passes over the bit set (default 1)
 *   WAIT_BIT=<n>       probe only bit n (decimal position) instead of the set
 *   WAIT_TYPE=<n>      irq type for WAIT_BIT mode (default 0 = INT)
 *   WAIT_TAG=<s>       extra label for WAIT_BIT mode log lines
 *
 * Output goes to logcat, tag ISPWAIT.
 */

typedef unsigned int u32;

int open(const char *path, int flags, ...);
int close(int fd);
int ioctl(int fd, unsigned long req, void *arg);
int __android_log_print(int prio, const char *tag, const char *fmt, ...);
void exit(int status);
char *getenv(const char *name);
extern int *__errno(void);

#define printf(...) __android_log_print(6, "ISPWAIT", __VA_ARGS__)
#define O_RDWR 2

/* _IOW('k', 6, struct ISP_WAIT_IRQ_STRUCT) - 16 bytes */
#define ISP_WAIT_IRQ 0x40106b06

#define ISP_IRQ_CLEAR_WAIT 1
#define ISP_IRQ_TYPE_INT 0
#define ISP_IRQ_TYPE_DMA 1

struct wait_irq {
	u32 Clear;
	u32 Type;
	u32 Status;
	u32 Timeout;
};

static const struct { u32 type; u32 bit; const char *name; } probes[] = {
	{ ISP_IRQ_TYPE_INT, 1u << 0,  "VS1" },
	{ ISP_IRQ_TYPE_INT, 1u << 1,  "TG1_ST1" },
	{ ISP_IRQ_TYPE_INT, 1u << 2,  "TG1_ST2" },
	{ ISP_IRQ_TYPE_INT, 1u << 3,  "EXPDON1" },
	{ ISP_IRQ_TYPE_INT, 1u << 10, "PASS1_TG1_DON" },
	{ ISP_IRQ_TYPE_INT, 1u << 12, "SOF1" },
	{ ISP_IRQ_TYPE_INT, 1u << 28, "FBC_IMGO_DONE" },
	{ ISP_IRQ_TYPE_DMA, 1u << 0,  "DMA_IMGO_DONE" },
};

static int hexnum(const char *s, int dflt)
{
	int v = 0;
	if (!s || !*s)
		return dflt;
	while (*s >= '0' && *s <= '9')
		v = v * 10 + (*s++ - '0');
	return v;
}

int main(void)
{
	int ms = hexnum(getenv("WAIT_MS"), 300);
	int loops = hexnum(getenv("WAIT_LOOPS"), 1);
	int fd = open("/dev/camera-isp", O_RDWR);
	int l, i;

	if (fd < 0) {
		printf("open /dev/camera-isp failed errno=%d", *__errno());
		exit(1);
	}

	if (getenv("WAIT_BIT")) {
		int bit = hexnum(getenv("WAIT_BIT"), 0);
		int type = hexnum(getenv("WAIT_TYPE"), 0);
		const char *tag = getenv("WAIT_TAG");

		for (l = 0; l < loops; l++) {
			struct wait_irq w;
			int ret;

			w.Clear = ISP_IRQ_CLEAR_WAIT;
			w.Type = (u32)type;
			w.Status = 1u << bit;
			w.Timeout = (u32)ms;
			ret = ioctl(fd, ISP_WAIT_IRQ, &w);
			printf("%s pass%d type=%d bit=%d -> %s (ret=%d errno=%d)",
			       tag ? tag : "bit", l, type, bit,
			       ret == 0 ? "FIRED" : "timeout", ret,
			       ret ? *__errno() : 0);
		}
		close(fd);
		exit(0);
	}

	for (l = 0; l < loops; l++) {
		for (i = 0; i < (int)(sizeof(probes) / sizeof(probes[0])); i++) {
			struct wait_irq w;
			int ret;

			w.Clear = ISP_IRQ_CLEAR_WAIT;
			w.Type = probes[i].type;
			w.Status = probes[i].bit;
			w.Timeout = (u32)ms;
			ret = ioctl(fd, ISP_WAIT_IRQ, &w);
			printf("pass%d %-14s type=%u bit=0x%08x -> %s (ret=%d errno=%d)",
			       l, probes[i].name, probes[i].type, probes[i].bit,
			       ret == 0 ? "FIRED" : "timeout", ret,
			       ret ? *__errno() : 0);
		}
	}
	close(fd);
	exit(0);
	return 0;
}
