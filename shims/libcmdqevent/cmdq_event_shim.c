/* Translate cmdq event ids for the legacy camera stack.
 *
 * The API 25 libdpframework that the camera blobs use passes *raw GCE
 * hardware event values* in op_meta.event on CMDQ_IOCTL_ASYNC_EXEC, because
 * that is what its vintage of the MDP interface took. This kernel's
 * translate_meta() instead hands that field to cmdq_op_wait(), which reads it
 * as a CMDQ_EVENT_ENUM *index* and maps it to a hardware value through the
 * DTS-populated event table.
 *
 * The mismatch is fatal rather than merely wrong: cmdq_core_init_DTS_data()
 * defaults every hardware-event slot to CMDQ_SYNC_TOKEN_INVALID - 1 - i, which
 * is always negative, so a hardware value read as an enum index usually lands
 * on an event the DTS never named. cmdq_op_wait() then returns -EINVAL,
 * translate_user_job() aborts, and the whole submit is discarded - one bad
 * meta out of 167 kills the job. The camera saw this as
 * "startStream fail(-26)" and never received a frame.
 *
 * The failure is invisible from userspace by default because cmdq_ioctl()
 * assigns each handler's status to a local and then unconditionally returns 0.
 * The only signal is mdp_submit.job_id, which the kernel writes solely on the
 * success path; CMDQ_EVENT_TRACE=1 logs it.
 *
 * This is deliberately a userspace interposer rather than a kernel patch.
 * /dev/mtk_cmdq has a second client, android.hardware.graphics.composer, which
 * is Android 9 vintage and already agrees with this kernel. A kernel-side
 * remap would have to apply to both and would break display - the same
 * coupling that made downgrading libdpframework impossible. Loading this only
 * into cameraserver (via setenv LD_PRELOAD in its init rc) scopes the fix
 * precisely to the client that needs it.
 *
 * Built freestanding against bionic; see build.sh.
 */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

int __android_log_print(int prio, const char *tag, const char *fmt, ...);
void *dlsym(void *handle, const char *symbol);
void *dlopen(const char *filename, int flags);
#define RTLD_NOW 2
#define RTLD_NOLOAD 4
char *getenv(const char *name);

/* Not (void *)-1 as on glibc. Getting this wrong makes dlsym return NULL, the
 * interposer tail-call through it, and the process spin at 100% CPU in a way
 * that looks exactly like a hang inside the camera stack.
 */
#define RTLD_NEXT ((void *)0xfffffffeUL)

#define LOGE 6
#define TAG "CmdqEventShim"

#define IOC_NR(c) (((c) >> 0) & 0xff)
#define IOC_TYPE(c) (((c) >> 8) & 0xff)

/* _IOW('x', 20, struct mdp_submit) */
#define CMDQ_IOCTL_MAGIC 'x'
#define CMDQ_NR_ASYNC_EXEC 20

/* _IOWR('i', 65, struct IMAGESENSOR_GETINFO_STRUCT): the kernel fills an
 * ACDK_SENSOR_INFO2_STRUCT through pInfo. Overriding SensorOutputDataFormat
 * on the way back lets the Bayer order be tried at runtime instead of costing
 * a kernel build and a boot flash per candidate.
 */
#define IMGSENSOR_IOCTL_MAGIC 'i'
#define IMGSENSOR_NR_GETINFO 5
#define IMGSENSOR_NR_GETINFO2 65

/* struct IMAGESENSOR_GETINFO_STRUCT { u32 SensorId; INFO2 *pInfo; ... } */
#define GETINFO2_PINFO_OFF 4
/* struct ACDK_SENSOR_GETINFO_STRUCT { enum ScenarioId[2]; INFO *pInfo[2]; ... } */
#define GETINFO_PINFO_OFF 8
/* SensorOutputDataFormat within ACDK_SENSOR_INFO_STRUCT and its INFO2 twin,
 * which share this prefix: four u16 (8) + 15 u8 (23) padded to 24, then two
 * 32-bit fields and SensroInterfaceType.
 */
#define INFO_DATAFORMAT_OFF 36

/* Naturally aligned, 16 bytes; matches struct op_meta in mdp_def_ex.h. */
struct op_meta_k {
	u8 op;
	u8 _pad;
	u16 engine;
	u16 offset;	/* union: offset or event */
	u16 _pad2;
	u32 value;
	u32 mask;
};

