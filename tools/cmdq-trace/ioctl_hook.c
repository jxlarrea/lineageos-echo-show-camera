/* LD_PRELOAD interposer for the API 25 libdpframework_cam.so cmdq traffic.
 *
 * Two jobs:
 *
 *  1. Trace CMDQ_IOCTL_ASYNC_EXEC / ASYNC_WAIT. cmdq_ioctl() swallows every
 *     handler's status and returns 0, so the only failure signal available
 *     is mdp_submit.job_id, which the kernel writes solely on success.
 *
 *  2. Translate wait/set/clear event ids. The old library passes raw GCE
 *     *hardware* event values, while this kernel's meta ABI expects
 *     CMDQ_EVENT_ENUM *indices* that it maps to hardware values through the
 *     DTS table. A hardware value read as an enum index usually lands on an
 *     event with no DTS entry, whose slot defaults negative, so
 *     cmdq_op_wait() returns -EINVAL and the whole submit is rejected.
 *     Set CMDQ_FIX_EVENTS=0 to trace without rewriting.
 *
 * Doing this here rather than in the kernel keeps the graphics composer -
 * the other /dev/mtk_cmdq client, and one that matches this kernel's ABI -
 * completely untouched.
 *
 * Built freestanding against the device's own bionic.
 */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

/* bionic */
int __android_log_print(int prio, const char *tag, const char *fmt, ...);
void *dlsym(void *handle, const char *symbol);
extern int *__errno(void);
#define RTLD_NEXT ((void *)0xfffffffeUL) /* LP32 bionic */
#define LOGP 6 /* ANDROID_LOG_ERROR */
#define TAG "CMDQHOOK"

#define IOC_NR(c) (((c) >> 0) & 0xff)
#define IOC_TYPE(c) (((c) >> 8) & 0xff)
#define IOC_SIZE(c) (((c) >> 16) & 0x3fff)

struct op_meta {
	u8 op;
	u16 engine;
	u16 offset_or_event;
	u32 value;
	u32 mask;
} __attribute__((packed, aligned(4)));

/* kernel struct is naturally aligned, 16 bytes */
struct op_meta_k {
	u8 op; u8 _pad;
	u16 engine;
	u16 offset;
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

char *getenv(const char *name);
int __system_property_get(const char *name, char *value);

static int (*real_ioctl)(int, unsigned long, void *);

/* ---- ISP ioctl tracing ('k' = ISP_MAGIC), for the takePicture bug ----
 *
 * Off unless the system property debug.isptrace is 1, re-checked every 64
 * calls so it can be toggled around a single capture attempt without
 * restarting cameraserver.
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
	static const char *const names[] = {
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

		__android_log_print(LOGP, "ISPTRACE",
			"WRITE_REG fd=%d count=%u", fd, io->count);
		for (i = 0; i < io->count && io->pData; i++) {
			u32 off = io->pData[i].addr & 0xFFFF;
			/* always show TG/VF/CTL/CQ writes, cap the rest */
			int hot = (off >= 0x400 && off <= 0x4EC) ||
				  off <= 0x38 ||
				  (off >= 0xA8 && off <= 0xF8);

