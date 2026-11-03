# 交接：dsh-Go 本地引擎换容器（自用改造）

> 2026-11-03 交接，同日第二、第三个会话更新（系统容器第一期已实现并跑通；第三个会话删掉了命名空间启动，加了 Docker/Podman 存储、备份恢复、卸载保护，见 §5b、§6b）。所有改动**未提交**（工作区里），本仓库不接受外部 PR，这是用户自用的改造。
> 新会话先读本文，再读 `CLAUDE.md`。

## 1. 用户已经拍板的决定

- 自用分叉，不提 PR、不 commit/push，除非用户明确要求。
- **命名空间启动已删除**（第三个会话，用户决定）：那是 proot rootfs + `ns-launch.sh`、node 做 PID 1 的半成品，只用于测试。界面里不再有「命名空间」这种启动方式，只剩 系统容器 / 直接启动 / 兼容启动；检查页里「挂载命名空间」之类的技术项保留。
- **存储与备份（第三个会话拍板）**：Docker/Podman 存储是 32 GB 稀疏 ext4 镜像，放 App 私有目录（`files/system/storage.img`，不放 `/data/adb`），带日志，引擎页可「扩容 16 GB」；「删除系统容器」连存储一起删；「能备份的都备份」（容器系统 + 存储 + `/root`），只走 App 里的按钮；被别的 App 检测时，让用户先「退出 DSH Go（含容器）」，不用 `ksud kernel nuke-ext4-sysfs`（见 §4）；常见问题里写系统容器一节。
- **系统容器第一期：已实现，真机跑通**（见 §5、§6）：systemd 做 PID 1，和宿主共用网络。需求包括数据库、Docker/Podman（"开发环境的一部分"），不需要图形界面。Docker/Podman 的存储已实现（§5b）；Podman 能跑，Docker 跑不起容器（§7）。
  - **第二期**（以后再做）：独立网络命名空间 + NAT，Docker 的默认网桥要靠它。
  - 用新的预制 Ubuntu 镜像（LXC noble），不在现有 rootfs 上缝补。能从 Droidspaces（`../Droidspaces-OSS`，GPL-3.0，和本项目 AGPL-3.0 兼容）抄就抄，但不依赖它运行。
  - 基础镜像**打进 APK**，但要能**单独更新**；现有 proot 那套保留。
  - **proot 和系统容器共用 `/root`**。
  - 容器以**守护进程运行**，App 被杀、更新都不影响它；同时要一个明显的「退出 DSH Go（含容器）」按钮（放在引擎页和常驻通知），先确认 → 停引擎 → 让 systemd 正常关机（超时约 60 s 强杀）→ 关 App。
  - **不补内核**（缺 CGROUP_PIDS、NF_TABLES、BRIDGE_NETFILTER、AUTOFS、CHECKPOINT_RESTORE）。不需要资源限制，不用 k8s。运行要求检查里这几项算"警告"，不算"不通过"。
- 安全模型：App 已在 SukiSU 授权 root；容器里的进程必须带 KernelSU 的 no-new-privs 标志（`su --ksu-no-new-privs`，或启动器调 ioctl `_IO('K',21)`）。真机已验证：带标志后，容器内执行 su 和 GRANT_ROOT 都会被拒。第三个会话补测：`su --ksu-no-new-privs` 本身就有效；被拒时 su **仍会执行、退出码 0**，只是以 App 身份运行、能力位全 0，所以不能拿退出码判断（`dshns trial` 看 `id -u`）。dshns 的 start/exec/run/trial 自己也调 ioctl 21。

## 2. 环境与操作要点

- 设备：一加平板 OPD2513，Android 16，内核 6.12 GKI，SukiSU Ultra **v4.3.0（版本号 41100）**，SELinux Enforcing，root 域 `u:r:ksu:s0`。
  - App 包名 `io.github.ccch1mneyyy.dsh_app`，uid **10346**，附加组 `3003 9997 20346 50346`，SELinux 上下文 `untrusted_app_27:s0:c90,c257,c512,c768`。