struct mdp_submit {
	u64 metas;
	u32 meta_count;
	u32 priority;
	u64 engine_flag;
	u32 prop_size;
	u32 _pad;
	u64 prop_addr;
	u64 readback_ext;
	u32 read_count_v1;
	u32 _pad2;
	u64 hw_metas_read_v1;
	u64 job_id;
	u8 secData[64];
};

#include "event_map.h"

/* CMDQ_MOP_WAIT .. CMDQ_MOP_ACQUIRE all carry an event id. */
#define MOP_EVENT_FIRST 3
#define MOP_EVENT_LAST 7

int __system_property_get(const char *name, char *value);
extern int *__errno(void);

static int (*real_ioctl)(int, unsigned long, void *);
static int trace = -1;

/* ---- AWB debug dump redirection ----------------------------------------
 *
 * With awb.debug.dump.enable=1 the 3A blob writes per-frame AWB statistics
 * to a hardcoded /sdcard/awb/ path, which cameraserver cannot write (sdcardfs
 * gid "everybody", and native daemons do not get the emulated-storage mount).
 * Rewrite the prefix to /data/awbdump/, which camera-bringup.rc creates
 * world-writable. Harmless when dumping is off: the path never occurs.
 */

typedef void *FILE_p;
FILE_p (*real_fopen)(const char *path, const char *mode);
void *dlsym(void *handle, const char *symbol);

static const char *awb_redirect(const char *path, char *buf, unsigned buflen)
{
	static const char pfx[] = "/sdcard/awb/";
	unsigned i = 0, j = 0;
	static const char npfx[] = "/data/awbdump/";

	if (!path)
		return path;
	while (i < sizeof(pfx) - 1) {
		if (path[i] != pfx[i])
			return path;
		i++;
	}
	while (j < sizeof(npfx) - 1 && j < buflen - 1)
		buf[j] = npfx[j], j++;
	while (path[i] && j < buflen - 1)
		buf[j++] = path[i++];
	buf[j] = 0;
	return buf;
}

FILE_p fopen(const char *path, const char *mode)
{
	char buf[256];

	if (!real_fopen)
		real_fopen = dlsym(RTLD_NEXT, "fopen");
	return real_fopen(awb_redirect(path, buf, sizeof(buf)), mode);
}

/* persist.camera.bayer: 0=RAW_B 1=RAW_Gb 2=RAW_Gr 3=RAW_R, unset = leave
 * whatever the driver reports. Read fresh each time so a setprop plus a
 * camera restart is enough to try the next candidate.
 */
static int bayer_override(void)
{
	char v[16] = {0};

	if (__system_property_get("persist.camera.bayer", v) <= 0)
		return -1;
	if (v[0] < '0' || v[0] > '3' || v[1])
		return -1;
	return v[0] - '0';
}

static int tracing(void)
{
	if (trace < 0) {
		char *e = getenv("CMDQ_EVENT_TRACE");
		char v[16] = {0};

		/* init starts cameraserver, so an env var cannot be set
		 * without editing its rc; allow a property too.
		 */
		if (e && e[0] == '1')
			trace = 1;
		else if (__system_property_get("persist.camera.cmdqtrace", v) > 0)
			trace = (v[0] == '1');
		else
			trace = 0;
	}
	return trace;
}

/* ---- ISP ioctl tracing ('k' = ISP_MAGIC) --------------------------------
 *
 * Exists for the takePicture bug: the capture scenario switch leaves the TG
 * seeing vsyncs but never starting a frame, and the only way to see what the
 * HAL asked the ISP driver to do is to log the ioctl stream. Off unless
 * debug.isptrace is 1, re-checked every 64 calls so it can be toggled around
 * a single capture attempt without restarting cameraserver.
 */

struct isp_reg { u32 addr; u32 val; };
struct isp_reg_io { struct isp_reg *pData; u32 count; };
struct isp_wait_irq { u32 clear; u32 type; u32 status; u32 timeout; };
struct isp_buf_ctrl { u32 ctrl; u32 buf_id; void *data; void *ex_data; void *ext; };

static int isp_trace_on(void)
{
	static int state; /* 0 unknown, 1 on, 2 off */
	static u32 calls;

	if (state == 0 || (++calls & 63) == 0) {
		char v[92];

		v[0] = 0;
		__system_property_get("debug.isptrace", v);
		state = (v[0] == '1') ? 1 : 2;
	}
	return state == 1;
}

