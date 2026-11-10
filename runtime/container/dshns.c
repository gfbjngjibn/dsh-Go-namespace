// dshns: the system container launcher for the local engine. Runs as root
// (`su -W --ksu-no-new-privs -c ...`), statically linked against bionic.
//
//   dshns start  --name N --state DIR --rootfs DIR --uid U --gid G [--groups G1,G2...] --range BASE:COUNT
//                [--bind HOST:GUEST[:ro]]... [--storage IMG --storage-size BYTES] [--net]
//   dshns exec   --name N --state DIR [--cwd DIR] [--forward PORT] -- ARGV...
//   dshns stop   --name N --state DIR [--timeout SECONDS]
//   dshns status --name N --state DIR
//   dshns run    (start's options) -- ARGV
//   dshns grow   --name N --state DIR --storage IMG --storage-size BYTES
//   dshns trial  --uid U --gid G
//
// The container is rootless: every namespace is created inside a new user
// namespace whose root is the app's uid outside (uid_map "0 U 1"), and whose
// other uids map to an unused host range for system users. systemd runs as
// PID 1 in a delegated cgroup2 subtree, so it can manage its own services and
// the container leaves the app's cgroup (and Android's phantom process killer).
// The container gets its own network namespace: CLONE_NEWNET is part of the
// same clone as CLONE_NEWUSER, so the namespace belongs to the container's
// user namespace and root inside can manage it (Docker needs that). The
// monitor, real root in the host namespace, wires the host side (bridge, veth,
// NAT and the Android policy rules that make the container follow the app's
// own uplink, VPN included) and keeps them alive. The container's root keeps
// the app's supplementary groups (unmapped inside, so shown as nogroup): with
// them it reaches what the app reaches, such as /storage.
//
// Adapted in part from Droidspaces (GPL-3.0-or-later, Copyright (C) 2026
// ravindu644): the /dev layout, devpts and console, the cgroup2 container
// directory and its cleanup, the KernelSU escape guard, the seccomp rule
// against KernelSU's magic reboot, and systemd's init arguments.
// SPDX-License-Identifier: AGPL-3.0-or-later

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/loop.h>
#include <linux/openat2.h>
#include <linux/seccomp.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CG_HOST "/sys/fs/cgroup"
#define CG_ROOT CG_HOST "/dsh-go"
// In the state dir: this boot's id, when dshns turned pids on in the root cgroup.
#define PIDS_MARK "pids.root"
// The container's ceiling on PIDs (threads count too): half of pid_max, at most this.
#define PIDS_CAP 65536
#define OWNERSHIP "/var/lib/dsh-go/ownership.tsv"
// The container storage's mount point in the rootfs, and /home's place on it.
#define STORAGE_DIR "var/lib/dsh-go/storage"
#define HOME_STORE "/" STORAGE_DIR "/home"
// In the rootfs: /home has moved onto the storage (see home_on_storage).
#define HOME_MOVED "/var/lib/dsh-go/home-on-storage"
#define CONSOLE_CAP (1 << 20)
// How long stop waits for a dead container's monitor to finish its teardown.
#define TEARDOWN_WAIT_S 10
// systemd's "SIGRTMIN+3" (halt) in the guest's glibc numbering. Bionic's
// SIGRTMIN is a different number, so never write SIGRTMIN+3 here.
#define SYSTEMD_HALT 37
#define EXIT_FAIL 125
#ifndef EXT4_IOC_RESIZE_FS
#define EXT4_IOC_RESIZE_FS _IOW('f', 16, unsigned long long)
#endif
#if defined(__aarch64__)
#define AUDIT_ARCH_SELF AUDIT_ARCH_AARCH64
#elif defined(__x86_64__)
#define AUDIT_ARCH_SELF AUDIT_ARCH_X86_64
#else
#error "unsupported architecture"
#endif

struct bind {
  const char *host, *guest;
  int ro;
};

static struct {
  const char *cmd, *name, *state, *rootfs, *cwd, *storage;
  unsigned long long storage_size;
  uid_t uid;
  gid_t gid;
  unsigned long base, count;
  gid_t groups[32];
  int ngroups;
  struct bind binds[64];
  int nbinds, timeout;
  int net;             // create a network namespace and wire the host side
  int forward;         // `exec`: host loopback port to forward into the netns
  char **argv;
} opt = {.cwd = "/", .timeout = 60};

static void say(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs("[dshns] ", stderr);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
}

// The write end of `start`'s ready pipe while the monitor sets up: a failure
// there is also the caller's answer.
static int ready_fd = -1;
// What a failing `run` leaves behind for no one else to clear (its cgroups).
static void (*on_die)(void);

static void die(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs("[dshns] ", stderr);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
  if (ready_fd >= 0) {
    va_start(ap, fmt);
    vdprintf(ready_fd, fmt, ap);
    va_end(ap);
  }
  if (on_die)
    on_die();
  exit(EXIT_FAIL);
}

static int write_file(const char *path, const char *text) {
  int fd = open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0)
    return -1;
  ssize_t n = write(fd, text, strlen(text));
  int saved = errno;
  close(fd);
  errno = saved;
  return n == (ssize_t)strlen(text) ? 0 : -1;
}

static int mkdir_p(const char *path, mode_t mode) {
  char buf[4096];
  snprintf(buf, sizeof(buf), "%s", path);
  for (char *p = buf + 1; *p; p++) {
    if (*p != '/')
      continue;
    *p = 0;
    if (mkdir(buf, mode) < 0 && errno != EEXIST)
      return -1;
    *p = '/';
  }
  return mkdir(buf, mode) < 0 && errno != EEXIST ? -1 : 0;
}

static void path_in(char *buf, size_t size, const char *dir, const char *name) {
  snprintf(buf, size, "%s/%s", dir, name);
}

// Runs a program and returns its exit code. Its output goes to stderr: `run`'s
// stdout is the caller's data (a backup).
static int run_with(char *const argv[], char *const envp[]) {
  pid_t pid = fork();
  if (pid == 0) {
    dup2(2, 1);
    execve(argv[0], argv, envp);
    _exit(127);
  }
  int status;
  if (pid < 0 || waitpid(pid, &status, 0) != pid)
    return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

// A host tool (mke2fs, e2fsck, resize2fs).
static int run_tool(char *const argv[]) { return run_with(argv, environ); }

// One of the guest's own programs, from its PID 1 before systemd.
static int guest_tool(char *const argv[]) {
  char *env[] = {"PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LANG=C.UTF-8", NULL};
  return run_with(argv, env);
}

// Field 22 of /proc/PID/stat: tells a reused PID from the process we started.
static unsigned long long starttime(pid_t pid) {
  char path[64], buf[1024];
  snprintf(path, sizeof(path), "/proc/%d/stat", pid);
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return 0;
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0)
    return 0;
  buf[n] = 0;
  char *p = strrchr(buf, ')');
  if (!p)
    return 0;
  unsigned long long value = 0;
  // After ") ": state is field 3, starttime field 22.
  if (sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %*d %*d %llu",
             &value) != 1)
    return 0;
  return value;
}

// The running container's init, or 0. The state file holds "PID STARTTIME".
static pid_t running_init(void) {
  char path[4096], buf[128];
  path_in(path, sizeof(path), opt.state, "init.pid");
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return 0;
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0)
    return 0;
  buf[n] = 0;
  int pid = 0;
  unsigned long long start = 0;
  if (sscanf(buf, "%d %llu", &pid, &start) != 2 || pid <= 0)
    return 0;
  return start && starttime(pid) == start ? pid : 0;
}

// KernelSU: mark this thread so neither su nor GRANT_ROOT can escalate again.
// The su that started us already did (--ksu-no-new-privs); this covers a
// launch without it. Silent on kernels without KernelSU.
static void ksu_guard(void) {
  int fd = -1;
  syscall(__NR_reboot, 0xDEADBEEFu, 0xCAFEBABEu, 0, &fd);
  if (fd < 0)
    return;
  ioctl(fd, _IO('K', 21), 0);
  close(fd);
}

// Deny KernelSU's magic reboot, which installs the [ksu_driver] fd: without it
// nothing in the guest can reach KernelSU's ioctls. Everything else is allowed.
static void seccomp_guard(void) {
  struct sock_filter filter[] = {
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_SELF, 1, 0),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_reboot, 0, 5),
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0])),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0xDEADBEEFu, 0, 3),
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[1])),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0xCAFEBABEu, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  };
  struct sock_fprog prog = {.len = sizeof(filter) / sizeof(filter[0]), .filter = filter};
  if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) < 0)
    say("seccomp guard not installed: %s", strerror(errno));
}

static int open_state(const char *name, int flags);

// cgroup2 ------------------------------------------------------------------

// The first line of a small file, without its newline.
static int read_line(const char *path, char *buf, size_t size) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return -1;
  ssize_t n = read(fd, buf, size - 1);
  close(fd);
  if (n < 0)
    return -1;
  buf[n] = 0;
  buf[strcspn(buf, "\n")] = 0;
  return 0;
}

// Whether the space-separated controller list names NAME.
static int has_controller(const char *list, const char *name) {
  size_t n = strlen(name);
  for (const char *p = list; (p = strstr(p, name)); p += n)
    if ((p == list || p[-1] == ' ') && (p[n] == 0 || p[n] == ' '))
      return 1;
  return 0;
}

static void boot_id(char *buf, size_t size) {
  if (read_line("/proc/sys/kernel/random/boot_id", buf, size) < 0)
    buf[0] = 0;
}

// The pids controller caps the whole container. Turning it on in the host's
// root cgroup gives every top-level Android cgroup a pids counter too, so it is
// on only while a container is: dshns records (with this boot's id) that it was
// the one to turn it on and turns it off again with the last container (see
// cg_release). A +pids someone else wrote (Droidspaces never takes its own
// back) is theirs and stays.
static void pids_enable(void) {
  char list[256], boot[64], mark[4096], had[64];
  if (read_line(CG_HOST "/cgroup.controllers", list, sizeof(list)) < 0 || !has_controller(list, "pids"))
    die("no pids controller on cgroup2: the kernel lacks CONFIG_CGROUP_PIDS, or a cgroup v1 hierarchy holds it");
  boot_id(boot, sizeof(boot));
  path_in(mark, sizeof(mark), opt.state, PIDS_MARK);
  if (read_line(mark, had, sizeof(had)) == 0 && strcmp(had, boot) != 0)
    unlink(mark);
  if (read_line(CG_HOST "/cgroup.subtree_control", list, sizeof(list)) < 0)
    die("cannot read " CG_HOST "/cgroup.subtree_control: %s", strerror(errno));
  if (!has_controller(list, "pids")) {
    if (write_file(CG_HOST "/cgroup.subtree_control", "+pids") < 0)
      die("cannot enable pids in the root cgroup: %s", strerror(errno));
    int fd = open_state(PIDS_MARK, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd >= 0) {
      dprintf(fd, "%s\n", boot);
      close(fd);
    }
  }
  if (write_file(CG_ROOT "/cgroup.subtree_control", "+pids") < 0)
    die("cannot enable pids in " CG_ROOT ": %s", strerror(errno));
}

// CG_ROOT once nothing is in it (it stays while another container runs), then
// the root cgroup's pids if dshns turned it on this boot.
static void cg_release(void) {
  if (rmdir(CG_ROOT) < 0 && errno != ENOENT)
    return;
  char mark[4096], had[64], boot[64];
  path_in(mark, sizeof(mark), opt.state, PIDS_MARK);
  if (read_line(mark, had, sizeof(had)) < 0)
    return;
  boot_id(boot, sizeof(boot));
  // EBUSY: someone else's cgroup below the root delegates pids now; theirs.
  if (!strcmp(had, boot) && write_file(CG_HOST "/cgroup.subtree_control", "-pids") < 0 && errno != EBUSY)
    say("cannot disable pids in the root cgroup: %s", strerror(errno));
  unlink(mark);
}