- USB 是用户手动挂给 WSL 的，开始前先 `~/android-sdk/platform-tools/adb devices` 确认设备在。
- **在设备上用 root 执行命令要写成 `adb shell su <<'EOF' ... EOF`**。`adb shell su -c 'a; b'` 只有 a 以 root 运行。
- 工具链：`source ~/dev/env.sh`，内含 JDK17、Flutter 3.47.7、`ANDROID_HOME=~/android-sdk`、`GRADLE_USER_HOME=~/dev/gradle-home`。用户的 `~/.gradle` 代理失效且缓存损坏，**不要用它**。
- 构建：`tooling/build-apk.sh -f arm64`，增量约 3 分钟，结束后 `git checkout app/pubspec.lock`（构建会把镜像地址改成国内源）。APK 约 347 MB（含 134 MB 系统镜像）。
- 安装：`adb install` 在 ColorOS 上会卡住。改用 `adb push apk /data/local/tmp/dsh-go.apk`，再在 su 里执行 `pm install -r -t` 并删除临时文件。
- `adb push` 偶尔会**整个卡死**（正常 14 MB/s，卡住时十分钟只传几 MB）。按 PID `kill` 掉那个 adb 进程重推即可；不要用 `pkill -f "adb push"`，它会把发起它的那条 shell 命令一起杀掉。
- 界面自动化：`uiautomator dump` 能拿到 Flutter 的语义节点（文字和坐标），比截图可靠；本会话的脚本在会话 scratchpad 里，没进仓库，需要时照着重写（dump → 按文字找 bounds → `input tap` 中心点）。
- 测试：
  - Kotlin：`cd app/android && ./gradlew :app:testArm64DebugUnitTest`
  - Dart：`DSH_TEST_FONT_DIR=<放 NotoSansCJK-{Regular,Bold,Light}.ttc 的目录> env -u http_proxy -u https_proxy -u HTTP_PROXY -u HTTPS_PROXY flutter test ...`。本机只有 Regular 和 Bold，Light 用 Regular 的软链顶替。
  - Dart 测试要在 Bash 沙箱外跑，否则 flutter_tester 连不上测试进程（WebSocket upgrade 报错）。全量跑有 43 个截图比对和依赖外网的测试在本机失败，与改动无关。
  - 静态检查：`flutter analyze`
  - 文案：改 `app/lib/l10n/parts/phone_{zh,en}.arb`，然后跑 `python3 app/tool/l10n_merge.py`
- App 内跳转：`adb shell am start -a android.intent.action.VIEW -d dshgo://open/diagnostics`（另有 `engine`），开发者选项在诊断页最下面。平板是 3392×2400 横屏，有时会转成竖屏；截图后再点。
- runtime 构建：`runtime/build-runtime.sh arm64-v8a`。proot 5.1.107.98 已从所有 Termux 镜像下架，`build-runtime.sh` 里改钉成 **5.1.107.102**（本地改动）。
- 只构建系统容器（镜像 + `libdshns.so`，约 1 分钟）：`runtime/build-runtime.sh --system-only arm64-v8a`。需要 NDK 28.2.13676358（`NDK_VERSION` 可改）。
- 预制镜像已下载并核对过校验和：`runtime/.cache/lxc/ubuntu-noble-arm64-20261018_1335/rootfs.tar.xz`，sha256 存在同目录 `rootfs.sha256`，来源 images.linuxcontainers.org。**上游每天重建并删除旧版本，务必保留这份缓存**。

## 3. 命名空间启动（已删除）

第一个会话做的命名空间启动（`ns-launch.sh`、`NamespaceLaunch.kt`、命名空间开关和检查页）在第三个会话按用户要求整体删除。系统容器还在用的部分挪进了 `SystemContainer`（`Preference`、`guest()`、`supplementaryGroups()`）；`NamespaceCheck` 改成 `SystemCheck`，`namespace_check_sheet.dart` 改成 `system_check_sheet.dart`，`phoneNamespaceCheck*` 文案改名为 `phoneSystemCheck*`。`RuntimeController` 启动时会清掉旧的 `namespaceEngine*` 偏好。

## 4. 实测得到的关键事实（不要再踩）