static const char *isp_cmd_name(u32 nr)
{
	static const char *const names[16] = {
		"RESET", "RESET_BUF", "READ_REG", "WRITE_REG", "HOLD_TIME",
		"HOLD_REG", "WAIT_IRQ", "READ_IRQ", "CLEAR_IRQ", "DUMP_REG",
		"SET_USER_PID", "RT_BUF_CTRL", "REF_CNT", "DEBUG_FLAG",
		"WAKELOCK_CTRL", "SENSOR_FREQ_CTRL",
	};

	return nr < 16 ? names[nr] : "?";
}

static int isp_ioctl_trace(int fd, unsigned long cmd, void *arg)
{
	u32 nr = IOC_NR(cmd);
	int r;

	if (nr == 3 && arg) { /* WRITE_REG: log the writes before issuing */
		struct isp_reg_io *io = arg;
		u32 i, logged = 0;

		__android_log_print(LOGE, "ISPTRACE",
			"WRITE_REG fd=%d count=%u", fd, io->count);
		for (i = 0; i < io->count && io->pData; i++) {
			u32 off = io->pData[i].addr & 0xFFFF;
			/* always show TG/VF/CTL/CQ writes, cap the rest */
			int hot = (off >= 0x400 && off <= 0x4EC) ||
				  off <= 0x38 ||
				  (off >= 0xA8 && off <= 0xF8);

			if (hot || logged < 24) {
				__android_log_print(LOGE, "ISPTRACE",
					"  wr[%u] %08x = %08x%s", i,
					io->pData[i].addr, io->pData[i].val,
					hot ? " *" : "");
				logged++;
			}
		}
		r = real_ioctl(fd, cmd, arg);
		__android_log_print(LOGE, "ISPTRACE",
			"WRITE_REG ret=%d errno=%d", r, r ? *__errno() : 0);
		return r;
	}

	if (nr == 6 && arg) { /* WAIT_IRQ */
		struct isp_wait_irq *w = arg;

		__android_log_print(LOGE, "ISPTRACE",
			"WAIT_IRQ fd=%d clear=%u type=%u status=%08x timeout=%u",
			fd, w->clear, w->type, w->status, w->timeout);

		/* The capture stall waits on PASS1_TG1_DON (bit 10). Sample
		 * the TG state right at the failing wait, and optionally
		 * intervene, selected by debug.isppoke:
		 *   vf   - toggle TG_VF_CON off/on
		 *   kick - rewrite CAM_CTL_START = 0x20
		 *   cmos - toggle TG_SEN_MODE CMOS_EN
		 */
		if (w->type == 0 && w->status == 0x400) {
			struct isp_reg regs[3] = {
				{ 0x4414, 0 }, { 0x444c, 0 }, { 0x40f4, 0 },
			};
			struct isp_reg_io rio = { regs, 3 };
			char pk[92];

			pk[0] = 0;
			real_ioctl(fd, 0xc0086b02 /* READ_REG */, &rio);
			__system_property_get("debug.isppoke", pk);
			__android_log_print(LOGE, "ISPTRACE",
				"  pre-wait VF=%08x TGST=%08x FBC=%08x poke=%s",
				regs[0].val, regs[1].val, regs[2].val, pk);

			if (pk[0] == 'v') { /* vf toggle */
				struct isp_reg wr = { 0x4414, 0x1000 };
				struct isp_reg_io wio = { &wr, 1 };

				real_ioctl(fd, 0xc0086b03, &wio);
				wr.val = 0x1001;
				real_ioctl(fd, 0xc0086b03, &wio);
			} else if (pk[0] == 'k') { /* CTL_START kick */
				struct isp_reg wr = { 0x4000, 0x20 };
				struct isp_reg_io wio = { &wr, 1 };

				real_ioctl(fd, 0xc0086b03, &wio);
			} else if (pk[0] == 'c') { /* cmos toggle */
				struct isp_reg wr = { 0x4410, 0x4 };
				struct isp_reg_io wio = { &wr, 1 };

				real_ioctl(fd, 0xc0086b03, &wio);
				wr.val = 0x5;
				real_ioctl(fd, 0xc0086b03, &wio);
			}
		}

		r = real_ioctl(fd, cmd, arg);
		__android_log_print(LOGE, "ISPTRACE",
			"WAIT_IRQ status=%08x -> ret=%d errno=%d", w->status, r,
			r ? *__errno() : 0);
		return r;
	}

	if (nr == 8 && arg) { /* CLEAR_IRQ: {type, status} */
		u32 *c = arg;

		r = real_ioctl(fd, cmd, arg);
		__android_log_print(LOGE, "ISPTRACE",
			"CLEAR_IRQ type=%u status=%08x ret=%d", c[0], c[1], r);
		return r;
	}

	if (nr == 11 && arg) { /* RT_BUF_CTRL */
		struct isp_buf_ctrl *b = arg;
		static const char *const ops[6] = {
			"ENQUE", "EX_ENQUE", "DEQUE", "IS_RDY", "GET_SIZE",
			"CLEAR",
		};

		r = real_ioctl(fd, cmd, arg);
		__android_log_print(LOGE, "ISPTRACE",
			"RT_BUF_CTRL %s dma=%u ret=%d errno=%d",
			b->ctrl < 6 ? ops[b->ctrl] : "?", b->buf_id, r,
			r ? *__errno() : 0);
		return r;
	}

	r = real_ioctl(fd, cmd, arg);
	__android_log_print(LOGE, "ISPTRACE",
		"%s fd=%d cmd=%08lx ret=%d errno=%d", isp_cmd_name(nr), fd,
		cmd, r, r ? *__errno() : 0);
	return r;
}

