/* Submit hand-built jobs to /dev/mtk_cmdq and report whether the kernel
 * accepted them.
 *
 * cmdq_ioctl() swallows every handler's status and returns 0, so the only
 * signal available from userspace is mdp_submit.job_id, which
 * mdp_ioctl_async_exec() writes exclusively on its success path. A non-zero
 * job_id means the whole submit path worked; zero means it failed somewhere
 * and the reason was discarded.
 *
 * Runs a series of cases that bisect the camera's failing submit: vary the
 * engine_flag, the meta mix and the meta count independently, so the failure
 * can be attributed to one of them without a kernel rebuild.
 */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

int open(const char *path, int flags, ...);
int close(int fd);
void exit(int status);
int ioctl(int fd, unsigned long req, void *arg);
int __android_log_print(int prio, const char *tag, const char *fmt, ...);
#define printf(...) __android_log_print(6, "CMDQPROBE", __VA_ARGS__)
extern int *__errno(void);

#define O_RDWR 2

struct op_meta {
	u8 op;
	u8 _pad;
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

/* _IOW('x', 20, struct mdp_submit), 136 bytes */
#define CMDQ_IOCTL_ASYNC_EXEC 0x40887814

#define MOP_WRITE 0
#define MOP_POLL 2
#define MOP_WAIT 3

static struct op_meta metas[512];

/* Event ids worth probing: the MDP/mutex stream events an MDP job would
 * legitimately wait on, plus boundary values around CMDQ_SYNC_TOKEN_MAX.
 */
static const u16 sweep[] = {
	/* The four the camera actually waits on, then boundaries. */
	28, 32, 33, 131,
	0, 1, 0x35, 0x80, 0xFF, 0x100, 0x140, 0x180,
	0x1FE, 0x1FF, 0x200, 0x2FF,
};

static void run(int fd, const char *name, u32 count, u64 engine_flag)
{
	struct mdp_submit s;
	u8 *p = (u8 *)&s;
	u32 i;
	int r;

	for (i = 0; i < sizeof(s); i++)
		p[i] = 0;

	s.metas = (u32)(unsigned long)metas;
	s.meta_count = count;
	s.priority = 20;
	s.engine_flag = engine_flag;
	s.job_id = 0;

	r = ioctl(fd, CMDQ_IOCTL_ASYNC_EXEC, &s);
	printf("%-34s count=%-4u eng=%#-6llx ret=%d errno=%-3d job_id=%llu  %s\n",
		name, count, engine_flag, r, *__errno(), s.job_id,
		s.job_id ? "ACCEPTED" : "REJECTED");
}

static void run_evt(int fd, u16 event)
{
	struct mdp_submit s;
	u8 *p = (u8 *)&s;
	u32 i;
	int r;

	for (i = 0; i < sizeof(s); i++)
		p[i] = 0;
	s.metas = (u32)(unsigned long)metas;
	s.meta_count = 1;
	s.priority = 20;
	s.engine_flag = 0x150;
	s.job_id = 0;

	r = ioctl(fd, CMDQ_IOCTL_ASYNC_EXEC, &s);
	printf("wait-only event=%#-6x ret=%d errno=%-3d job_id=%llu  %s",
		event, r, *__errno(), s.job_id,
		s.job_id ? "ACCEPTED" : "REJECTED");
}

int main(void)
{
	int fd = open("/dev/mtk_cmdq", O_RDWR, 0);
	u32 i;

	if (fd < 0) {
		printf("open /dev/mtk_cmdq failed errno=%d\n", *__errno());
		return 1;
	}
	printf("opened /dev/mtk_cmdq fd=%d\n\n", fd);

	/* A single benign masked write to MDP_RDMA0, the same engine and
	 * offset the camera's own first meta uses.
	 */
	metas[0].op = MOP_WRITE;
	metas[0].engine = 1;	/* ENGBASE_MDP_RDMA0 */
	metas[0].offset = 0x008;
	metas[0].value = 0x1;
	metas[0].mask = 0x1;

	run(fd, "1 write, no engine flag", 1, 0);
	run(fd, "1 write, camera engine flag", 1, 0x150);
	run(fd, "1 write, RDMA0 bit only", 1, 1ull << 1);

	/* Fill with the same write and scale up, to separate "this kernel
	 * rejects the content" from "it rejects the size".
	 */
	for (i = 0; i < 512; i++)
		metas[i] = metas[0];

	run(fd, "167 writes, camera engine flag", 167, 0x150);
	run(fd, "231 writes, camera engine flag", 231, 0x369);

	/* Add the op mix the camera actually sends: writes + polls + waits. */
	metas[1].op = MOP_POLL;
	metas[1].engine = 1;
	metas[1].offset = 0x408;
	metas[1].value = 0x100;
	metas[1].mask = 0x100;

	run(fd, "write+poll", 2, 0x150);

	/* MUTEX0_STREAM_EOF is what the stuck thread in /proc status waits on. */
	metas[2].op = MOP_WAIT;
	metas[2].engine = 0;
	metas[2].offset = 0x35;
	metas[2].value = 0;
	metas[2].mask = 0;

	run(fd, "write+poll+wait(evt 0x35)", 3, 0x150);

	/* Sweep the event id: which values does cmdq_op_wait() accept?
	 * A single WAIT meta, nothing else, so only the event varies.
	 */
	metas[0].op = MOP_WAIT;
	metas[0].engine = 0;
	metas[0].value = 0;
	metas[0].mask = 0;
	for (i = 0; i < sizeof(sweep) / sizeof(sweep[0]); i++) {
		metas[0].offset = sweep[i];
		run_evt(fd, sweep[i]);
	}

	close(fd);
	exit(0);
	return 0;
}