- App（`untrusted_app_27`）**看不到 ksu 域进程的 /proc，也不能给它们发信号**（被 SELinux 拦截）。所以要停容器，只能关闭 su 进程的 stdin，或再通过 su 去杀。
- 必须用 **`su -W`**，否则 KernelSU 的 fd 包装会吞掉 stdin 的 EOF。
- toybox 的坑：`pkill -P` 不带进程名时什么都匹配不上，要用 `kill $(pgrep -P ..)`；`mount -o bind` 绑定一个文件时会当成镜像去建 loop 设备；`remount` 要用绝对路径；toybox 有 `pivot_root`。
- ksu 域创建的文件标签是 `app_data_file:s0`（没有 MLS 分类），App 仍然可以读、写、删。
- 不属于任何 App 的 uid（如 1100000000），亮屏时联网正常；后台没测。
- cgroup v2（`/sys/fs/cgroup`）**一个控制器都没开**，内存、CPU、cpuset 都在 `/dev/...` 下的 v1 层级。所以容器做不了资源限制。
- 预制镜像内容：有 systemd、dbus、udev、sudo、curl、cron、python3、apt、resolved；**没有 git、iptables**。有 26 个非 root 属主的条目、18 个 setuid/setgid 文件、2 个带 xattr 的条目（ping、journal）。

第二个会话（做系统容器时）实测到的：

- `/data` 的 f2fs 开了 **casefold**，overlayfs 直接拒绝（dmesg：`overlay: case-insensitive capable filesystem ... not supported`）。**Docker/Podman 的 overlay 存储不能放在 `/data` 下面**，要另想办法（见 §7）。
- `/data` 挂载带 `nosuid,nodev`。dshns 的 monitor 先 `unshare(NEWNS)`，再把 rootfs 自绑定一次并去掉 nosuid（只在 monitor 和容器的命名空间里），容器里的 sudo/passwd 才能用。
- 在 user 命名空间里新挂 `proc` **没被** mount_too_revealing 拒绝，`/sys` 的 rbind 也正常。
- 在 `sys/fs/cgroup` 上挂 cgroup2 会 EBUSY：那里是宿主同一个 cgroup2 超级块，内核拒绝把同一个文件系统挂在它自己的挂载点上。先盖一层 tmpfs 再挂。
- 从 monitor 命名空间复制进来的挂载带 MNT_LOCKED，`pivot_root` 会报 EINVAL。要在容器子进程里再 bind 一次 rootfs，新 bind 的根不带锁。
- `/storage` 的权限是 `0710 shell:everybody`，必须带附加组 9997 才进得去。monitor 在 clone 前 `setgroups` 成 App 的附加组，容器里显示为 nogroup；`exec` 从容器 1 号进程的 `/proc/<pid>/status` 复制同一组附加组。
- 根 cgroup 没有 `cgroup.kill` 文件，只有子 cgroup 有，所以检查要在 `dsh-go/` 下面做。toybox `unshare` 支持 `-C`（cgroup 命名空间）。
- 容器里 systemd 关机最后调用 reboot(HALT)，内核按 PID 命名空间的规则用 **SIGINT** 结束 init（monitor 日志里是 "killed by signal 2"），属于正常现象。整个关机约 0.6 秒。
- ksu 域是 permissive：容器进程碰到的 SELinux 拒绝只记日志，不拦截。
- 容器进程已移出 App 的 cgroup v2 子树（现在在 `/dsh-go/dsh/...`），但 **v1 的 memory/cpu/cpuset 还停在启动它的 App 所在组**（`memory:/apps`、`cpu:/top-app`）。
- 防提权实测：分别在 `dshns exec` 的进程、root 身份的 systemd 服务、nobody 身份的 systemd 服务里，伪造 `/data/adb/ksud` 后执行 `/system/bin/su`，都**没有**被 KernelSU 重定向（返回 127）。seccomp 过滤器也从 init 一路继承了下去。

## 5. 系统容器第一期：实现（未提交）

设计和上一版交接一致（launcher `dshns` + 预制镜像 + 绑定 proot rootfs 的 DSH 层），下面只写实际落地的样子。

### 启动器 `runtime/container/dshns.c`