/* ---- AWB output trim ----------------------------------------------------
 *
 * The 3A blob set carries OV9734 tuning but the sensor is an OV02B10, so
 * every adaptive AWB decision lands with a fixed spectral bias: measured
 * under daylight, the algorithm outputs R=851 B=638 where the scene needs
 * R=722 B=732 (512 = unity). The gains are applied through mmap'd register
 * stores, so they cannot be intercepted at the ISP layer; instead this wraps
 * lib3a's exported per-frame entry
 *   AwbAlgo::handleAWB(AWB_INPUT_T&, AWB_OUTPUT_T&)
 * and rescales the gain triples in the output struct. Triples are located
 * by pattern - (R, 512, B) with both channels in a plausible gain range -
 * because the struct layout is not public.
 *
 * The bias is illuminant-dependent (the two modules' spectral responses
 * diverge differently under different spectra), so the trim is interpolated
 * between two calibrated anchors using the algorithm's own untrimmed B gain
 * as the illuminant key - it tracks warmth monotonically (measured ~640
 * under daylight, ~835 under 2600K LED) and is available in the very triple
 * being corrected, so no CCT field needs locating.
 *
 * Trim factors are 512-based multipliers from properties, re-read every 64
 * calls; .r/.b are the cool (daylight) anchors, .r.warm/.b.warm the warm
 * (2600K) anchors:
 *   persist.camera.awbtrim.r        default 390   (x0.762 at B<=640)
 *   persist.camera.awbtrim.b        default 532   (x1.039 at B<=640)
 *   persist.camera.awbtrim.r.warm   default 431
 *   persist.camera.awbtrim.b.warm   default 412
 * All four at 512 leaves the output untouched.
 */

#define AWBTRIM_B_COOL 640
#define AWBTRIM_B_WARM 835

static int (*real_handleawb)(void *thiz, void *in, void *out);

static u32 trim_prop(const char *name)
{
	char v[92];
	u32 n = 0;
	const char *p = v;

	v[0] = 0;
	__system_property_get(name, v);
	if (!v[0])
		return 512;
	while (*p >= '0' && *p <= '9')
		n = n * 10 + (*p++ - '0');
	return (n >= 128 && n <= 2048) ? n : 512;
}