			if (hot || logged < 24) {
				__android_log_print(LOGP, "ISPTRACE",
					"  wr[%u] %08x = %08x%s", i,
					io->pData[i].addr, io->pData[i].val,
					hot ? " *" : "");
				logged++;
			}
		}
		r = real_ioctl(fd, cmd, arg);
		__android_log_print(LOGP, "ISPTRACE",
			"WRITE_REG ret=%d errno=%d", r, r ? *__errno() : 0);
		return r;
	}

	if (nr == 6 && arg) { /* WAIT_IRQ */
		struct isp_wait_irq *w = arg;

		__android_log_print(LOGP, "ISPTRACE",
			"WAIT_IRQ fd=%d clear=%u type=%u status=%08x timeout=%u",
			fd, w->clear, w->type, w->status, w->timeout);
		r = real_ioctl(fd, cmd, arg);
		__android_log_print(LOGP, "ISPTRACE",
			"WAIT_IRQ status=%08x -> ret=%d errno=%d", w->status, r,
			r ? *__errno() : 0);
		return r;
	}

	if (nr == 8 && arg) { /* CLEAR_IRQ: {type, status} */
		u32 *c = arg;

		r = real_ioctl(fd, cmd, arg);
		__android_log_print(LOGP, "ISPTRACE",
			"CLEAR_IRQ type=%u status=%08x ret=%d", c[0], c[1], r);
		return r;
	}

	if (nr == 11 && arg) { /* RT_BUF_CTRL */
		struct isp_buf_ctrl *b = arg;
		static const char *const ops[] = {
			"ENQUE", "EX_ENQUE", "DEQUE", "IS_RDY", "GET_SIZE",
			"CLEAR",
		};

		r = real_ioctl(fd, cmd, arg);
		__android_log_print(LOGP, "ISPTRACE",
			"RT_BUF_CTRL %s dma=%u ret=%d errno=%d",
			b->ctrl < 6 ? ops[b->ctrl] : "?", b->buf_id, r,
			r ? *__errno() : 0);
		return r;
	}

	r = real_ioctl(fd, cmd, arg);
	__android_log_print(LOGP, "ISPTRACE",
		"%s fd=%d cmd=%08lx ret=%d errno=%d", isp_cmd_name(nr), fd,
		cmd, r, r ? *__errno() : 0);
	return r;
}

/* -1 until resolved from the environment. */
static int fix_events = -1;

static int should_fix(void)
{
	if (fix_events < 0) {
		char *e = getenv("CMDQ_FIX_EVENTS");

		fix_events = (e && e[0] == '0') ? 0 : 1;
	}
	return fix_events;
}

static void dump_hex(const char *pfx, const void *buf, int len)
{
	const u8 *p = buf;
	char line[3 * 16 + 1];
	int i, j;

	for (i = 0; i < len; i += 16) {
		char *o = line;
		for (j = 0; j < 16 && i + j < len; j++) {
			static const char h[] = "0123456789abcdef";
			*o++ = h[p[i + j] >> 4];
			*o++ = h[p[i + j] & 15];
			*o++ = ' ';
		}
		*o = 0;
		__android_log_print(LOGP, TAG, "%s +%03x: %s", pfx, i, line);
	}
}