- 子命令 `start | exec | stop | status`，通过 `su -W --ksu-no-new-privs -c` 调用。
- 在设计基础上补的东西（理由见 §4）：
  - monitor 建私有 mount 命名空间，rootfs 去掉 nosuid；
  - cgroup2 下面垫一层 tmpfs；
  - 子进程里再 bind 一次 rootfs；
  - `--groups` 参数，`exec` 复制 init 的附加组；
  - init 退出后把控制台剩下的输出读完，`console.log` 能看到完整的关机过程；
  - `stop` 等到 monitor 清理完 cgroup 和 `init.pid` 才返回；
  - seccomp 的架构常量按编译目标选，x86_64 也能编过。
- 修掉的 bug：
  - 非 start 命令会把 state 目录 chown 给 root；
  - lock 被 monitor 一直持有，导致第二次 start 卡死；
  - exec 子进程用 `getppid()==0` 判断父进程，结果永远误判然后退出；
  - console 挂断后 monitor 空转；
  - 启动出错时子进程会永远卡在读管道上。
- `init.pid` 的格式是 `PID STARTTIME`，App 自己读（不需要 root）。判断容器在不在跑时，用这个文件的修改时间和本次开机时间比较。

### 镜像构建

- `rtbuild.py system-image`：
  - 以 LXC rootfs 为基础，保留原有属主和权限；硬链接转成复制，软链接一律转成相对路径。
  - git 的依赖闭包（9 个包）按 LXC 镜像的 dpkg 状态解析，锁定在 `runtime/locks/ubuntu-noble-arm64-system.json`。
  - overlay 内容：
    - 主机名 dsh-go，`/etc/hosts` 里的 LXC_NAME 换成 dsh-go；
    - `resolv.conf` 链到 `/run/dsh-go/proot-etc/resolv.conf`；
    - 屏蔽 13 个 unit：resolved、networkd 三件、kernel config/debug/tracing、fuse-connections、sysctl、binfmt 三件、motd-news；
    - `/etc/profile.d/dsh-app.sh`、`/etc/pip.conf`；
    - `/var/lib/dsh-go/image.json`。
  - 生成 `ownership.tsv`（37 行），归档里 set-id 位清掉，由它在首次启动时恢复。
- 为了复用，从 `assemble` 里抽出了 `register_deb`、`normalize_tree`、`write_tar` 三个函数。**已验证 proot rootfs 的 tar 和压缩包 sha256 与改动前完全一致。**
- `build-runtime.sh`：
  - 钉 LXC 镜像：`LXC_SERIAL` + PIN，`lxc_image` 优先用缓存，缓存没有才下载；
  - `ubuntu_debs` 是从原来的解析函数推广出来的；
  - 新增 `build_system`，用 NDK 编 `libdshns.so`，组 `system.json`，并写进 `out/manifest.json`；
  - 新增 `--system-only` 入口；
  - 没有 PIN 的 ABI（x86_64）跳过系统容器。
- 产物：`runtime/out/arm64-v8a/assets/runtime/system-arm64-v8a.tar.xz`（134 MB，解压后 679 MB）、`system.json`、`jniLibs/arm64-v8a/libdshns.so`。

### App 侧（Kotlin）

- `SystemContainer.kt`：
  - `Layout`：路径都在 `files/system/{rootfs,rootfs.partial,state}`；
  - `bundled` / `installedVersion`：后者读镜像里的 `image.json`；
  - `running`：读 `init.pid`；
  - `install`：先校验 sha256，再并行解 xz，解完 **chmod 根目录 0755**，最后整体改名就位；
  - 命令构建：`startArgs` / `execCommand`；
  - `start`：比较「签名」，即 runtime 版本加启动参数，不含启动器路径。签名不同（runtime 升级、存储授权变了）就先停掉再起；
  - `stop`、`remove`（用 su 执行 `rm -rf`）。
- `RootShell.kt`：su 子进程统一从这里起（NamespaceCheck 也改用它）。
- `QuitActivity.kt`：原生的「退出 DSH Go（含容器）」流程，确认 → `RuntimeController.quit` → 结束所有任务 → kill 进程。
- `NamespaceLaunch.guest()`：从原来的 `command()` 里抽出来解析 proot argv，系统容器复用。
- `NamespaceCheck.runSystem()`：
  - 硬性要求：root、6 个命名空间（含 cgroup）、no-new-privs、cgroup2 + `cgroup.kill`、启动器、镜像；
  - 只算警告（`warn`）：5 个内核选项。