static void cg_dir(char *buf, size_t size, const char *leaf) {
  snprintf(buf, size, CG_ROOT "/%s%s%s", opt.name, leaf ? "/" : "", leaf ? leaf : "");
}

static int cg_join(const char *dir, pid_t pid) {
  char path[4096], value[32];
  path_in(path, sizeof(path), dir, "cgroup.procs");
  snprintf(value, sizeof(value), "%d", pid);
  return write_file(path, value);
}

static void cg_rmdir_tree(const char *path) {
  DIR *d = opendir(path);
  if (d) {
    struct dirent *e;
    while ((e = readdir(d))) {
      if (e->d_name[0] == '.' || e->d_type != DT_DIR)
        continue;
      char child[4096];
      path_in(child, sizeof(child), path, e->d_name);
      cg_rmdir_tree(child);
    }
    closedir(d);
  }
  for (int i = 0; i < 50 && rmdir(path) < 0 && errno == EBUSY; i++)
    usleep(20000);
}

// Kill whatever is left (cgroup.kill, 5.14+), wait until empty, remove bottom-up.
static void cg_remove(const char *dir) {
  char path[4096], buf[256];
  if (access(dir, F_OK) != 0)
    return;
  path_in(path, sizeof(path), dir, "cgroup.kill");
  write_file(path, "1");
  path_in(path, sizeof(path), dir, "cgroup.events");
  for (int i = 0; i < 100; i++) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
      break;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
      break;
    buf[n] = 0;
    if (strstr(buf, "populated 0"))
      break;
    usleep(20000);
  }
  cg_rmdir_tree(dir);
}

// The monitor's cgroup, then CG_ROOT (see cg_release).
static void cg_tidy(void) {
  char dir[4096];
  snprintf(dir, sizeof(dir), CG_ROOT "/monitor-%s", opt.name);
  for (int i = 0; i < 20 && rmdir(dir) < 0 && errno == EBUSY; i++)
    usleep(50000);
  cg_release();
}

// A fresh cgroup for this boot, capped in PIDs and delegated to the container's
// root: systemd creates its slices below it, and cannot raise the cap.
static void cg_prepare(void) {
  char dir[4096], path[4096], value[32];
  if (mkdir(CG_ROOT, 0755) < 0 && errno != EEXIST)
    die("cannot create " CG_ROOT ": %s", strerror(errno));
  pids_enable();
  cg_dir(dir, sizeof(dir), NULL);
  cg_remove(dir);
  if (mkdir(dir, 0755) < 0)
    die("cannot create %s: %s", dir, strerror(errno));
  long limit = read_line("/proc/sys/kernel/pid_max", value, sizeof(value)) == 0 ? atol(value) / 2 : 0;
  if (limit > PIDS_CAP)
    limit = PIDS_CAP;
  snprintf(value, sizeof(value), "%ld", limit);
  path_in(path, sizeof(path), dir, "pids.max");
  if (limit <= 0 || write_file(path, value) < 0) {
    int saved = errno;
    rmdir(dir);
    die("cannot cap %s at %ld PIDs: %s", dir, limit, strerror(saved));
  }
  const char *delegated[] = {"", "cgroup.procs", "cgroup.threads", "cgroup.subtree_control"};
  for (size_t i = 0; i < sizeof(delegated) / sizeof(delegated[0]); i++) {
    snprintf(path, sizeof(path), "%s%s%s", dir, delegated[i][0] ? "/" : "", delegated[i]);
    if (chown(path, opt.uid, opt.gid) < 0)
      die("cannot delegate %s: %s", path, strerror(errno));
  }
}

// Mounts --------------------------------------------------------------------

static int do_mount(const char *src, const char *dst, const char *type, unsigned long flags, const char *data) {
  if (mount(src, dst, type, flags, data) == 0)
    return 0;
  say("mount %s on %s (%s): %s", src ? src : "-", dst, type ? type : "bind", strerror(errno));
  return -1;
}

// Read-only remount of a bind keeps the flags the source mount locked in.
static int remount_ro(const char *dst) {
  struct statvfs sv;
  unsigned long flags = MS_REMOUNT | MS_BIND | MS_RDONLY;
  if (statvfs(dst, &sv) == 0) {
    if (sv.f_flag & ST_NOSUID)
      flags |= MS_NOSUID;
    if (sv.f_flag & ST_NODEV)
      flags |= MS_NODEV;
    if (sv.f_flag & ST_NOEXEC)
      flags |= MS_NOEXEC;
    if (sv.f_flag & ST_NOATIME)
      flags |= MS_NOATIME;
    if (sv.f_flag & ST_NODIRATIME)
      flags |= MS_NODIRATIME;
    if (sv.f_flag & ST_RELATIME)
      flags |= MS_RELATIME;
  }
  return do_mount(NULL, dst, NULL, flags, NULL);
}

static int touch(const char *path) {
  int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0)
    return -1;
  close(fd);
  return 0;
}

// Bind a host path at a guest path (relative to the rootfs, which is the cwd).
static int bind_into(const char *host, const char *guest, int ro) {
  char target[4096];
  struct stat st;
  snprintf(target, sizeof(target), ".%s", guest);
  if (stat(host, &st) < 0) {
    say("skip bind %s (missing)", host);
    return 0;
  }
  if (S_ISDIR(st.st_mode)) {
    if (mkdir_p(target, 0755) < 0)
      return -1;
  } else {
    char parent[4096];
    snprintf(parent, sizeof(parent), "%s", target);
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) {
      *slash = 0;
      mkdir_p(parent, 0755);
    }
    if (access(target, F_OK) != 0 && touch(target) < 0)
      return -1;
  }
  if (do_mount(host, target, NULL, MS_BIND | MS_REC, NULL) < 0)
    return -1;
  return ro ? remount_ro(target) : 0;
}

// Devices the container gets nodes of its own for. The host's belong to users
// it cannot map (/dev/fuse is root's 0600, /dev/tun system:vpn 0660), so not
// even its root could open them, and they stay as Android set them. The
// monitor, real root, makes these on a tmpfs in its own namespace, outside the
// rootfs (a mount made in the container's user namespace, like its /dev, is
// nodev), owned by the container's root and open to all its users, as in an
// unprivileged LXC container: a tun device can only be made in the
// container's own netns, and a FUSE mount is seen only in the container.
static const struct {
  const char *name;
  unsigned major, minor;
} OWN_NODES[] = {{"fuse", 10, 229}, {"net/tun", 10, 200}};
// That tmpfs, state/dev in the monitor's namespace; "" without one.
static char own_dev[4096];

static void make_nodes(void) {
  path_in(own_dev, sizeof(own_dev), opt.state, "dev");
  if ((mkdir(own_dev, 0755) < 0 && errno != EEXIST) || chown(own_dev, opt.uid, opt.gid) < 0 ||
      mount("tmpfs", own_dev, "tmpfs", MS_NOSUID | MS_NOEXEC, "mode=755,size=16k") < 0) {
    say("no device nodes of the container's own (%s: %s)", own_dev, strerror(errno));
    own_dev[0] = 0;
    return;
  }
  for (size_t i = 0; i < sizeof(OWN_NODES) / sizeof(OWN_NODES[0]); i++) {
    char path[4096];
    path_in(path, sizeof(path), own_dev, OWN_NODES[i].name);
    char *slash = strrchr(path, '/');
    *slash = 0;
    mkdir(path, 0755);
    *slash = '/';
    if (mknod(path, S_IFCHR | 0666, makedev(OWN_NODES[i].major, OWN_NODES[i].minor)) < 0 || chmod(path, 0666) < 0 ||
        chown(path, opt.uid, opt.gid) < 0) {
      say("no /dev/%s in the container: %s", OWN_NODES[i].name, strerror(errno));
      unlink(path);
    }
  }
}