int ioctl(int fd, unsigned long cmd, void *arg)
{
	if (!real_ioctl)
		real_ioctl = dlsym(RTLD_NEXT, "ioctl");

	if (IOC_TYPE(cmd) == 'k' && isp_trace_on())
		return isp_ioctl_trace(fd, cmd, arg);

	if (IOC_TYPE(cmd) == 'x' && IOC_NR(cmd) == 20 && arg) {
		struct mdp_submit *s = arg;
		/* The preview loop resubmits the same two job shapes forever,
		 * and logd's chatty filter drops repeated identical lines
		 * ("expire N lines"), which silently truncates the trace. Do
		 * the verbose per-meta logging once per distinct meta_count.
		 */
		static u32 seen[8];
		static u32 seen_n;
		int verbose = 1;
		u32 k;

		for (k = 0; k < seen_n; k++)
			if (seen[k] == s->meta_count)
				verbose = 0;
		if (verbose && seen_n < 8)
			seen[seen_n++] = s->meta_count;

		__android_log_print(LOGP, TAG,
			"EXEC fd=%d cmd=%08lx metas=%llx count=%u prio=%u eng=%llx prop=%u/%llx rb=%llx rc1=%u hw1=%llx job=%llx",
			fd, cmd, s->metas, s->meta_count, s->priority,
			s->engine_flag, s->prop_size, s->prop_addr,
			s->readback_ext, s->read_count_v1, s->hw_metas_read_v1,
			s->job_id);
		if (s->metas && s->meta_count) {
			const struct op_meta_k *m = (void *)(unsigned long)s->metas;
			u32 i, bad = 0;
			u32 ophist[16] = {0};
			u32 evt[8] = {0};
			u32 evt_n = 0;
			u32 fixed = 0;

			for (i = 0; i < s->meta_count; i++) {
				u32 op = m[i].op;
				const char *why = 0;

				if (op < 16)
					ophist[op]++;

				/* mirror kernel translate_meta() validation */
				if (op >= 12)
					why = "op out of range";
				else if (op == 8)
					/* WRITE_FD: cmdq_mdp_get_hw_port() is
					 * all-zero on mt8163 -> always 0
					 */
					why = "WRITE_FD but mdp_engine_port[]==0";
				else if (op == 1)
					why = "READ needs a readback slot";
				else if (op >= 3 && op <= 7) {
					/* event ops: cmdq_op_wait() and
					 * friends reject >= CMDQ_SYNC_TOKEN_MAX
					 */
					u16 ev = m[i].offset;

					if (should_fix() && ev < EVENT_MAP_MAX &&
					    event_map[ev] >= 0) {
						/* rewrite in place: hardware
						 * value -> enum index
						 */
						((struct op_meta_k *)m)[i].offset =
							event_map[ev];
						fixed++;
					}
					if (m[i].offset >= 0x1FF)
						why = "event >= CMDQ_SYNC_TOKEN_MAX";
					else if (evt_n < 8)
						evt[evt_n++] = ev;
				} else if (op == 0 || op == 2 || op == 9 ||
					 op == 10) {
					if (m[i].engine >= 10)
						why = "engine >= ENGBASE_COUNT";
					else if (m[i].offset > 0xFFC)
						why = "offset > 0xFFC";
					else if (m[i].offset & 3)
						why = "offset unaligned";
				}

				if (why) {
					bad++;
					if (bad <= 8)
						__android_log_print(LOGP, TAG,
						  "  BAD meta[%u] op=%u eng=%u off=%04x val=%08x mask=%08x : %s",
						  i, op, m[i].engine,
						  m[i].offset, m[i].value,
						  m[i].mask, why);
				}
			}
			__android_log_print(LOGP, TAG,
				"EXEC meta_count=%u bad=%u ops: w=%u rd=%u poll=%u wait=%u wnc=%u clr=%u set=%u acq=%u fd=%u fromreg=%u sec=%u nop=%u",
				s->meta_count, bad, ophist[0], ophist[1],
				ophist[2], ophist[3], ophist[4], ophist[5],
				ophist[6], ophist[7], ophist[8], ophist[9],
				ophist[10], ophist[11]);
			(void)verbose;
			{
				__android_log_print(LOGP, TAG,
					"EXEC meta_count=%u events(hw): %u %u %u %u %u %u %u %u (n=%u, remapped=%u)",
					s->meta_count, evt[0], evt[1], evt[2],
					evt[3], evt[4], evt[5], evt[6], evt[7],
					evt_n, fixed);
			}
		}
		int r = real_ioctl(fd, cmd, arg);
		__android_log_print(LOGP, TAG,
			"EXEC ret=%d errno=%d job_id(after)=%llx",
			r, *__errno(), s->job_id);
		return r;
	}

	if (IOC_TYPE(cmd) == 'x' && IOC_NR(cmd) == 21 && arg) {
		u64 job = *(u64 *)arg;
		int r = real_ioctl(fd, cmd, arg);
		__android_log_print(LOGP, TAG, "WAIT job=%llx ret=%d errno=%d",
			job, r, *__errno());
		return r;
	}

	if (IOC_TYPE(cmd) == 'x') {
		int r = real_ioctl(fd, cmd, arg);
		__android_log_print(LOGP, TAG,
			"OTHER cmd=%08lx nr=%lu size=%lu ret=%d errno=%d",
			cmd, (unsigned long)IOC_NR(cmd),
			(unsigned long)IOC_SIZE(cmd), r, *__errno());
		return r;
	}

	return real_ioctl(fd, cmd, arg);
}
