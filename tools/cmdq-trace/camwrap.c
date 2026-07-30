/* Exec cameraserver as uid/gid cameraserver(1047) with its rc groups
 * (audio, camera, input, drmrpc) plus media(1013) for the legacy nodes,
 * inheriting LD_PRELOAD from the caller. Run as root. */

typedef unsigned int gid_t;
typedef unsigned int uid_t;

int setgroups(int size, const gid_t *list);
int setgid(gid_t gid);
int setuid(uid_t uid);
int execve(const char *path, char *const argv[], char *const envp[]);
int __android_log_print(int prio, const char *tag, const char *fmt, ...);
char *getenv(const char *name);

extern char **environ;

int main(int argc, char **argv)
{
	static const gid_t groups[] = { 1005, 1006, 1004, 1026, 1013 };
	static char *cargv[] = { "/system/bin/cameraserver", 0 };

	if (setgroups(5, groups) || setgid(1047) || setuid(1047)) {
		__android_log_print(6, "CAMWRAP", "priv drop failed");
		return 1;
	}
	/* Forward CMDQ_FIX_EVENTS so the event remap can be toggled off for
	 * an A/B test without rebuilding anything.
	 */
	char *fix = getenv("CMDQ_FIX_EVENTS");
	char *cenvp[3];

	cenvp[0] = "LD_PRELOAD=/data/local/tmp/ioctl_hook.so";
	cenvp[1] = (fix && fix[0] == '0') ? "CMDQ_FIX_EVENTS=0"
					  : "CMDQ_FIX_EVENTS=1";
	cenvp[2] = 0;

	execve(cargv[0], cargv, cenvp);
	__android_log_print(6, "CAMWRAP", "exec failed");
	return 1;
}