// /dev for a rootless container: a tmpfs with the host's nodes bound in (the
// guest cannot mknod) and the monitor's own ones (make_nodes), a private
// devpts, and a console whose master goes to the monitor.
static int setup_dev(int console_sock) {
  if (do_mount("tmpfs", "dev", "tmpfs", MS_NOSUID | MS_NOEXEC, "mode=755,size=8m") < 0)
    return -1;
  const char *nodes[] = {"null", "zero", "full", "random", "urandom", "tty"};
  for (size_t i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
    char host[64], guest[64];
    snprintf(host, sizeof(host), "/dev/%s", nodes[i]);
    snprintf(guest, sizeof(guest), "/dev/%s", nodes[i]);
    if (access(host, F_OK) != 0)
      continue;
    if (bind_into(host, guest, 0) < 0)
      return -1;
  }
  // A node it cannot open would only fail with EACCES; without one, many
  // programs find another way.
  for (size_t i = 0; own_dev[0] && i < sizeof(OWN_NODES) / sizeof(OWN_NODES[0]); i++) {
    char node[4096], guest[64];
    path_in(node, sizeof(node), own_dev, OWN_NODES[i].name);
    snprintf(guest, sizeof(guest), "/dev/%s", OWN_NODES[i].name);
    if (access(node, F_OK) == 0 && bind_into(node, guest, 0) < 0) {
      snprintf(node, sizeof(node), ".%s", guest);
      unlink(node);
    }
  }
  mkdir("dev/pts", 0755);
  if (do_mount("devpts", "dev/pts", "devpts", MS_NOSUID | MS_NOEXEC, "newinstance,gid=5,ptmxmode=0666,mode=0620") < 0 &&
      do_mount("devpts", "dev/pts", "devpts", MS_NOSUID | MS_NOEXEC, "newinstance,ptmxmode=0666,mode=0620") < 0)
    return -1;
  if (touch("dev/ptmx") < 0 || do_mount("dev/pts/ptmx", "dev/ptmx", NULL, MS_BIND, NULL) < 0)
    return -1;
  mkdir("dev/shm", 01777);
  do_mount("tmpfs", "dev/shm", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777");
  mkdir("dev/mqueue", 0755);
  do_mount("mqueue", "dev/mqueue", "mqueue", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
  symlink("/proc/self/fd", "dev/fd");
  symlink("/proc/self/fd/0", "dev/stdin");
  symlink("/proc/self/fd/1", "dev/stdout");
  symlink("/proc/self/fd/2", "dev/stderr");
  symlink("/proc/kcore", "dev/core");

  // Console: a pty from our own devpts, slave bound at /dev/console.
  int master = open("dev/pts/ptmx", O_RDWR | O_NOCTTY | O_CLOEXEC);
  if (master < 0 || grantpt(master) < 0 || unlockpt(master) < 0)
    return say("console pty: %s", strerror(errno)), -1;
  int n = 0;
  if (ioctl(master, TIOCGPTN, &n) < 0)
    return -1;
  char slave[64];
  snprintf(slave, sizeof(slave), "dev/pts/%d", n);
  if (touch("dev/console") < 0 || do_mount(slave, "dev/console", NULL, MS_BIND, NULL) < 0)
    return -1;
  struct winsize ws = {.ws_row = 24, .ws_col = 120};
  ioctl(master, TIOCSWINSZ, &ws);
  char dummy = 'c';
  struct iovec iov = {.iov_base = &dummy, .iov_len = 1};
  char control[CMSG_SPACE(sizeof(int))];
  struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
  struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
  c->cmsg_level = SOL_SOCKET;
  c->cmsg_type = SCM_RIGHTS;
  c->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(c), &master, sizeof(int));
  if (sendmsg(console_sock, &msg, 0) < 0)
    say("console handoff: %s", strerror(errno));
  close(master);
  return 0;
}

// The image ships some files owned by system users and with setuid bits; the
// app extracted everything as its own uid. Container root fixes that once.
static void apply_ownership(void) {
  FILE *f = fopen(OWNERSHIP, "re");
  if (!f)
    return;
  char line[4096];
  int fixed = 0;
  while (fgets(line, sizeof(line), f)) {
    unsigned uid, gid, mode;
    int at = 0;
    if (sscanf(line, "%u %u %o %n", &uid, &gid, &mode, &at) != 3 || !at)
      continue;
    char *path = line + at;
    path[strcspn(path, "\n")] = 0;
    char abs[4096];
    snprintf(abs, sizeof(abs), "/%s", path);
    struct stat st;
    if (lstat(abs, &st) < 0)
      continue;
    if (lchown(abs, uid, gid) < 0)
      say("chown %s: %s", abs, strerror(errno));
    if (!S_ISLNK(st.st_mode) && chmod(abs, mode) < 0)
      say("chmod %s: %s", abs, strerror(errno));
    fixed++;
  }
  fclose(f);
  rename(OWNERSHIP, OWNERSHIP ".done");
  say("first boot: set owner and mode of %d image entries", fixed);
}

// /home lives on the container storage, by Docker's and Podman's data: ext4,
// where rootless Podman gets the kernel's own overlay. The first boot on a
// storage without one moves the rootfs's /home there, as the container's root
// and in its own paths; HOME_MOVED in the rootfs remembers that, so a storage
// that lost its /home fails the start instead of booting an empty one.
// Container PID 1, after pivot_root; NULL, or why there is no /home.
static const char *home_on_storage(void) {
  static char why[256];
  struct stat st;
  if (stat(HOME_STORE, &st) < 0) {
    if (errno != ENOENT)
      return snprintf(why, sizeof(why), "%s: %s", HOME_STORE, strerror(errno)), why;
    if (access(HOME_MOVED, F_OK) == 0)
      return "the container storage has lost /home: restore a backup, or delete the system container";
    char *clear[] = {"/usr/bin/rm", "-rf", "--one-file-system", "--", HOME_STORE ".partial", NULL};
    char *copy[] = {"/usr/bin/cp", "-a", "--", "/home", HOME_STORE ".partial", NULL};
    int rc = guest_tool(clear);
    if (rc == 0)
      rc = guest_tool(copy);
    if (rc != 0)
      return snprintf(why, sizeof(why), "copying /home to the container storage failed (exit %d)", rc), why;
    // The copy on the storage for good before the rename says it is whole (the
    // journal may commit a rename ahead of data still in delayed allocation),
    // and the rename before the original goes: they are two filesystems.
    int fd = open(HOME_STORE ".partial", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0 || syscall(__NR_syncfs, fd) < 0 || rename(HOME_STORE ".partial", HOME_STORE) < 0 ||
        syscall(__NR_syncfs, fd) < 0)
      return snprintf(why, sizeof(why), "%s: %s", HOME_STORE, strerror(errno)), why;
    close(fd);
    char *drop[] = {"/usr/bin/rm", "-rf", "--one-file-system", "--", "/home", NULL};
    if (guest_tool(drop) != 0)
      say("the rootfs keeps its old /home, hidden below the storage's");
    say("/home moved to the container storage");
  }
  if (touch(HOME_MOVED) < 0)
    say("%s: %s", HOME_MOVED, strerror(errno));
  mkdir("/home", 0755);
  if (mount(HOME_STORE, "/home", NULL, MS_BIND, NULL) < 0)
    return snprintf(why, sizeof(why), "bind /home: %s", strerror(errno)), why;
  return NULL;
}

// start ---------------------------------------------------------------------

struct child_pipes {
  int go, go_writer, booted, console;
};

static void child_abort(int booted, const char *msg) {
  write(booted, msg, strlen(msg));
  _exit(EXIT_FAIL);
}

static void child_fail(int booted, const char *what) {
  char msg[512];
  snprintf(msg, sizeof(msg), "%s: %s", what, strerror(errno));
  child_abort(booted, msg);
}

static void child_network(void);

// PID 1 of the new namespaces. Waits for the monitor to write the uid maps and
// move it into its cgroup, builds the root, then execs systemd.
static int container_main(void *arg) {
  struct child_pipes *p = arg;
  char go;
  close(p->go_writer);
  if (read(p->go, &go, 1) != 1)
    _exit(EXIT_FAIL);
  close(p->go);
  // Keeps the supplementary groups the monitor set.
  if (setresgid(0, 0, 0) < 0 || setresuid(0, 0, 0) < 0)
    child_fail(p->booted, "become container root");
  if (unshare(CLONE_NEWCGROUP) < 0)
    child_fail(p->booted, "cgroup namespace");
  sethostname("dsh-go", 6);
  // Mounts copied in from the monitor are locked, and pivot_root refuses a
  // locked new root: bind the monitor's (suid) rootfs mount again, here.
  if (mount(opt.rootfs, opt.rootfs, NULL, MS_BIND | MS_REC, NULL) < 0 || chdir(opt.rootfs) < 0)
    child_fail(p->booted, "bind rootfs");

  mkdir("proc", 0555);
  if (do_mount("proc", "proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) < 0)
    child_fail(p->booted, "mount /proc");
  // The host's sysfs, read-only (the guest cannot mount its own while it shares
  // the host network). Recursive: a plain bind of a mount with locked children
  // is refused inside a user namespace.
  mkdir("sys", 0555);
  if (do_mount("/sys", "sys", NULL, MS_BIND | MS_REC, NULL) < 0)
    child_fail(p->booted, "bind /sys");
  do_mount("tmpfs", "sys/fs/selinux", "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, "size=4k,mode=755");
  remount_ro("sys");
  // The bound sys/fs/cgroup is the host's cgroup2 superblock, and the kernel
  // refuses a filesystem on its own mount point (EBUSY): put a layer between.
  do_mount("tmpfs", "sys/fs/cgroup", "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, "size=4k,mode=755");
  if (do_mount("cgroup2", "sys/fs/cgroup", "cgroup2", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) < 0)
    child_fail(p->booted, "mount cgroup2");
  if (setup_dev(p->console) < 0)
    child_fail(p->booted, "set up /dev");
  close(p->console);
  mkdir("run", 0755);
  if (do_mount("tmpfs", "run", "tmpfs", MS_NOSUID | MS_NODEV, "mode=755") < 0)
    child_fail(p->booted, "mount /run");
  for (int i = 0; i < opt.nbinds; i++)
    if (bind_into(opt.binds[i].host, opt.binds[i].guest, opt.binds[i].ro) < 0)
      child_fail(p->booted, opt.binds[i].host);

  mkdir(".old_root", 0700);
  if (syscall(SYS_pivot_root, ".", ".old_root") < 0)
    child_fail(p->booted, "pivot_root");
  if (chdir("/") < 0 || umount2("/.old_root", MNT_DETACH) < 0)
    child_fail(p->booted, "detach old root");
  rmdir("/.old_root");
  apply_ownership();
  // Before running any of the guest's programs.
  seccomp_guard();
  const char *home = opt.storage ? home_on_storage() : NULL;
  if (home)
    child_abort(p->booted, home);
  if (opt.net)
    child_network();

  char *env[] = {"container=dsh-go", "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
                 "TERM=linux", "HOME=/root", "LANG=C.UTF-8", NULL};
  if (opt.argv) {
    // `dshns run`: the command is PID 1, on the caller's stdio.
    execve(opt.argv[0], opt.argv, env);
    child_fail(p->booted, opt.argv[0]);
  }
  int console = open("/dev/console", O_RDWR | O_NOCTTY | O_CLOEXEC);
  if (console >= 0) {
    setsid();
    ioctl(console, TIOCSCTTY, 0);
    dup2(console, 0);
    dup2(console, 1);
    dup2(console, 2);
    if (console > 2)
      close(console);
  }
  env[3] = NULL;
  char *argv[] = {"/sbin/init", "systemd.unified_cgroup_hierarchy=1", NULL};
  execve(argv[0], argv, env);
  child_fail(p->booted, "exec /sbin/init");
  return EXIT_FAIL;
}

static int write_maps(pid_t pid) {
  char path[64], map[128];
  snprintf(map, sizeof(map), "0 %u 1\n1 %lu %lu\n", opt.uid, opt.base, opt.count);
  snprintf(path, sizeof(path), "/proc/%d/uid_map", pid);
  if (write_file(path, map) < 0)
    return -1;
  snprintf(map, sizeof(map), "0 %u 1\n1 %lu %lu\n", opt.gid, opt.base, opt.count);
  snprintf(path, sizeof(path), "/proc/%d/gid_map", pid);
  return write_file(path, map);
}

// A mount namespace of the monitor's own, with the rootfs bound onto itself
// and the container's device nodes (make_nodes). /data is nosuid; this bind
// is not, so the image's setuid programs (sudo, su) work for the container's
// users. It is never visible outside: the host keeps its nosuid /data, and the
// container's suid root is the app's uid here.
static void private_rootfs(void) {
  if (unshare(CLONE_NEWNS) < 0)
    die("mount namespace: %s", strerror(errno));
  if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
    die("make / private: %s", strerror(errno));
  if (mount(opt.rootfs, opt.rootfs, NULL, MS_BIND | MS_REC, NULL) < 0)
    die("bind rootfs: %s", strerror(errno));
  if (mount(NULL, opt.rootfs, NULL, MS_REMOUNT | MS_BIND | MS_NODEV | MS_NOATIME, NULL) < 0)
    say("rootfs stays nosuid: %s", strerror(errno));
  make_nodes();
}

static pid_t spawn(int *console_sock);

// Storage ------------------------------------------------------------------
//
// Docker and Podman need overlayfs, which refuses the app's /data (f2fs with
// casefold), so their state, and /home for rootless Podman's (see
// home_on_storage), lives on an ext4 image: sparse, created on first
// start, attached to a loop device that clears itself when the last mount
// goes, mounted with discard so that what the container deletes is handed
// back to /data. The mount is made in the monitor's namespace and copied into
// the container's; nothing outside sees it.

#define STORAGE_BLOCK 4096

// The filesystem's size in bytes from its superblock, or -1.
static long long fs_bytes(int img) {
  unsigned char sb[1024];
  if (pread(img, sb, sizeof(sb), 1024) != (ssize_t)sizeof(sb) || sb[0x38] != 0x53 || sb[0x39] != 0xEF)
    return -1;
  unsigned long long lo = sb[0x04] | sb[0x05] << 8 | sb[0x06] << 16 | (unsigned long long)sb[0x07] << 24;
  unsigned long long hi = sb[0x150] | sb[0x151] << 8 | sb[0x152] << 16 | (unsigned long long)sb[0x153] << 24;
  unsigned shift = 10 + (sb[0x18] | sb[0x19] << 8);
  return (long long)((hi << 32 | lo) << shift);
}

// A new sparse image: the container's root owns its root directory.
static void storage_create(void) {
  int img = open(opt.storage, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (img < 0)
    die("storage %s: %s", opt.storage, strerror(errno));
  if (ftruncate(img, opt.storage_size) < 0 || fchown(img, opt.uid, opt.gid) < 0) {
    int saved = errno;
    unlink(opt.storage);
    die("storage %s: %s", opt.storage, strerror(saved));
  }
  close(img);
  char owner[160];
  snprintf(owner, sizeof(owner), "root_owner=%u:%u,lazy_itable_init=1,lazy_journal_init=1,nodiscard", opt.uid, opt.gid);
  char *argv[] = {"/system/bin/mke2fs", "-q", "-t", "ext4", "-b", "4096", "-m", "0", "-L", "dsh-storage",
                  "-E", owner, (char *)opt.storage, NULL};
  int rc = run_tool(argv);
  if (rc != 0) {
    unlink(opt.storage);
    die("mke2fs %s failed (%d)", opt.storage, rc);
  }
  say("storage %s created (%llu bytes, sparse)", opt.storage, opt.storage_size);
}

// A free loop device bound to IMG, or -1. The node of a new device comes from
// ueventd, a moment after LOOP_CTL_GET_FREE.
static int loop_attach(int img, char *dev, size_t size) {
  int ctl = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
  if (ctl < 0)
    return -1;
  int fd = -1;
  for (int attempt = 0; attempt < 10 && fd < 0; attempt++) {
    int n = ioctl(ctl, LOOP_CTL_GET_FREE);
    if (n < 0)
      break;
    snprintf(dev, size, "/dev/block/loop%d", n);
    for (int i = 0; i < 40 && (fd = open(dev, O_RDWR | O_CLOEXEC)) < 0 && errno == ENOENT; i++)
      usleep(50000);
    if (fd < 0)
      break;
    struct loop_config config = {.fd = img, .block_size = STORAGE_BLOCK};
    config.info.lo_flags = LO_FLAGS_AUTOCLEAR | LO_FLAGS_DIRECT_IO;
    int r = ioctl(fd, LOOP_CONFIGURE, &config);
    if (r < 0 && errno == EINVAL) {
      config.info.lo_flags &= ~LO_FLAGS_DIRECT_IO;
      r = ioctl(fd, LOOP_CONFIGURE, &config);
    }
    if (r < 0) {
      int saved = errno;
      close(fd);
      fd = -1;
      // Taken by someone else in between: ask for the next free one.
      if (saved != EBUSY) {
        errno = saved;
        break;
      }
    }
  }
  close(ctl);
  return fd;
}

// Opens REL under ROOT as if ROOT were `/` (O_PATH). What lies below ROOT is
// the container's to change: a symlink there must never lead this process,
// real root on the host, out of it.
static int open_in(int root, const char *rel) {
  struct open_how how = {.flags = O_PATH | O_DIRECTORY | O_CLOEXEC,
                         .resolve = RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS};
  return (int)syscall(__NR_openat2, root, rel, &how, sizeof(how));
}

// A directory REL under ROOT, made (MODE) where missing and owned by the
// container's root; an O_PATH fd for it, resolved inside ROOT only.
static int dir_in(int root, const char *rel, mode_t mode) {
  char path[4096];
  snprintf(path, sizeof(path), "%s", rel);
  for (char *p = path;; p++) {
    if (*p != '/' && *p)
      continue;
    char saved = *p;
    *p = 0;
    if (path[0]) {
      int fd = open_in(root, path);
      if (fd < 0 && errno == ENOENT) {
        char *slash = strrchr(path, '/');
        int parent = slash ? (*slash = 0, open_in(root, path)) : open_in(root, ".");
        if (slash)
          *slash = '/';
        if (parent < 0)
          return -1;
        int made = mkdirat(parent, slash ? slash + 1 : path, mode);
        close(parent);
        if (made < 0 && errno != EEXIST)
          return -1;
        fd = open_in(root, path);
      }
      if (fd < 0)
        return -1;
      if (!saved) {
        if (fchownat(fd, "", opt.uid, opt.gid, AT_EMPTY_PATH) < 0) {
          close(fd);
          return -1;
        }
        return fd;
      }
      close(fd);
    }
    *p = saved;
    if (!saved)
      return -1;
  }
}

static void fd_path(char *buf, size_t size, int fd) { snprintf(buf, size, "/proc/self/fd/%d", fd); }

// Creates, checks, grows (after `dshns grow` while stopped) and mounts the
// image, then binds its docker, containerd (Docker's image store) and
// containers (Podman's) directories in. Monitor only; /home is the
// container's to bind (home_on_storage).
static void storage_attach(void) {
  if (access(opt.storage, F_OK) != 0)
    storage_create();
  char *fsck[] = {"/system/bin/e2fsck", "-p", (char *)opt.storage, NULL};
  int rc = run_tool(fsck);
  if (rc < 0 || rc > 1)
    die("e2fsck %s: exit %d; the storage needs repair", opt.storage, rc);
  int img = open(opt.storage, O_RDWR | O_CLOEXEC);
  struct stat st;
  if (img < 0 || fstat(img, &st) < 0)
    die("storage %s: %s", opt.storage, strerror(errno));
  long long have = fs_bytes(img);
  if (have < 0)
    die("storage %s holds no ext4", opt.storage);
  if (st.st_size >= have + STORAGE_BLOCK) {
    char *force[] = {"/system/bin/e2fsck", "-f", "-p", (char *)opt.storage, NULL};
    char *resize[] = {"/system/bin/resize2fs", (char *)opt.storage, NULL};
    int checked = run_tool(force);
    int resized = checked == 0 || checked == 1 ? run_tool(resize) : -1;
    if (resized != 0)
      say("storage stays at %lld bytes: e2fsck -f exit %d, resize2fs exit %d", have, checked, resized);
  }
  char dev[64], path[64], where[64];
  int loop = loop_attach(img, dev, sizeof(dev));
  close(img);
  if (loop < 0)
    die("no loop device for %s: %s", opt.storage, strerror(errno));
  int root = open(opt.rootfs, O_PATH | O_DIRECTORY | O_CLOEXEC);
  int mnt = root < 0 ? -1 : dir_in(root, STORAGE_DIR, 0755);
  if (mnt < 0)
    die("cannot create %s in the rootfs: %s", STORAGE_DIR, strerror(errno));
  fd_path(where, sizeof(where), mnt);
  if (mount(dev, where, "ext4", MS_NODEV | MS_NOATIME, "discard") < 0)
    die("mount %s on %s: %s", dev, STORAGE_DIR, strerror(errno));
  close(mnt);
  // The mount holds the device now; closing ours lets it clear with the mount.
  close(loop);
  int state = open_state("storage.dev", O_WRONLY | O_CREAT | O_TRUNC);
  if (state >= 0) {
    dprintf(state, "%s\n", dev);
    close(state);
  }
  const char *users[][2] = {{"docker", "var/lib/docker"}, {"containerd", "var/lib/containerd"},
                            {"containers", "var/lib/containers"}};
  // The storage's own root now; what it holds is the container's too.
  int top = open_in(root, STORAGE_DIR);
  if (top < 0)
    die("open %s: %s", STORAGE_DIR, strerror(errno));
  for (size_t i = 0; i < sizeof(users) / sizeof(users[0]); i++) {
    int inner = dir_in(top, users[i][0], 0710);
    int target = dir_in(root, users[i][1], 0710);
    if (inner < 0 || target < 0)
      die("cannot create %s: %s", users[i][1], strerror(errno));
    fd_path(path, sizeof(path), inner);
    fd_path(where, sizeof(where), target);
    if (mount(path, where, NULL, MS_BIND, NULL) < 0)
      die("bind %s: %s", users[i][1], strerror(errno));
    close(inner);
    close(target);
  }
  close(top);
  close(root);
  say("storage %s on %s", opt.storage, dev);
}

static int open_state(const char *name, int flags) {
  char path[4096];
  path_in(path, sizeof(path), opt.state, name);
  int fd = open(path, flags | O_CLOEXEC, 0644);
  if (fd >= 0)
    fchown(fd, opt.uid, opt.gid);
  return fd;
}

// Network -------------------------------------------------------------------
//
// The container's netns is created by the clone in spawn(); the monitor (real
// root, host netns) does the host plumbing while the child waits for its `go`
// pipe, and the child configures eth0 after pivot_root. `exec` joins the netns;
// `--forward PORT` maps the app's 127.0.0.1:PORT onto the engine's port inside
// the netns, because the app keeps its loopback origin (a secure context).
//
// The policy rules use the app's own routing table: scan `ip rule` for the
// first `iif lo` rule covering the app uid (a VPN's per-uid rule when one is
// active) and point the container's traffic at that table, so it follows the
// same uplink as the app. Without a matching uid rule, netd's default-network
// rule (fwmark 0x0/0xffff) is used. The monitor re-checks every two seconds,
// which also repairs whatever netd flushes.
//
// IPv6 is the same, on a ULA subnet NATed to the uplink's address: the rules,
// the policy and the forwarding switch are made once per family.

#define NET_BRIDGE "dsh-br0"
#define NET_VETH "dsh-v0"
#define NET_PEER "dsh-p0"
#define NET_GW "10.213.0.1"
#define NET_IP "10.213.0.2"
#define NET_PREFIX 16
#define NET_SUBNET "10.213.0.0/16"
#define NET6_GW "fd10:213::1"
#define NET6_IP "fd10:213::2"
#define NET6_PREFIX 64
#define NET6_SUBNET "fd10:213::/64"
#define NET_RULE_TO "6060"
#define NET_RULE_FROM "6070"
#define IP_BIN "/system/bin/ip"
#define IPT_BIN "/system/bin/iptables"
#define IP6T_BIN "/system/bin/ip6tables"

struct family {
  const char *label;    // in log lines
  const char *ip;       // ip's family flag
  const char *ipt;      // iptables or ip6tables
  const char *subnet, *gw;
  const char *forward;  // the forwarding switch
  const char *orig;     // in the state dir: the switch before a container first turned it on
};
static const struct family NET_FAMILIES[] = {
    {"", "-4", IPT_BIN, NET_SUBNET, NET_GW, "/proc/sys/net/ipv4/ip_forward", "ip_forward.orig"},
    {"IPv6 ", "-6", IP6T_BIN, NET6_SUBNET, NET6_GW, "/proc/sys/net/ipv6/conf/all/forwarding", "ip6_forward.orig"},
};
#define NET_FAMILY_COUNT (sizeof(NET_FAMILIES) / sizeof(NET_FAMILIES[0]))

// Runs a host tool with no output and returns its exit status (-1 if it did
// not run at all). The platform's own ip/iptables, never a shell.
static int run_quiet(char *const argv[]) {
  pid_t pid = fork();
  if (pid == 0) {
    int null = open("/dev/null", O_RDWR);
    if (null >= 0) {
      dup2(null, 1);
      dup2(null, 2);
      if (null > 2)
        close(null);
    }
    execv(argv[0], argv);
    _exit(127);
  }
  int status;
  if (pid < 0 || waitpid(pid, &status, 0) != pid)
    return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

// Runs a host tool and captures stdout (`ip rule` only).
static int capture(char *const argv[], char *out, size_t size) {
  int fds[2];
  if (pipe2(fds, O_CLOEXEC) < 0)
    return -1;
  pid_t pid = fork();
  if (pid == 0) {
    dup2(fds[1], 1);
    int null = open("/dev/null", O_WRONLY);
    if (null >= 0) {
      dup2(null, 2);
      if (null > 2)
        close(null);
    }
    close(fds[0]);
    close(fds[1]);
    execv(argv[0], argv);
    _exit(127);
  }
  close(fds[1]);
  size_t used = 0;
  for (;;) {
    ssize_t n = read(fds[0], out + used, size - 1 - used);
    if (n <= 0)
      break;
    used += (size_t)n;
    if (used >= size - 1)
      break;
  }
  out[used] = 0;
  close(fds[0]);
  int status;
  if (pid < 0 || waitpid(pid, &status, 0) != pid)
    return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

static void net_command_failed(const char *what) { say("[net] %s failed", what); }

// What the app's own traffic gets: the first `iif lo` rule covering the app
// uid (a VPN's per-uid rule), else netd's default-network rule. A table name;
// "!prohibit" (or "!unreachable", "!blackhole") when that first rule blocks the
// app, as always-on VPN's lockdown does while the VPN is down, so that the
// container is blocked too; empty when no route to the WAN was found.
static void app_table_from(const char *buf, char *out, size_t size) {
  out[0] = 0;
  char fallback[64] = "";
  const char *p = buf;
  while (p && *p) {
    const char *e = strchr(p, '\n');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    char line[1024];
    if (len >= sizeof(line))
      len = sizeof(line) - 1;
    memcpy(line, p, len);
    line[len] = 0;
    p = e ? e + 1 : NULL;
    if (!strstr(line, "iif lo") || strstr(line, " oif "))
      continue;
    const char *block = strstr(line, "unreachable") ? "unreachable"
                        : strstr(line, "prohibit")  ? "prohibit"
                        : strstr(line, "blackhole") ? "blackhole"
                                                    : NULL;
    const char *lookup = strstr(line, "lookup ");
    char table[64] = "";
    if (lookup && (sscanf(lookup + 7, "%63s", table) != 1 || !table[0]))
      continue;
    const char *ur = strstr(line, "uidrange ");
    if (ur) {
      unsigned lo = 0, hi = 0;
      if (sscanf(ur + 9, "%u-%u", &lo, &hi) == 2 && lo <= opt.uid && opt.uid <= hi) {
        if (block) {
          snprintf(out, size, "!%s", block);
          return;
        }
        if (table[0]) {
          snprintf(out, size, "%s", table);
          return;
        }
      }
      continue;
    }
    if (block || !table[0])
      continue;
    if (!fallback[0] && strstr(line, "fwmark 0x0/0xffff"))
      snprintf(fallback, sizeof(fallback), "%s", table);
  }
  snprintf(out, size, "%s", fallback);
}

// iptables rules, idempotent: check with -C, insert only when missing.
static void ipt_ensure(char *const check[], char *const add[]) {
  if (run_quiet(check) == 0)
    return;
  if (run_quiet(add) != 0)
    net_command_failed("iptables");
}

// Removes a rule when it is there.
static void ipt_remove(char *const check[], char *const del[]) {
  if (run_quiet(check) == 0 && run_quiet(del) != 0)
    net_command_failed("iptables -D");
}

// The app's 「局域网访问容器」: state/lan exists while it is on.
static int lan_open(void) {
  char path[4096];
  path_in(path, sizeof(path), opt.state, "lan");
  return access(path, F_OK) == 0;
}

// Turns forwarding on, again whenever netd turned it off (a hotspot stopping
// does); only on a change: IPv6's switch walks every interface when written.
static void forward_on(const struct family *f) {
  char value[8];
  if (read_line(f->forward, value, sizeof(value)) < 0 || strcmp(value, "1") != 0)
    write_file(f->forward, "1");
}

static void net_base_rules(const struct family *f, int lan) {
  char *ipt = (char *)f->ipt, *sub = (char *)f->subnet, *gw = (char *)f->gw;
  forward_on(f);
  char *m_chk[] = {ipt, "-w", "-t", "nat", "-C", "POSTROUTING", "-s", sub, "-j", "MASQUERADE", NULL};
  char *m_add[] = {ipt, "-w", "-t", "nat", "-A", "POSTROUTING", "-s", sub, "-j", "MASQUERADE", NULL};
  ipt_ensure(m_chk, m_add);
  char *fi_chk[] = {ipt, "-w", "-C", "FORWARD", "-i", NET_BRIDGE, "-j", "ACCEPT", NULL};
  char *fi_add[] = {ipt, "-w", "-I", "FORWARD", "1", "-i", NET_BRIDGE, "-j", "ACCEPT", NULL};
  ipt_ensure(fi_chk, fi_add);
  // Into the container: replies only. With LAN access on, anything routed to
  // the subnet (hotspot clients, a LAN with a route to it) reaches its services.
  char *open_chk[] = {ipt, "-w", "-C", "FORWARD", "-o", NET_BRIDGE, "-j", "ACCEPT", NULL};
  char *open_add[] = {ipt, "-w", "-I", "FORWARD", "1", "-o", NET_BRIDGE, "-j", "ACCEPT", NULL};
  char *open_del[] = {ipt, "-w", "-D", "FORWARD", "-o", NET_BRIDGE, "-j", "ACCEPT", NULL};
  char *est_chk[] = {ipt, "-w", "-C", "FORWARD", "-o", NET_BRIDGE, "-m", "state", "--state", "ESTABLISHED,RELATED",
                     "-j", "ACCEPT", NULL};
  char *est_add[] = {ipt, "-w", "-I", "FORWARD", "1", "-o", NET_BRIDGE, "-m", "state", "--state",
                     "ESTABLISHED,RELATED", "-j", "ACCEPT", NULL};
  char *est_del[] = {ipt, "-w", "-D", "FORWARD", "-o", NET_BRIDGE, "-m", "state", "--state", "ESTABLISHED,RELATED",
                     "-j", "ACCEPT", NULL};
  if (lan) {
    ipt_ensure(open_chk, open_add);
    ipt_remove(est_chk, est_del);
  } else {
    ipt_ensure(est_chk, est_add);
    ipt_remove(open_chk, open_del);
  }
  // The gateway address is the container's way to the app (the phone bridge
  // listens there): nothing else may reach it, whatever the LAN setting.
  char *gw_chk[] = {ipt, "-w", "-C", "INPUT", "-d", gw, "!", "-i", NET_BRIDGE, "-j", "DROP", NULL};
  char *gw_add[] = {ipt, "-w", "-I", "INPUT", "1", "-d", gw, "!", "-i", NET_BRIDGE, "-j", "DROP", NULL};
  ipt_ensure(gw_chk, gw_add);
  char *mss_chk[] = {ipt, "-w", "-t", "mangle", "-C", "FORWARD", "-p", "tcp", "--tcp-flags", "SYN,RST", "SYN", "-j",
                     "TCPMSS", "--clamp-mss-to-pmtu", NULL};
  char *mss_add[] = {ipt, "-w", "-t", "mangle", "-A", "FORWARD", "-p", "tcp", "--tcp-flags", "SYN,RST", "SYN", "-j",
                     "TCPMSS", "--clamp-mss-to-pmtu", NULL};
  ipt_ensure(mss_chk, mss_add);
}

// Android policy routing. Container packets enter from the bridge, so netd's
// fwmark rules never match them: one rule selects the app's table for the
// container's subnet, the other sends the way back through main.
static void net_policy(const struct family *f) {
  char *buf = malloc(65536);
  if (!buf)
    return;
  char *ip = (char *)f->ip, *sub = (char *)f->subnet;
  char *argv[] = {IP_BIN, ip, "rule", NULL};
  if (capture(argv, buf, 65536) != 0) {
    free(buf);
    return;
  }
  char table[64], want[80], to[80], from[80];
  app_table_from(buf, table, sizeof(table));
  int blocked = table[0] == '!';
  snprintf(want, sizeof(want), blocked ? "%s" : "lookup %s", blocked ? table + 1 : table);
  snprintf(to, sizeof(to), "to %s", f->subnet);
  snprintf(from, sizeof(from), "from %s", f->subnet);
  int have_to = 0, have_from = 0;
  char *save = NULL;
  for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
    if (!strncmp(line, NET_RULE_TO ":", strlen(NET_RULE_TO) + 1) && strstr(line, to) && strstr(line, "lookup main"))
      have_to = 1;
    if (table[0] && !strncmp(line, NET_RULE_FROM ":", strlen(NET_RULE_FROM) + 1) && strstr(line, from) &&
        strstr(line, want))
      have_from = 1;
  }
  free(buf);
  if (!have_to) {
    char *del[] = {IP_BIN, ip, "rule", "del", "pref", NET_RULE_TO, NULL};
    run_quiet(del);
    char *add[] = {IP_BIN, ip, "rule", "add", "to", sub, "lookup", "main", "pref", NET_RULE_TO, NULL};
    if (run_quiet(add) != 0)
      net_command_failed("ip rule add (host to container)");
  }
  if (!table[0])
    return;
  if (!have_from) {
    char *del[] = {IP_BIN, ip, "rule", "del", "pref", NET_RULE_FROM, NULL};
    run_quiet(del);
    char *lookup[] = {IP_BIN, ip, "rule", "add", "from", sub, "lookup", table, "pref", NET_RULE_FROM, NULL};
    char *block[] = {IP_BIN, ip, "rule", "add", "from", sub, table + 1, "pref", NET_RULE_FROM, NULL};
    if (run_quiet(blocked ? block : lookup) != 0)
      net_command_failed("ip rule add (container WAN)");
    else if (blocked)
      say("[net] %sthe app's own traffic is blocked (%s): so is the container's", f->label, table + 1);
    else
      say("[net] %scontainer traffic follows the app's table %s", f->label, table);
  }
}

// The app keeps its loopback origin: it talks to 127.0.0.1:PORT. dsh web
// refuses wildcard bind addresses, so the engine keeps binding 127.0.0.1:PORT
// inside the container netns and this relay bridges the two: it listens in the
// host netns and, per connection, enters the container netns and connects to
// the engine's loopback. No host netfilter is involved.
static void relay_all(int fd, const char *buf, size_t len) {
  while (len > 0) {
    ssize_t n = write(fd, buf, len);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return;
    }
    buf += n;
    len -= (size_t)n;
  }
}

static void relay_stream(int a, int b) {
  char buf[65536];
  struct pollfd fds[2] = {{.fd = a, .events = POLLIN}, {.fd = b, .events = POLLIN}};
  int open[2] = {1, 1};
  while (open[0] || open[1]) {
    if (poll(fds, 2, -1) < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    for (int i = 0; i < 2; i++) {
      if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR)))
        continue;
      int from = fds[i].fd, to = fds[1 - i].fd;
      ssize_t n = read(from, buf, sizeof(buf));
      if (n > 0) {
        relay_all(to, buf, (size_t)n);
        continue;
      }
      if (n < 0 && errno == EINTR)
        continue;
      shutdown(to, SHUT_WR);
      fds[i].events = 0;
      fds[i].revents = 0;
      open[i] = 0;
    }
  }
}

static void relay_one(int client, int netns_fd, int port) {
  struct sockaddr_in addr = {
      .sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  if (setns(netns_fd, CLONE_NEWNET) < 0) {
    say("[net] relay: enter the container netns: %s", strerror(errno));
    return;
  }
  int engine = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (engine < 0)
    return;
  if (connect(engine, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(engine);
    return;
  }
  relay_stream(client, engine);
  close(engine);
}

// The relay's socket on the host's 127.0.0.1:PORT, or -1. Bound by `exec`
// itself before the engine starts: a port someone else holds must fail the
// launch, not leave the app talking to whoever has it.
static int relay_listen(int port) {
  int lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (lfd < 0)
    return -1;
  int one = 1;
  setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in addr = {
      .sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(lfd, 16) < 0) {
    int saved = errno;
    close(lfd);
    errno = saved;
    return -1;
  }
  return lfd;
}

// A child of `exec`, in the host netns, until the wrapper dies: then the
// write end of ALIVE closes. No signal would do: the wrapper turns into the
// container's root, which may not signal this host-root process, and the
// kernel checks the parent-death signal the same way.
static void relay_serve(int lfd, int port, int netns_fd, int alive) {
  signal(SIGCHLD, SIG_IGN);
  say("[net] forwarding 127.0.0.1:%d into the container", port);
  struct pollfd pfd[2] = {{.fd = lfd, .events = POLLIN}, {.fd = alive, .events = POLLIN}};
  for (;;) {
    if (poll(pfd, 2, -1) < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (pfd[1].revents)
      break;
    if (!(pfd[0].revents & POLLIN))
      continue;
    int client = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
    if (client < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    pid_t pid = fork();
    if (pid == 0) {
      close(lfd);
      prctl(PR_SET_PDEATHSIG, SIGTERM);
      relay_one(client, netns_fd, port);
      _exit(0);
    }
    close(client);
  }
  close(lfd);
}

// netd's tether rules: a hotspot (or USB/Bluetooth tethering) runs and needs
// forwarding. Unknown counts as running.
static int tethering(const struct family *f) {
  char buf[8192];
  char *argv[] = {(char *)f->ipt, "-w", "-S", "tetherctrl_FORWARD", NULL};
  if (capture(argv, buf, sizeof(buf)) != 0)
    return 1;
  return strstr(buf, " -i ") || strstr(buf, " -o ");
}

// The forwarding switch as it was before a container first turned it on, kept
// in the state dir (a monitor that died leaves it for the next start's cleanup).
static void forward_remember(const struct family *f) {
  char path[4096], value = '1';
  path_in(path, sizeof(path), opt.state, f->orig);
  if (access(path, F_OK) == 0)
    return;
  int fd = open(f->forward, O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    if (read(fd, &value, 1) != 1)
      value = '1';
    close(fd);
  }
  int out = open_state(f->orig, O_WRONLY | O_CREAT | O_TRUNC);
  if (out >= 0) {
    write(out, &value, 1);
    close(out);
  }
}

// Puts the switch back off if it was off, unless tethering needs it now.
static void forward_restore(const struct family *f) {
  char path[4096], value = '1';
  path_in(path, sizeof(path), opt.state, f->orig);
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return;
  if (read(fd, &value, 1) != 1)
    value = '1';
  close(fd);
  unlink(path);
  if (value != '0')
    return;
  if (tethering(f)) {
    say("[net] tethering runs: %sforwarding stays on", f->label);
    return;
  }
  write_file(f->forward, "0");
}

static void net_cleanup(void) {
  for (size_t i = 0; i < NET_FAMILY_COUNT; i++) {
    const struct family *f = &NET_FAMILIES[i];
    char *ipt = (char *)f->ipt, *ip = (char *)f->ip, *sub = (char *)f->subnet, *gw = (char *)f->gw;
    char *m_del[] = {ipt, "-w", "-t", "nat", "-D", "POSTROUTING", "-s", sub, "-j", "MASQUERADE", NULL};
    run_quiet(m_del);
    char *fi_del[] = {ipt, "-w", "-D", "FORWARD", "-i", NET_BRIDGE, "-j", "ACCEPT", NULL};
    run_quiet(fi_del);
    char *fo_del[] = {ipt, "-w", "-D", "FORWARD", "-o", NET_BRIDGE, "-j", "ACCEPT", NULL};
    run_quiet(fo_del);
    char *est_del[] = {ipt, "-w", "-D", "FORWARD", "-o", NET_BRIDGE, "-m", "state", "--state", "ESTABLISHED,RELATED",
                       "-j", "ACCEPT", NULL};
    run_quiet(est_del);
    char *gw_del[] = {ipt, "-w", "-D", "INPUT", "-d", gw, "!", "-i", NET_BRIDGE, "-j", "DROP", NULL};
    run_quiet(gw_del);
    char *mss_del[] = {ipt, "-w", "-t", "mangle", "-D", "FORWARD", "-p", "tcp", "--tcp-flags", "SYN,RST", "SYN", "-j",
                       "TCPMSS", "--clamp-mss-to-pmtu", NULL};
    run_quiet(mss_del);
    char *r1[] = {IP_BIN, ip, "rule", "del", "pref", NET_RULE_TO, NULL};
    run_quiet(r1);
    char *r2[] = {IP_BIN, ip, "rule", "del", "pref", NET_RULE_FROM, NULL};
    run_quiet(r2);
  }
  // The bridge's IPv6 route is in main, not in the device's table (see net_setup).
  char *rt6_del[] = {IP_BIN, "-6", "route", "del", NET6_SUBNET, "dev", NET_BRIDGE, "table", "main", NULL};
  run_quiet(rt6_del);
  char *v_del[] = {IP_BIN, "link", "del", NET_VETH, NULL};
  run_quiet(v_del);
  char *b_del[] = {IP_BIN, "link", "del", NET_BRIDGE, NULL};
  run_quiet(b_del);
  for (size_t i = 0; i < NET_FAMILY_COUNT; i++)
    forward_restore(&NET_FAMILIES[i]);
}

static void net_reconcile(void);

// Removes the state of a previous run, then builds a fresh bridge and veth.
// The child is waiting on its `go` pipe, so its netns exists and has no user.
static int net_setup(pid_t init) {
  net_cleanup();
  for (size_t i = 0; i < NET_FAMILY_COUNT; i++)
    forward_remember(&NET_FAMILIES[i]);
  char ns[64];
  snprintf(ns, sizeof(ns), "/proc/%d/ns/net", (int)init);
  char *br_add[] = {IP_BIN, "link", "add", NET_BRIDGE, "type", "bridge", NULL};
  if (run_quiet(br_add) != 0) {
    net_command_failed("ip link add " NET_BRIDGE);
    return -1;
  }
  char gw[64];
  snprintf(gw, sizeof(gw), NET_GW "/%d", NET_PREFIX);
  char *br_ip[] = {IP_BIN, "addr", "add", gw, "dev", NET_BRIDGE, NULL};
  run_quiet(br_ip);
  // Android puts a device's IPv6 prefix routes in a table of its own
  // (accept_ra_rt_table), which the host-to-container rule does not look up:
  // the route goes to main by hand, once the bridge is up (IPv6 refuses a
  // route through a device that is down).
  char gw6[64];
  snprintf(gw6, sizeof(gw6), NET6_GW "/%d", NET6_PREFIX);
  char *br_ip6[] = {IP_BIN, "-6", "addr", "add", gw6, "dev", NET_BRIDGE, "nodad", "noprefixroute", NULL};
  if (run_quiet(br_ip6) != 0)
    net_command_failed("ip -6 addr add " NET6_GW);
  char *br_up[] = {IP_BIN, "link", "set", NET_BRIDGE, "up", NULL};
  run_quiet(br_up);
  char *br_rt6[] = {IP_BIN, "-6", "route", "add", NET6_SUBNET, "dev", NET_BRIDGE, "table", "main", NULL};
  if (run_quiet(br_rt6) != 0)
    net_command_failed("ip -6 route add " NET6_SUBNET);
  char *veth[] = {IP_BIN, "link", "add", NET_VETH, "type", "veth", "peer", "name", NET_PEER, "netns", ns, NULL};
  if (run_quiet(veth) != 0) {
    net_command_failed("ip link add " NET_VETH);
    return -1;
  }
  char *master[] = {IP_BIN, "link", "set", NET_VETH, "master", NET_BRIDGE, NULL};
  run_quiet(master);
  char *vup[] = {IP_BIN, "link", "set", NET_VETH, "up", NULL};
  run_quiet(vup);
  run_quiet(br_up);
  net_reconcile();
  say("[net] " NET_BRIDGE " " NET_GW "/%d " NET6_GW "/%d <-> " NET_VETH ", netns of %d", NET_PREFIX, NET6_PREFIX,
      (int)init);
  return 0;
}

static void net_reconcile(void) {
  if (!opt.net)
    return;
  int lan = lan_open();
  static int shown = -1;
  if (shown != lan) {
    if (shown != -1 || lan)
      say("[net] LAN access to the container %s", lan ? "on" : "off");
    shown = lan;
  }
  for (size_t i = 0; i < NET_FAMILY_COUNT; i++) {
    net_base_rules(&NET_FAMILIES[i], lan);
    net_policy(&NET_FAMILIES[i]);
  }
}

// Runs as PID 1 inside the container after pivot_root: the peer is already in
// this netns, the container's own iproute2 configures it, then systemd starts.
static void child_network(void) {
  const char *ipbin = access("/usr/sbin/ip", X_OK) == 0 ? "/usr/sbin/ip" : "/sbin/ip";
  char *down[] = {(char *)ipbin, "link", "set", NET_PEER, "down", NULL};
  run_quiet(down);
  char *rename[] = {(char *)ipbin, "link", "set", NET_PEER, "name", "eth0", NULL};
  if (run_quiet(rename) != 0) {
    say("[net] child: no %s; the container has no network", NET_PEER);
    return;
  }
  char *up[] = {(char *)ipbin, "link", "set", "eth0", "up", NULL};
  run_quiet(up);
  char *addr[] = {(char *)ipbin, "addr", "add", NET_IP "/16", "dev", "eth0", NULL};
  if (run_quiet(addr) != 0)
    say("[net] child: cannot address eth0");
  char *route[] = {(char *)ipbin, "route", "add", "default", "via", NET_GW, NULL};
  if (run_quiet(route) != 0)
    say("[net] child: cannot add the default route");
  char *addr6[] = {(char *)ipbin, "-6", "addr", "add", NET6_IP "/64", "dev", "eth0", "nodad", NULL};
  char *route6[] = {(char *)ipbin, "-6", "route", "add", "default", "via", NET6_GW, "dev", "eth0", NULL};
  if (run_quiet(addr6) != 0 || run_quiet(route6) != 0)
    say("[net] child: no IPv6 on eth0");
  char *lo[] = {(char *)ipbin, "link", "set", "lo", "up", NULL};
  run_quiet(lo);
  say("[net] child: eth0 " NET_IP "/16 via " NET_GW ", " NET6_IP "/64 via " NET6_GW);
}


// What uninstalling the app (or clearing its storage) deletes, from several
// corners of its data: once one is gone, the files under the container are
// going away and its uid may soon be another app's. Only those there at the
// start count.
#define SENTINELS 4
static char sentinel[SENTINELS][4096];

static void sentinels_init(void) {
  path_in(sentinel[0], sizeof(sentinel[0]), opt.state, "lock");
  path_in(sentinel[1], sizeof(sentinel[1]), opt.rootfs, "usr/lib/systemd/systemd");
  snprintf(sentinel[2], sizeof(sentinel[2]), "%s", opt.storage ? opt.storage : "");
  // files/system/state -> the app's data directory.
  const char *tail = "/files/system/state";
  size_t n = strlen(opt.state), t = strlen(tail);
  sentinel[3][0] = 0;
  if (n > t && !strcmp(opt.state + n - t, tail))
    snprintf(sentinel[3], sizeof(sentinel[3]), "%.*s/shared_prefs", (int)(n - t), opt.state);
  for (int i = 0; i < SENTINELS; i++)
    if (sentinel[i][0] && access(sentinel[i], F_OK) != 0)
      sentinel[i][0] = 0;
}

static const char *sentinel_gone(void) {
  for (int i = 0; i < SENTINELS; i++)
    if (sentinel[i][0] && access(sentinel[i], F_OK) != 0 && errno == ENOENT)
      return sentinel[i];
  return NULL;
}

// Drain the console into console.log, starting over past CONSOLE_CAP, until
// the container's init exits and the console is empty; kill the container if
// the app's data is being removed. Then clean up after it, holding the lock
// until this process exits: the next start or run waits for the teardown.
static void monitor(pid_t init, int master) {
  int log = open_state("console.log", O_WRONLY | O_CREAT | O_TRUNC);
  off_t written = 0;
  char buf[4096];
  int exited = 0, killed = 0;
  time_t next_check = 0;
  sentinels_init();
  for (;;) {
    int status;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!exited && !killed && now.tv_sec >= next_check) {
      next_check = now.tv_sec + 2;
      net_reconcile();
      const char *gone = sentinel_gone();
      if (gone) {
        killed = 1;
        say("%s is gone: the app's data is being removed; killing the container", gone);
        char dir[4096], path[4096];
        cg_dir(dir, sizeof(dir), NULL);
        path_in(path, sizeof(path), dir, "cgroup.kill");
        write_file(path, "1");
      }
    }
    siginfo_t info = {0};
    if (!exited && waitid(P_PID, init, &info, WEXITED | WNOHANG | WNOWAIT) == 0 && info.si_pid == init) {
      // Lock before reaping: a zombie init still counts as running, so no
      // start or run gets in before the teardown below is done.
      int lock = open_state("lock", O_RDWR | O_CREAT);
      if (lock < 0 || flock(lock, LOCK_EX) < 0)
        say("cannot lock %s: %s", opt.state, strerror(errno));
      waitpid(init, &status, 0);
      if (WIFEXITED(status))
        say("container init exited with %d", WEXITSTATUS(status));
      else
        say("container init killed by signal %d", WTERMSIG(status));
      exited = 1;
    }
    // With no console, or none open in the guest (the master then reports a
    // hang-up at once), just wait for init.
    struct pollfd pfd = {.fd = master, .events = POLLIN};
    ssize_t n = 0;
    if (master >= 0 && poll(&pfd, 1, exited ? 0 : 500) > 0 && (pfd.revents & POLLIN))
      n = read(master, buf, sizeof(buf));
    if (n > 0) {
      if (log >= 0) {
        if (written + n > CONSOLE_CAP) {
          ftruncate(log, 0);
          lseek(log, 0, SEEK_SET);
          written = 0;
        }
        if (write(log, buf, n) == n)
          written += n;
      }
      continue;
    }
    if (exited)
      break;
    if (master < 0 || pfd.revents)
      usleep(500000);
  }
  net_cleanup();
  char dir[4096], path[4096];
  cg_dir(dir, sizeof(dir), NULL);
  cg_remove(dir);
  path_in(path, sizeof(path), opt.state, "init.pid");
  unlink(path);
  path_in(path, sizeof(path), opt.state, "storage.dev");
  unlink(path);
  // Leave our own cgroup for the root one, so that no dsh-go cgroup outlives
  // the container (an app can probe for the name).
  char pid[32];
  snprintf(pid, sizeof(pid), "%d", getpid());
  write_file("/sys/fs/cgroup/cgroup.procs", pid);
  cg_tidy();
  // Unmount the storage (it lives under the rootfs bind) before the lock goes
  // at exit: the next start checks and mounts the same image.
  if (umount2(opt.rootfs, MNT_DETACH) < 0)
    say("unmount %s: %s", opt.rootfs, strerror(errno));
}

static int cmd_start(void) {
  if (!opt.rootfs || !opt.uid || !opt.gid || !opt.base || !opt.count)
    die("start needs --rootfs, --uid, --gid and --range");
  if (opt.storage && opt.storage_size % STORAGE_BLOCK)
    die("--storage-size must be a multiple of %d", STORAGE_BLOCK);
  if (mkdir_p(opt.state, 0755) < 0 || chown(opt.state, opt.uid, opt.gid) < 0)
    die("state dir %s: %s", opt.state, strerror(errno));
  char init[4096];
  snprintf(init, sizeof(init), "%s/usr/lib/systemd/systemd", opt.rootfs);
  if (access(init, X_OK) != 0)
    die("no systemd in %s", opt.rootfs);
  int lock = open_state("lock", O_RDWR | O_CREAT);
  if (lock < 0 || flock(lock, LOCK_EX) < 0)
    die("cannot lock %s: %s", opt.state, strerror(errno));
  pid_t running = running_init();
  if (running) {
    printf("running %d\n", running);
    return 0;
  }
  opt.net = 1;
  ksu_guard();

  int ready[2];
  if (pipe2(ready, O_CLOEXEC) < 0)
    die("pipe: %s", strerror(errno));
  pid_t m = fork();
  if (m < 0)
    die("fork: %s", strerror(errno));
  if (m > 0) {
    close(ready[1]);
    char msg[600] = "";
    ssize_t n = read(ready[0], msg, sizeof(msg) - 1);
    msg[n > 0 ? n : 0] = 0;
    if (strncmp(msg, "ok ", 3) != 0) {
      waitpid(m, NULL, 0);
      // The monitor may have failed after making the container's cgroup.
      char dir[4096];
      cg_dir(dir, sizeof(dir), NULL);
      cg_remove(dir);
      cg_tidy();
      die("container did not start: %s", msg[0] ? msg : "monitor exited");
    }
    printf("running %s\n", msg + 3);
    return 0;
  }

  // The monitor: outlives the caller, never inside the container.
  close(ready[0]);
  ready_fd = ready[1];
  setsid();
  int null = open("/dev/null", O_RDWR | O_CLOEXEC);
  int log = open_state("monitor.log", O_WRONLY | O_CREAT | O_APPEND);
  if (null >= 0)
    dup2(null, 0);
  if (log >= 0) {
    dup2(log, 1);
    dup2(log, 2);
  }
  setvbuf(stderr, NULL, _IOLBF, 0);
  write_file("/proc/self/oom_score_adj", "-900");
  char mdir[4096];
  snprintf(mdir, sizeof(mdir), CG_ROOT "/monitor-%s", opt.name);
  mkdir(CG_ROOT, 0755);
  mkdir(mdir, 0755);
  cg_join(mdir, getpid());
  cg_prepare();
  private_rootfs();
  if (opt.storage)
    storage_attach();
  int console_sock;
  pid_t init_pid = spawn(&console_sock);
  // The app, which cannot look at our processes, reads this file (written after
  // the last boot) as "running"; the monitor removes it when init is gone.
  int state = open_state("init.pid", O_WRONLY | O_CREAT | O_TRUNC);
  if (state >= 0) {
    dprintf(state, "%d %llu\n", init_pid, starttime(init_pid));
    close(state);
  }
  say("container init %d started (systemd)", init_pid);
  // The caller may be gone (its su interrupted): the container keeps its monitor.
  signal(SIGPIPE, SIG_IGN);
  dprintf(ready[1], "ok %d", init_pid);
  close(ready[1]);
  ready_fd = -1;
  // init.pid is written: the next start sees this container and needs no lock.
  close(lock);

  int master = -1;
  char dummy;
  struct iovec iov = {.iov_base = &dummy, .iov_len = 1};
  char control[CMSG_SPACE(sizeof(int))];
  struct msghdr hdr = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
  struct pollfd pfd = {.fd = console_sock, .events = POLLIN};
  if (poll(&pfd, 1, 2000) > 0 && recvmsg(console_sock, &hdr, MSG_CMSG_CLOEXEC) > 0) {
    struct cmsghdr *c = CMSG_FIRSTHDR(&hdr);
    if (c && c->cmsg_type == SCM_RIGHTS)
      memcpy(&master, CMSG_DATA(c), sizeof(int));
  }
  close(console_sock);
  monitor(init_pid, master);
  return 0;
}

// Clones the container's PID 1 into its new namespaces, maps its ids, moves it
// into the container's cgroup and returns once it has built its root and
// exec'd. *console_sock receives the console pty's master.
static pid_t spawn(int *console_sock) {
  if (setgroups(opt.ngroups, opt.groups) < 0)
    die("supplementary groups: %s", strerror(errno));

  int go[2], booted[2], console[2];
  if (pipe2(go, O_CLOEXEC) < 0 || pipe2(booted, O_CLOEXEC) < 0 ||
      socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, console) < 0)
    die("pipes: %s", strerror(errno));
  struct child_pipes cp = {.go = go[0], .go_writer = go[1], .booted = booted[1], .console = console[1]};
  size_t stack_size = 1 << 20;
  char *stack = malloc(stack_size);
  int clone_flags = CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWIPC | SIGCHLD;
  if (opt.net)
    clone_flags |= CLONE_NEWNET;
  pid_t init_pid = clone(container_main, stack + stack_size, clone_flags, &cp);
  if (init_pid < 0)
    die("clone: %s", strerror(errno));
  close(go[0]);
  close(booted[1]);
  close(console[1]);
  char dir[4096];
  cg_dir(dir, sizeof(dir), NULL);
  if (write_maps(init_pid) < 0 || cg_join(dir, init_pid) < 0) {
    int saved = errno;
    kill(init_pid, SIGKILL);
    waitpid(init_pid, NULL, 0);
    cg_remove(dir);
    die("id maps or cgroup: %s", strerror(saved));
  }
  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/oom_score_adj", init_pid);
  write_file(path, "0");
  if (opt.net && net_setup(init_pid) < 0)
    say("[net] host side failed; the container has no network");
  write(go[1], "g", 1);
  close(go[1]);

  // EOF on booted: init exec'd systemd (the write end is close-on-exec).
  char msg[512] = "";
  ssize_t n = read(booted[0], msg, sizeof(msg) - 1);
  close(booted[0]);
  if (n > 0) {
    msg[n] = 0;
    waitpid(init_pid, NULL, 0);
    cg_remove(dir);
    if (opt.net)
      net_cleanup();
    die("%s", msg);
  }
  *console_sock = console[0];
  return init_pid;
}

// `run`'s cgroup, then CG_ROOT and the root cgroup's pids (see cg_release).
static void run_release(void) {
  char dir[4096];
  cg_dir(dir, sizeof(dir), NULL);
  cg_remove(dir);
  cg_release();
}

// A one-off command as the container's PID 1 instead of systemd, on the
// caller's stdio, with the container's root, mounts and storage (backup and
// restore). Only while the container is stopped; exits with the command.
static int cmd_run(void) {
  if (!opt.rootfs || !opt.uid || !opt.gid || !opt.base || !opt.count || !opt.argv || !opt.argv[0])
    die("run needs --rootfs, --uid, --gid, --range and -- ARGV");
  if (mkdir_p(opt.state, 0755) < 0 || chown(opt.state, opt.uid, opt.gid) < 0)
    die("state dir %s: %s", opt.state, strerror(errno));
  int lock = open_state("lock", O_RDWR | O_CREAT);
  if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0 || running_init())
    die("the container is running or busy: stop it first");
  ksu_guard();
  // A run that fails on the way (the storage, the container's root) cleans up as one that ends.
  on_die = run_release;
  cg_prepare();
  private_rootfs();
  if (opt.storage)
    storage_attach();
  int console_sock;
  pid_t pid = spawn(&console_sock);
  close(console_sock);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
    ;
  run_release();
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

// exec ----------------------------------------------------------------------

// The supplementary groups of PID (host numbers), from its status file.
static int init_groups(pid_t pid, gid_t *out, int max) {
  char path[64], line[1024];
  snprintf(path, sizeof(path), "/proc/%d/status", pid);
  FILE *f = fopen(path, "re");
  if (!f)
    return -1;
  int n = -1;
  while (fgets(line, sizeof(line), f)) {
    if (strncmp(line, "Groups:", 7) != 0)
      continue;
    n = 0;
    char *p = line + 7, *end;
    for (unsigned long g; n < max && (g = strtoul(p, &end, 10), end != p); p = end)
      out[n++] = g;
    break;
  }
  fclose(f);
  return n;
}

static volatile sig_atomic_t got_signal;
static void on_signal(int sig) { got_signal = sig; }

// Run ARGV as the container's root, in all its namespaces and below its
// cgroup. Stops it when our stdin closes (the app stopped the engine, or died).
static int cmd_exec(void) {
  if (!opt.argv || !opt.argv[0])
    die("exec needs -- ARGV");
  pid_t init = running_init();
  if (!init)
    die("container is not running");
  char dir[4096];
  cg_dir(dir, sizeof(dir), "dsh-exec");
  mkdir(dir, 0755);
  if (cg_join(dir, getpid()) < 0)
    say("join %s: %s", dir, strerror(errno));
  ksu_guard();

  // The app keeps talking to 127.0.0.1:<port>; the relay listens here in the
  // host netns and joins the container netns per connection.
  if (opt.forward) {
    char nsp[64];
    snprintf(nsp, sizeof(nsp), "/proc/%d/ns/net", (int)init);
    int netns = open(nsp, O_RDONLY | O_CLOEXEC);
    if (netns < 0)
      die("relay: open %s: %s", nsp, strerror(errno));
    int lfd = relay_listen(opt.forward);
    if (lfd < 0)
      die("relay: cannot listen on 127.0.0.1:%d: %s", opt.forward, strerror(errno));
    // Its write end stays open in this process alone (CLOEXEC keeps it from
    // the guest command) and closes when we exit, however we exit.
    int alive[2];
    if (pipe2(alive, O_CLOEXEC) < 0)
      die("relay: pipe: %s", strerror(errno));
    pid_t relay = fork();
    if (relay < 0)
      die("relay: fork: %s", strerror(errno));
    if (relay == 0) {
      close(alive[1]);
      relay_serve(lfd, opt.forward, netns, alive[0]);
      _exit(0);
    }
    close(alive[0]);
    close(lfd);
    close(netns);
  }

  // The groups the container's processes have: its init's, set at start.
  gid_t groups[32];
  int ngroups = init_groups(init, groups, 32);
  if (ngroups < 0 || setgroups(ngroups, groups) < 0)
    die("supplementary groups of %d: %s", init, strerror(errno));

  const char *kinds[] = {"user", "net", "mnt", "pid", "uts", "ipc", "cgroup"};
  int fds[7];
  for (int i = 0; i < 7; i++) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/ns/%s", init, kinds[i]);
    if ((fds[i] = open(path, O_RDONLY | O_CLOEXEC)) < 0)
      die("open %s: %s", path, strerror(errno));
  }
  // User first: the others belong to it, and we hold its capabilities after.
  for (int i = 0; i < 7; i++)
    if (setns(fds[i], 0) < 0)
      die("enter %s namespace: %s", kinds[i], strerror(errno));
  if (setresgid(0, 0, 0) < 0 || setresuid(0, 0, 0) < 0)
    die("become container root: %s", strerror(errno));
  if (chdir(opt.cwd) < 0 && chdir("/") < 0)
    die("chdir: %s", strerror(errno));
  seccomp_guard();

  pid_t child = fork();
  if (child < 0)
    die("fork: %s", strerror(errno));
  if (child == 0) {
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    // Our parent lives outside this PID namespace, so getppid() reads 0 until
    // it dies and we are handed to the container's init.
    if (getppid() != 0)
      _exit(EXIT_FAIL);
    // Stdin is the stop line, ours alone.
    int null = open("/dev/null", O_RDONLY);
    if (null >= 0 && null != 0) {
      dup2(null, 0);
      close(null);
    }
    char *env[] = {"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", "HOME=/root", "TERM=xterm-256color",
                   NULL};
    execve(opt.argv[0], opt.argv, env);
    say("exec %s: %s", opt.argv[0], strerror(errno));
    _exit(127);
  }
  say("exec pid=%d", child);
  signal(SIGTERM, on_signal);
  signal(SIGHUP, on_signal);
  signal(SIGPIPE, SIG_IGN);
  int stopping = 0;
  time_t kill_at = 0;
  for (;;) {
    int status;
    pid_t done = waitpid(child, &status, WNOHANG);
    if (done == child)
      return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    if (!stopping) {
      struct pollfd pfd = {.fd = 0, .events = POLLIN};
      int closed = got_signal != 0;
      if (!closed && poll(&pfd, 1, 200) > 0) {
        char buf[256];
        if ((pfd.revents & (POLLHUP | POLLERR)) || read(0, buf, sizeof(buf)) <= 0)
          closed = 1;
      }
      if (closed) {
        stopping = 1;
        kill(child, SIGTERM);
        kill_at = time(NULL) + 5;
      }
    } else {
      if (time(NULL) >= kill_at)
        kill(child, SIGKILL);
      usleep(100000);
    }
  }
}

// stop / status -------------------------------------------------------------

// Once init is reaped, the lock is the monitor's until it has torn down and
// exited (see monitor).
static void teardown_wait(void) {
  int lock = open_state("lock", O_RDWR | O_CREAT);
  for (int i = 0; lock >= 0 && i < TEARDOWN_WAIT_S * 10 && flock(lock, LOCK_EX | LOCK_NB) < 0; i++)
    usleep(100000);
  if (lock >= 0)
    close(lock);
}

static int cmd_stop(void) {
  pid_t init = running_init();
  if (!init) {
    teardown_wait();
    printf("stopped\n");
    return 0;
  }
  kill(init, SYSTEMD_HALT);
  // Until the monitor has reaped init, then until it has torn down.
  for (int i = 0; i < opt.timeout * 10; i++) {
    if (!running_init()) {
      teardown_wait();
      printf("stopped\n");
      return 0;
    }
    usleep(100000);
  }
  say("container did not halt in %ds; killing it", opt.timeout);
  char dir[4096], path[4096];
  cg_dir(dir, sizeof(dir), NULL);
  path_in(path, sizeof(path), dir, "cgroup.kill");
  write_file(path, "1");
  kill(init, SIGKILL);
  for (int i = 0; i < 50 && running_init(); i++)
    usleep(100000);
  teardown_wait();
  printf("stopped\n");
  return 0;
}

// Grows the storage image (never shrinks it). A running container's
// filesystem grows online; a stopped one's at its next start.
static int cmd_grow(void) {
  if (!opt.storage || !opt.storage_size || opt.storage_size % STORAGE_BLOCK)
    die("grow needs --storage and --storage-size (a multiple of %d)", STORAGE_BLOCK);
  int img = open(opt.storage, O_RDWR | O_CLOEXEC);
  struct stat st;
  if (img < 0 || fstat(img, &st) < 0)
    die("storage %s: %s", opt.storage, strerror(errno));
  if ((unsigned long long)st.st_size >= opt.storage_size) {
    printf("size %lld\n", (long long)st.st_size);
    return 0;
  }
  if (ftruncate(img, opt.storage_size) < 0)
    die("grow %s: %s", opt.storage, strerror(errno));
  close(img);
  pid_t init = running_init();
  if (init) {
    char path[4096], dev[64] = "";
    path_in(path, sizeof(path), opt.state, "storage.dev");
    FILE *f = fopen(path, "re");
    if (!f || !fgets(dev, sizeof(dev), f))
      die("no storage device recorded; it grows at the next start");
    fclose(f);
    dev[strcspn(dev, "\n")] = 0;
    int loop = open(dev, O_RDWR | O_CLOEXEC);
    if (loop < 0 || ioctl(loop, LOOP_SET_CAPACITY, 0) < 0)
      die("loop %s: %s; it grows at the next start", dev, strerror(errno));
    close(loop);
    // The mount is in the container's namespace: reach it through init's root,
    // resolved inside that root only.
    snprintf(path, sizeof(path), "/proc/%d/root", init);
    int root = open(path, O_PATH | O_DIRECTORY | O_CLOEXEC);
    int at = root < 0 ? -1 : open_in(root, STORAGE_DIR);
    char where[64];
    fd_path(where, sizeof(where), at);
    int mnt = at < 0 ? -1 : open(where, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    unsigned long long blocks = opt.storage_size / STORAGE_BLOCK;
    if (mnt < 0 || ioctl(mnt, EXT4_IOC_RESIZE_FS, &blocks) < 0)
      die("resize %s: %s; it grows at the next start", STORAGE_DIR, strerror(errno));
    close(mnt);
    close(at);
    close(root);
  }
  printf("size %llu\n", opt.storage_size);
  return 0;
}

// Whether KernelSU still hands root to the app's uid from here. Run under
// `su --ksu-no-new-privs` and dshns's own guard, as the container is, it must
// not: su then still runs, but as the app. Prints the uid su's shell had
// ("su-uid 0" is an escalation), or "su-uid -" when su did not run at all.
static int cmd_trial(void) {
  if (!opt.uid || !opt.gid)
    die("trial needs --uid and --gid");
  ksu_guard();
  int out[2];
  if (pipe2(out, O_CLOEXEC) < 0)
    die("trial: %s", strerror(errno));
  pid_t pid = fork();
  if (pid == 0) {
    int null = open("/dev/null", O_RDWR);
    dup2(null, 0);
    dup2(out[1], 1);
    dup2(null, 2);
    if (setgroups(0, NULL) < 0 || setresgid(opt.gid, opt.gid, opt.gid) < 0 || setresuid(opt.uid, opt.uid, opt.uid) < 0)
      _exit(EXIT_FAIL);
    execl("/system/bin/su", "su", "-c", "id -u", (char *)NULL);
    _exit(127);
  }
  close(out[1]);
  if (pid < 0)
    die("trial: %s", strerror(errno));
  char buf[64] = "";
  ssize_t n = read(out[0], buf, sizeof(buf) - 1);
  buf[n > 0 ? n : 0] = 0;
  buf[strcspn(buf, "\n")] = 0;
  while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
    ;
  printf("su-uid %s\n", buf[0] ? buf : "-");
  return 0;
}

static int cmd_status(void) {
  pid_t init = running_init();
  if (init) {
    printf("running %d\n", init);
    return 0;
  }
  printf("stopped\n");
  return 3;
}

// main ----------------------------------------------------------------------

static unsigned long number(const char *s, const char *what) {
  char *end;
  errno = 0;
  unsigned long v = strtoul(s, &end, 10);
  if (errno || *end || end == s)
    die("bad %s: %s", what, s);
  return v;
}

int main(int argc, char **argv) {
  if (argc < 2)
    die("usage: dshns start|exec|stop|status|grow --name N --state DIR ... | trial --uid U --gid G");
  opt.cmd = argv[1];
  for (int i = 2; i < argc; i++) {
    const char *a = argv[i];
    const char *v = i + 1 < argc ? argv[i + 1] : NULL;
    if (!strcmp(a, "--")) {
      opt.argv = argv + i + 1;
      break;
    }
    if (!strcmp(a, "--net")) {
      opt.net = 1;
      continue;
    }
    if (!v)
      die("%s needs a value", a);
    i++;
    if (!strcmp(a, "--name"))
      opt.name = v;
    else if (!strcmp(a, "--state"))
      opt.state = v;
    else if (!strcmp(a, "--rootfs"))
      opt.rootfs = v;
    else if (!strcmp(a, "--cwd"))
      opt.cwd = v;
    else if (!strcmp(a, "--uid"))
      opt.uid = number(v, "uid");
    else if (!strcmp(a, "--gid"))
      opt.gid = number(v, "gid");
    else if (!strcmp(a, "--groups")) {
      char *copy = strdup(v), *save = NULL;
      for (char *g = strtok_r(copy, ",", &save); g; g = strtok_r(NULL, ",", &save)) {
        if (opt.ngroups == (int)(sizeof(opt.groups) / sizeof(opt.groups[0])))
          die("too many groups");
        opt.groups[opt.ngroups++] = number(g, "group");
      }
    } else if (!strcmp(a, "--storage"))
      opt.storage = v;
    else if (!strcmp(a, "--storage-size"))
      opt.storage_size = number(v, "storage size");
    else if (!strcmp(a, "--timeout"))
      opt.timeout = number(v, "timeout");
    else if (!strcmp(a, "--forward")) {
      unsigned long f = number(v, "forward port");
      if (!f || f > 65535)
        die("bad forward port: %s", v);
      opt.forward = (int)f;
    }
    else if (!strcmp(a, "--range")) {
      char buf[64];
      snprintf(buf, sizeof(buf), "%s", v);
      char *colon = strchr(buf, ':');
      if (!colon)
        die("--range is BASE:COUNT");
      *colon = 0;
      opt.base = number(buf, "range base");
      opt.count = number(colon + 1, "range count");
    } else if (!strcmp(a, "--bind")) {
      if (opt.nbinds == (int)(sizeof(opt.binds) / sizeof(opt.binds[0])))
        die("too many binds");
      char *copy = strdup(v);
      char *guest = strchr(copy, ':');
      if (!guest || copy[0] != '/' || guest[1] != '/')
        die("--bind is /HOST:/GUEST[:ro]");
      *guest++ = 0;
      char *ro = strchr(guest, ':');
      if (ro)
        *ro++ = 0;
      opt.binds[opt.nbinds++] = (struct bind){copy, guest, ro && !strcmp(ro, "ro")};
    } else
      die("unknown option %s", a);
  }
  if (!strcmp(opt.cmd, "trial"))
    return cmd_trial();
  if (!opt.name || !opt.state)
    die("--name and --state are required");
  for (const char *c = opt.name; *c; c++)
    if (!(*c == '-' || *c == '_' || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9')))
      die("bad name %s", opt.name);

  if (!strcmp(opt.cmd, "start"))
    return cmd_start();
  if (!strcmp(opt.cmd, "exec"))
    return cmd_exec();
  if (!strcmp(opt.cmd, "stop"))
    return cmd_stop();
  if (!strcmp(opt.cmd, "status"))
    return cmd_status();
  if (!strcmp(opt.cmd, "grow"))
    return cmd_grow();
  if (!strcmp(opt.cmd, "run"))
    return cmd_run();
  die("unknown command %s", opt.cmd);
}