- `DshProcess`：
  - 启动方式优先级：系统容器 > 命名空间 > 直接启动 > proot；
  - 停止和命名空间模式一样：先关 stdin，不行再用 su 杀 `dshns exec` 报出来的宿主 PID。
- `RuntimeController`：
  - 偏好 `systemEngine`，以及失败时记下的 runtime 版本（回退顺序：系统容器 → 命名空间 → 直接/proot）；
  - `prepareSystemContainer`：首次使用时解压镜像，日志按 10% 报进度；
  - `systemContainer(start|stop|delete)`、`quit()`；
  - `reset` 前先停容器；
  - 状态字段 `systemImage`、`systemImageInstalled`、`systemContainerRunning`、`systemContainerBusy` 等。
- `RuntimePlugin`：`setSystemEngine`、`checkSystemContainer`、`systemContainer`、`quit`。
- `DshService`：装了容器以后，通知上多一个「退出」按钮（打开 QuitActivity）。
- `GuestSetup`：AGENTS.md 加了系统容器的环境说明。
- `LaunchDiagnostics` / `CrashClassifier`：新增 `system` 模式，`[dshns] ...` 失败行算作启动失败。
- `AndroidManifest`（QuitActivity）、`values{,-zh}/i18n_strings.xml`（`service_quit`、`quit_*`）。

### App 侧（Dart）

- `runtime_bridge.dart`：状态字段、`NamespaceCheckItem.warn`、新方法。顺手把错位的 `RuntimeBridge` 文档注释放回了原位。
- 诊断页「开发者选项」：
  - 新增「系统容器启动」开关，只在带镜像的构建里出现，必须先过检查，警告不拦；
  - 新增「检查系统容器运行要求」；
  - 检查表单支持 `system` 模式，警告项显示黄色图标。
- 引擎页新增「系统容器」区块（`lib/ui/engine/system_container_section.dart`）：
  - 状态和镜像版本；
  - 启动 / 停止（引擎在里面跑时禁止停止）/ 删除（要确认）；
  - 「退出 DSH Go（含容器）」。
- `diagnostics_page.dart`：「系统容器」启动方式标签和失败原因。
- 文案：`phone_{zh,en}.arb` 里新增 47 个 `phoneSystem*` / `phoneQuitAll*` 键，已生成。

### 测试

- Kotlin 783 个全过。新增：
  - `SystemContainerTest`（参数、exec 命令、PID 解析、签名不受 App 升级影响）；
  - `NamespaceCheckTest.kernelOptions`；
  - `CrashClassifierTest` 的系统容器用例。
- Dart：settings / runtime / engine / diagnostics / safe_mode / l10n 共 451 个全过；新增系统容器开关的用例。
- `flutter analyze` 无问题；`l10n_merge --check`、`guide_sync --check` 通过。
- rtbuild 的 Python 测试（unittest）通过。
- `node --test runtime/tests/*.test.mjs` 有 10 个失败，原因是没设 `DSH_PREFIX`（环境问题），这次没改 patches/bridge，**未处理**。

## 5b. 第三个会话的改动（未提交）

- dshns（`runtime/container/dshns.c`）：
  - `--storage IMG --storage-size BYTES`：monitor 里第一次启动时建稀疏镜像（`/system/bin/mke2fs`，`root_owner` 设成容器 root），每次 `e2fsck -p`，必要时离线 `resize2fs`；`LOOP_CONFIGURE`（AUTOCLEAR、优先 DIRECT_IO）挂到 `/var/lib/dsh-go/storage`（`discard`），再把其中的 `docker`、`containerd`、`containers` 绑到 `/var/lib/` 下。loop 设备记在 `state/storage.dev`。
  - `grow`：只增不减；容器在跑时 `LOOP_SET_CAPACITY` + 经 `/proc/<init>/root` 调 `EXT4_IOC_RESIZE_FS` 在线扩容，否则下次启动时离线扩。
  - `run`：和 start 一样的命名空间与挂载，但前台执行一条命令代替 systemd（备份、恢复用）；容器在跑时拒绝。
  - `trial`：以 App 的 uid 执行 `su -c 'id -u'`，输出 `su-uid N`。
  - monitor 每 2 秒看 4 个哨兵（`state/lock`、rootfs 里的 systemd、存储镜像、App 数据目录的 `shared_prefs`），任一消失就 `cgroup.kill` 整个容器（卸载 App 或清除存储时）。
  - monitor 结束时搬到根 cgroup，删掉 `monitor-<name>` 和空的 `dsh-go`；`run_tool` 的输出改到 stderr（`run` 的 stdout 是备份数据）。