int _ZN7AwbAlgo9handleAWBER11AWB_INPUT_TR12AWB_OUTPUT_T(void *thiz, void *in,
							void *out)
{
	static u32 trim_r = 512, trim_b = 512;
	static u32 trim_r_warm = 512, trim_b_warm = 512;
	static u32 calls;
	int r;
	u32 *o = out;
	u32 i;

	if (!real_handleawb) {
		/* lib3a.so is dlopen'd by the passthrough HAL into a local
		 * group, so RTLD_NEXT from a preload cannot see it. Take a
		 * handle on the already-loaded library instead; RTLD_NOLOAD
		 * fails only if it is not loaded yet, in which case loading
		 * it is harmless (same library, refcounted).
		 */
		void *h = dlopen("lib3a.so", RTLD_NOLOAD | RTLD_NOW);

		if (!h)
			h = dlopen("lib3a.so", RTLD_NOW);
		if (h)
			real_handleawb = dlsym(h,
				"_ZN7AwbAlgo9handleAWBER11AWB_INPUT_TR12AWB_OUTPUT_T");
		if (!real_handleawb)
			real_handleawb = dlsym(RTLD_NEXT,
				"_ZN7AwbAlgo9handleAWBER11AWB_INPUT_TR12AWB_OUTPUT_T");
	}
	if (!real_handleawb)
		return -1;

	r = real_handleawb(thiz, in, out);

	if ((calls++ & 63) == 0) {
		trim_r = trim_prop("persist.camera.awbtrim.r");
		trim_b = trim_prop("persist.camera.awbtrim.b");
		trim_r_warm = trim_prop("persist.camera.awbtrim.r.warm");
		trim_b_warm = trim_prop("persist.camera.awbtrim.b.warm");
	}

	if (out && (trim_r != 512 || trim_b != 512 ||
		    trim_r_warm != 512 || trim_b_warm != 512)) {
		/* When AWB is converged the algorithm does not rewrite the
		 * output fields every frame, so an in-place rescale would
		 * compound geometrically (measured: R decays to zero within
		 * seconds of convergence). Remember what we wrote; a word
		 * still holding our value was not recomputed - skip it.
		 */
		static u32 last_r[40], last_b[40];

		for (i = 0; i + 2 < 40; i++) {
			u32 t256, tr, tb, bg;

			if (o[i + 1] != 512)
				continue;
			if (o[i] < 300 || o[i] > 1400)
				continue;
			if (o[i + 2] < 300 || o[i + 2] > 1400)
				continue;
			if (o[i] == last_r[i] && o[i + 2] == last_b[i]) {
				i += 2;
				continue;
			}

			/* interpolate the anchors on this triple's own
			 * untrimmed B gain (warmer light -> higher B)
			 */
			bg = o[i + 2];
			if (bg <= AWBTRIM_B_COOL)
				t256 = 0;
			else if (bg >= AWBTRIM_B_WARM)
				t256 = 256;
			else
				t256 = (bg - AWBTRIM_B_COOL) * 256 /
				       (AWBTRIM_B_WARM - AWBTRIM_B_COOL);
			tr = (trim_r * (256 - t256) + trim_r_warm * t256) >> 8;
			tb = (trim_b * (256 - t256) + trim_b_warm * t256) >> 8;

			o[i] = o[i] * tr / 512;
			o[i + 2] = o[i + 2] * tb / 512;
			last_r[i] = o[i];
			last_b[i] = o[i + 2];
			if (tracing())
				__android_log_print(LOGE, "AWBTRIM",
					"triple @word %u t=%u trim=%u/%u -> %u/512/%u",
					i, t256, tr, tb, o[i], o[i + 2]);
			i += 2;
		}
	}
	return r;
}

/* Log each distinct (magic, nr) once. Answers "which ioctl carries this
 * field" without guessing, and without drowning logd in the preview loop.
 */
static void note_cmd(unsigned long cmd)
{
	static u32 seen[128];
	static u32 seen_n;
	u32 key = ((cmd >> 8) & 0xff) << 8 | (cmd & 0xff);
	u32 i;

	for (i = 0; i < seen_n; i++)
		if (seen[i] == key)
			return;
	if (seen_n < 128)
		seen[seen_n++] = key;

	__android_log_print(LOGE, TAG, "ioctl magic='%c'(%#lx) nr=%lu size=%lu",
		(char)((cmd >> 8) & 0xff), (unsigned long)((cmd >> 8) & 0xff),
		(unsigned long)(cmd & 0xff),
		(unsigned long)((cmd >> 16) & 0x3fff));
}

