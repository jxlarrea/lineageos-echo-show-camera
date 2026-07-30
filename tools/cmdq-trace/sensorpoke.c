/* Read and write OV02B10 registers on the live sensor, from userspace.
 *
 * The preview is upside down because the sensor is mounted rotated 180
 * degrees and nothing compensates: the ported driver declares
 * .mirror = IMAGE_NORMAL and its set_mirror_flip() is #if 0'd out with empty
 * cases - and so is the upstream reference driver it came from, so no vendor
 * ever implemented it and the register is not documented anywhere available.
 *
 * Rather than guess the register and pay a kernel build plus a boot flash per
 * guess (with a wrong write to a paged sensor able to wedge it), this drives
 * MediaTek's own SENSOR_FEATURE_SET_REGISTER / GET_REGISTER through
 * KDIMGSENSORIOC_X_FEATURECONCTROL while cameraserver holds the sensor
 * powered and streaming. Effects are visible in the preview immediately and
 * every write is reverted by writing back the value read first.
 *
 * OV02B10 is paged: register 0xfd selects the bank, and page 3 register 0xfe
 * commits ("fresh"). Every access here sets the page explicitly.
 *
 * Parameters come from the environment, not argv: this is built -nostdlib with
 * a custom entry point, so there is no crt0 to hand main a real argc/argv.
 *
 *   POKE_OP=get POKE_PAGE=01 POKE_REG=12 sensorpoke
 *   POKE_OP=set POKE_PAGE=01 POKE_REG=12 POKE_VAL=03 sensorpoke
 *   POKE_OP=try POKE_PAGE=01 POKE_REG=12 POKE_VAL=03 sensorpoke  # set,hold,restore
 *   POKE_OP=scan POKE_PAGE=01                                    # dump a page
 *
 * Values are hex without 0x.
 */

typedef unsigned char u8;
typedef unsigned int u32;

int open(const char *path, int flags, ...);
int close(int fd);
int ioctl(int fd, unsigned long req, void *arg);
int __android_log_print(int prio, const char *tag, const char *fmt, ...);
unsigned int sleep(unsigned int seconds);
void exit(int status);
char *getenv(const char *name);
extern int *__errno(void);

#define printf(...) __android_log_print(6, "SENSORPOKE", __VA_ARGS__)
#define O_RDWR 2

/* _IOWR('i', 15, struct ACDK_SENSOR_FEATURECONTROL_STRUCT) — 16 bytes on 32-bit */
#define KDIMGSENSORIOC_X_FEATURECONCTROL 0xc010690f

#define SENSOR_FEATURE_SET_REGISTER 10
#define SENSOR_FEATURE_GET_REGISTER 11

/* cronos enumerates its camera in the sub slot (Facing: Front, CAM[1]) */
#define DUAL_CAMERA_SUB_SENSOR 2

struct feature_ctrl {
	u32 InvokeCamera;
	u32 FeatureId;
	u8 *pFeaturePara;
	u32 *pFeatureParaLen;
};

struct reg_info {
	u32 RegAddr;
	u32 RegData;
};

static int fd;
static u32 invoke_cam = DUAL_CAMERA_SUB_SENSOR;

static int feature(u32 id, struct reg_info *ri)
{
	struct feature_ctrl fc;
	u32 len = sizeof(*ri);

	fc.InvokeCamera = invoke_cam;
	fc.FeatureId = id;
	fc.pFeaturePara = (u8 *)ri;
	fc.pFeatureParaLen = &len;

	return ioctl(fd, KDIMGSENSORIOC_X_FEATURECONCTROL, &fc);
}

static void set_page(u32 page)
{
	struct reg_info ri = { 0xfd, page };

	feature(SENSOR_FEATURE_SET_REGISTER, &ri);
}

/* page 3 / 0xfe = 0x02 latches the paged writes */
static void commit(void)
{
	struct reg_info ri;

	set_page(0x03);
	ri.RegAddr = 0xfe;
	ri.RegData = 0x02;
	feature(SENSOR_FEATURE_SET_REGISTER, &ri);
}

static u32 reg_get(u32 page, u32 reg)
{
	struct reg_info ri;
	int r;

	set_page(page);
	ri.RegAddr = reg;
	ri.RegData = 0;
	r = feature(SENSOR_FEATURE_GET_REGISTER, &ri);
	if (r < 0) {
		printf("get p%02x:%02x FAILED errno=%d", page, reg, *__errno());
		return 0xffffffff;
	}
	return ri.RegData;
}

static void reg_set(u32 page, u32 reg, u32 val)
{
	struct reg_info ri;
	int r;

	set_page(page);
	ri.RegAddr = reg;
	ri.RegData = val;
	r = feature(SENSOR_FEATURE_SET_REGISTER, &ri);
	commit();
	printf("set p%02x:%02x = %02x (ret=%d)", page, reg, val, r);
}

static u32 hex(const char *s)
{
	u32 v = 0;

	while (*s) {
		u8 c = *s++;

		v <<= 4;
		if (c >= '0' && c <= '9')
			v |= c - '0';
		else if (c >= 'a' && c <= 'f')
			v |= c - 'a' + 10;
		else if (c >= 'A' && c <= 'F')
			v |= c - 'A' + 10;
	}
	return v;
}

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return *a == *b;
}

int main(void)
{
	const char *op = getenv("POKE_OP");
	const char *sp = getenv("POKE_PAGE");
	const char *sr = getenv("POKE_REG");
	const char *sv = getenv("POKE_VAL");
	const char *sc = getenv("POKE_CAM");   /* 1=main 2=sub 4=main2 */
	u32 p, r, v;

	if (!op) {
		printf("set POKE_OP=get|set|try|scan, POKE_PAGE, POKE_REG, POKE_VAL (hex)");
		exit(2);
	}

	fd = open("/dev/kd_camera_hw", O_RDWR, 0);
	if (fd < 0) {
		printf("open /dev/kd_camera_hw failed errno=%d", *__errno());
		exit(1);
	}

	if (sc)
		invoke_cam = hex(sc);
	p = sp ? hex(sp) : 0;
	r = sr ? hex(sr) : 0;
	v = sv ? hex(sv) : 0;

	if (streq(op, "get")) {
		printf("get p%02x:%02x = %02x", p, r, reg_get(p, r));
	} else if (streq(op, "scan")) {
		u32 i;

		for (i = 0; i <= 0xff; i++) {
			if (i == 0xfd || i == 0xfe || i == 0xfc)
				continue;      /* page select / commit */
			printf("scan p%02x:%02x = %02x", p, i, reg_get(p, i));
		}
	} else if (streq(op, "set")) {
		reg_set(p, r, v);
	} else if (streq(op, "try")) {
		u32 old = reg_get(p, r);

		printf("try p%02x:%02x old=%02x new=%02x", p, r, old, v);
		reg_set(p, r, v);
		sleep(6);              /* long enough to screencap */
		reg_set(p, r, old);
		printf("restored p%02x:%02x = %02x", p, r, old);
	} else {
		printf("unknown POKE_OP");
	}

	close(fd);
	exit(0);
	return 0;
}