- App：`SystemCheck`（含防提权实测）；`SystemContainer` 的存储参数、`grow`、`backup`/`restore`（`/storage/emulated/0/Download/DSH-Go/backups/dsh-system-*.tar.gz`，在容器视角下 `tar --numeric-owner --xattrs --acls --sparse --one-file-system`，所以 App 的 uid 变了也能恢复；恢复时存储上限取原上限和备份大小的 4 倍中较大的）；`RootShell.stream`；`GuestFiles` 在引擎跑在系统容器里时按容器的 rootfs 找文件（共享路径除外）；`resolv.conf` 改成所有人可读（原来是 0600，容器里非 root 用户——apt 的 `_apt`——解析不了域名）。
- 提示词（`GuestSetup`）：系统容器模式下说明沙箱可用（容器里 Landlock 完全生效），去掉"直接用 danger-full-access"，加上 apt 不共用、不要 reboot、Podman/Docker 现状。
- 镜像：`/etc/containers/containers.conf.d/50-dsh-go.conf`（`pids_limit = 0`，`netns = "host"`）。镜像版本因此变了；已解压的旧镜像不会被替换（用户平板上这个文件是手工补的）。
- 界面：引擎页系统容器区块加了存储用量、扩容、备份、从备份恢复；常见问题加「用系统容器跑引擎」一节（中英，已同步到手册技能）；`dsh-go-support` 技能认得 system 启动方式。

## 6. 真机验证结果（2026-11-03）

- 检查页：全部硬性项通过，5 个内核选项显示为警告。开关只能经检查打开。
- 首次启动引擎（系统容器）：
  - 镜像校验加解压 9–10 秒，容器起来，`dshns exec` 跑 `dsh web`，引擎就绪；
  - 容器内：systemd 状态 `running`，0 个失败单元；引擎和手机操控的 relay 都以容器 root 身份在跑；DNS 和 HTTPS 正常；`/sdcard` 可读写；`ubuntu` 用户的 sudo 可用；systemd-run 可用（含 `--uid=nobody`）。
- `am force-stop`：容器和 monitor 还在，引擎随 App 停止。重新打开 App 时复用同一个容器，准备阶段 7 ms，引擎约 1.2 秒就绪。
- `pm install -r` 升级 App：容器不受影响，升级后启动引擎**不会**重启容器（签名不含启动器路径）。
- 引擎页：停止容器（引擎在里面跑时禁止）、删除（su 删掉其他 uid 的文件）、启动容器（重新解压）都正常。
- 退出流程：
  - 从引擎页点：确认 → 引擎停止 → systemd 正常关机 → App 在 2 秒内退出，没有残留进程和通知；
  - 从通知栏点：同样能拉起确认框，取消后容器继续运行。
- 测试中发现并修掉的两个问题：
  - 镜像根目录是 0700，非 root 用户（messagebus、syslog）无法查找任何路径，导致 dbus 和 rsyslog 启动失败；
  - 签名里带了启动器路径，每次升级 App 后第一次启动引擎都会无故重启容器。
- 当前设备状态（第三个会话结束时）：开着「系统容器启动」，引擎在系统容器里运行；容器里装着 podman 和 docker.io；存储上限 48 GB；`Download/DSH-Go/backups` 里有一份测试备份（383 MB，含 `/root`）。

## 6b. 第三个会话的真机验证（2026-11-03）

