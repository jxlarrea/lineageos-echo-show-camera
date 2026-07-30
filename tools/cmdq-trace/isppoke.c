/* Read or write ISP registers through the driver's own REG ioctls.
 *
 * Addresses use the same convention as the HAL's ISP_WRITE_REGISTER calls:
 * offset from the CAMINF base, so the ISP block starts at 0x4000
 * (e.g. 0x4414 = TG_VF_CON, 0x4000 = CAM_CTL_START, 0x8014 = seninf top).
 *
 * Env parameters (no argv - built -nostdlib, see build.sh):
 *   POKE_OP=r|w
 *   POKE_ADDR=4414        hex offset
 *   POKE_VAL=1001         hex value (write only)
 *
 * Output goes to logcat, tag ISPPOKE.
 */

typedef unsigned int u32;

int open(const char *path, int flags, ...);
int close(int fd);
int ioctl(int fd, unsigned long req, void *arg);
int __android_log_print(int prio, const char *tag, const char *fmt, ...);
void exit(int status);
char *getenv(const char *name);
extern int *__errno(void);

#define printf(...) __android_log_print(6, "ISPPOKE", __VA_ARGS__)
#define O_RDWR 2

/* _IOWR('k', 2/3, struct ISP_REG_IO_STRUCT) - 8 bytes on LP32 */
#define ISP_READ_REGISTER 0xc0086b02
#define ISP_WRITE_REGISTER 0xc0086b03

struct isp_reg { u32 addr; u32 val; };
struct isp_reg_io { struct isp_reg *pData; u32 count; };

static u32 hex(const char *s)
{
	u32 v = 0;

	if (!s)
		return 0;
	while (*s) {
		char c = *s++;

		if (c >= '0' && c <= '9')
			v = v << 4 | (c - '0');
		else if (c >= 'a' && c <= 'f')
			v = v << 4 | (c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			v = v << 4 | (c - 'A' + 10);
	}
	return v;
}

int main(void)
{
	const char *op = getenv("POKE_OP");
	struct isp_reg reg;
	struct isp_reg_io io = { &reg, 1 };
	int fd = open("/dev/camera-isp", O_RDWR);
	int r;

	if (fd < 0) {
		printf("open /dev/camera-isp failed errno=%d", *__errno());
		exit(1);
	}

	reg.addr = hex(getenv("POKE_ADDR"));
	reg.val = hex(getenv("POKE_VAL"));

	if (op && op[0] == 'w') {
		r = ioctl(fd, ISP_WRITE_REGISTER, &io);
		printf("write %04x = %08x ret=%d errno=%d", reg.addr, reg.val,
		       r, r ? *__errno() : 0);
	} else {
		reg.val = 0;
		r = ioctl(fd, ISP_READ_REGISTER, &io);
		printf("read %04x -> %08x ret=%d errno=%d", reg.addr, reg.val,
		       r, r ? *__errno() : 0);
	}
	close(fd);
	exit(0);
	return 0;
}