int ioctl(int fd, unsigned long cmd, void *arg)
{
	if (!real_ioctl)
		real_ioctl = dlsym(RTLD_NEXT, "ioctl");

	if (tracing())
		note_cmd(cmd);

	if (IOC_TYPE(cmd) == 'k' && isp_trace_on())
		return isp_ioctl_trace(fd, cmd, arg);

	if (IOC_TYPE(cmd) == CMDQ_IOCTL_MAGIC &&
	    IOC_NR(cmd) == CMDQ_NR_ASYNC_EXEC && arg) {
		struct mdp_submit *s = arg;
		struct op_meta_k *m = (void *)(unsigned long)s->metas;
		u32 i, fixed = 0;

		if (m) {
			/* debug.cmdqdump=1: dump the register-write metas of the
			 * next few submits. Used to locate which pass-2 offsets
			 * carry the applied AWB gains (they are not written via
			 * the ISP WRITE_REG ioctl).
			 */
			static int dumps_left = -1;
			char dv[92];

			dv[0] = 0;
			__system_property_get("debug.cmdqdump", dv);
			if (dv[0] == '1' && dumps_left < 0)
				dumps_left = 4;
			else if (dv[0] != '1')
				dumps_left = -1;

			if (dumps_left > 0) {
				dumps_left--;
				for (i = 0; i < s->meta_count; i++) {
					if (m[i].op != 0)
						continue;
					__android_log_print(LOGE, "CMDQDUMP",
						"m[%u] eng=%u off=%04x val=%08x msk=%08x",
						i, m[i].engine, m[i].offset,
						m[i].value, m[i].mask);
				}
			}

			for (i = 0; i < s->meta_count; i++) {
				u16 ev;

				if (m[i].op < MOP_EVENT_FIRST ||
				    m[i].op > MOP_EVENT_LAST)
					continue;

				ev = m[i].offset;
				if (ev < EVENT_MAP_MAX && event_map[ev] >= 0) {
					m[i].offset = event_map[ev];
					fixed++;
				}
			}
		}

		if (tracing()) {
			int r = real_ioctl(fd, cmd, arg);

			__android_log_print(LOGE, TAG,
				"async_exec metas=%u events_remapped=%u job_id=%llu %s",
				s->meta_count, fixed, s->job_id,
				s->job_id ? "accepted" : "REJECTED");
			return r;
		}
		return real_ioctl(fd, cmd, arg);
	}

	if (IOC_TYPE(cmd) == IMGSENSOR_IOCTL_MAGIC) {
		int r;
		int nr = IOC_NR(cmd);

		/* FEATURECONCTROL: log the 3A-relevant sensor features around a
		 * capture attempt. SET_ESHUTTER (3004) is the prime suspect for
		 * the takePicture stall: a bogus line count here collapses the
		 * frame rate and every pass-1 interrupt wait then times out.
		 */
		if (nr == 15 && arg && isp_trace_on()) {
			struct { u32 cam; u32 id; u32 *para; u32 *len; } *fc =
				(void *)arg;
			u32 id = fc->id;

			if (id >= 3000 && id < 3200) {
				u32 p0 = fc->para ? fc->para[0] : 0;
				u32 p1 = (fc->para && fc->len && *fc->len >= 8)
					 ? fc->para[1] : 0;

				r = real_ioctl(fd, cmd, arg);
				__android_log_print(LOGE, "ISPTRACE",
					"feature id=%u in=%u,%u out=0x%x,0x%x ret=%d",
					id, p0, p1,
					fc->para ? fc->para[0] : 0,
					(fc->para && fc->len && *fc->len >= 8)
						? fc->para[1] : 0, r);
				return r;
			}
		}

		r = real_ioctl(fd, cmd, arg);

		if (tracing())
			__android_log_print(LOGE, TAG,
				"imgsensor nr=%d size=%lu ret=%d", nr,
				(unsigned long)((cmd >> 16) & 0x3fff), r);

		/* The format arrives through one of the two GETINFO calls, and
		 * which one the HAL uses is not obvious - handle both. Their
		 * info structs share a prefix, so the field sits at the same
		 * offset in each; only the pointer's location in the argument
		 * differs.
		 */
		if (r == 0 && arg && (nr == IMGSENSOR_NR_GETINFO ||
				      nr == IMGSENSOR_NR_GETINFO2)) {
			int want = bayer_override();
			u32 p[2] = {0};
			int n = 0, i;

			if (nr == IMGSENSOR_NR_GETINFO2) {
				p[0] = *(u32 *)((u8 *)arg + GETINFO2_PINFO_OFF);
				n = 1;
			} else {
				/* ScenarioId[2] first, then pInfo[2] */
				p[0] = *(u32 *)((u8 *)arg + GETINFO_PINFO_OFF);
				p[1] = *(u32 *)((u8 *)arg + GETINFO_PINFO_OFF + 4);
				n = 2;
			}

			for (i = 0; i < n; i++) {
				u32 *fmt;

				if (!p[i])
					continue;
				fmt = (u32 *)(unsigned long)
					(p[i] + INFO_DATAFORMAT_OFF);
				if (tracing())
					__android_log_print(LOGE, TAG,
						"getinfo%s pInfo[%d] dataformat=%u want=%d",
						nr == IMGSENSOR_NR_GETINFO2 ? "2" : "",
						i, *fmt, want);
				if (want >= 0)
					*fmt = (u32)want;
			}
		}
		return r;
	}

	return real_ioctl(fd, cmd, arg);
}