- 用独立的 `dshtest` 容器（复制的 rootfs）测 dshns：32 GB 镜像建好后只占 356 KB；写 1 GB 再删，镜像回到 1.6 MB（discard 生效）；在线扩到 48 GB；停容器后 `/proc/fs/jbd2`、`/proc/fs/ext4` 里的 loop 项、loop 设备和 cgroup 都消失；`rm -rf` 整个数据目录后 1 秒内容器结束、loop 释放，内核日志无警告；`run` 备份 223 MB 约 10 秒，换新镜像后恢复，系统、存储、`/root` 里的标记文件和属主（`6:12`）、setuid 都在。
- 模拟普通 App（`untrusted_app` + App uid）能看到的：`/proc/fs/ext4`、`/proc/fs/jbd2` 可见；`/sys/fs/ext4`、loop 的 backing_file、`/sys/block`、`/proc/partitions`、cgroup 列表、我们的挂载、容器进程、`dmesg` 都看不到；`/sys/fs/cgroup/dsh-go` 只能按名字试出存在。
- App 实机：检查页 15 项，防提权实测通过；升级后第一次启动引擎按新参数重启了容器并建好存储；引擎页扩容到 48 GB；停引擎后备份 383 MB 约 15 秒；从备份恢复约 20 秒（删除、解压镜像 10 秒、解包 7 秒），存储按原上限 48 GB 重建；恢复后容器 `running`、0 个失败单元。
- 容器里的 DSH 沙箱：Landlock 启动器 `--probe` 报 "fully enforced"（root 和 nobody 都是）。
- Podman（apt 装在用户的容器里，保留着）：导入、从镜像站拉取、`podman run`（主机网络）、`podman build` 都通；默认的网桥网络失败（netavark setns EPERM），pasta 只支持 rootless；内核没有 pids 控制器，必须 `pids_limit = 0`。
- Docker（docker.io 29，保留着）：守护进程能起、能拉镜像（设备挂代理后 Docker Hub 也通）；`/var/lib/containerd` 不在存储上时 overlay 挂载 EINVAL（已在 dshns 里加绑定，**这次的 APK 才带上**）；放到存储上之后，`docker run --network host` 仍失败：`failed to create default sandbox ... set into network namespace ... operation not permitted`——dockerd 要 setns 进宿主网络命名空间，容器 root 没有权限。

## 7. 未做 / 未验证 / 已知问题

- **Docker 跑不起容器**：需要容器有自己的网络命名空间（第二期），或者只给 dockerd 单独一个网络命名空间（systemd `PrivateNetwork=` + pasta 出网，类似 rootless Docker 的 RootlessKit）。待用户决定。
- 没在真机上触发过「系统容器启动失败 → 自动回退」的路径（单元测试覆盖了分类逻辑）。
- 后台、Doze、低内存查杀下的表现没测过；v1 的 cgroup 还挂在 App 的 `apps`/`top-app` 组下（见 §4）。
- 在系统容器模式下实际执行一次手机操控动作：没测（relay 已在容器里跑起来）。
- 容器里执行 `reboot`：init 被 SIGHUP 结束，monitor 当作退出处理，**不会自动重启**。
- 容器自己停掉（比如在里面 `poweroff`）时，引擎页的状态要等下一次状态事件才会刷新。
- 存储授权变化或 runtime 升级时，下次启动引擎会**自动重启容器**，容器里的服务会被打断，日志里有一行说明。
- `dsh-exec` cgroup 在 systemd 的 unit 体系之外，systemd 看不到引擎这个 unit。
- 系统容器模式下日志里会出现 "starting proot with incomplete process observation"（这句是原有措辞，不准确）。
- ping 的 file capability（xattr）没有保留。
- x86_64 没钉镜像，所以不显示系统容器功能。
- LXC 镜像只剩 `runtime/.cache/lxc/` 里这一份；上游的这个 serial 很快会消失，换 serial 时要同时更新 PIN。
- 第一份交接里未验证的项（命名空间模式的回退、su 强杀兜底、`WRITE_SECURE_SETTINGS`）仍然未验证。

## 8. 建议的下一步

1. 跟用户定 Docker 的网络方案（第二期，或只给 dockerd 单独的网络命名空间 + pasta），再实测 `docker run`/`docker build`。
2. 测后台和 Doze：熄屏放置一段时间，看容器和引擎还在不在、能不能联网。
3. 在系统容器模式下做一次手机操控。
4. 视需要支持容器内 `reboot`（monitor 看到 SIGHUP 时重新启动容器），以及容器状态的定期刷新。
5. 第二期：独立网络命名空间 + NAT（内核有 BRIDGE、VETH、IP_NF_NAT、MASQUERADE，可以用 iptables-legacy）。
