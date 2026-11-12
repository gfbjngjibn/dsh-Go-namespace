# 系统容器改造：历史记录（归档）

> 这是 2026-11-10 拆分前的交接全文，节号（§1–§11）保持原样，原文一字未改；只有 §11.8 末尾的脚本原文挪成了 `runtime/container/tests/` 下的文件。
> **现状以 [../HANDOFF-system-container.md](../HANDOFF-system-container.md) 为准**：本文里的「未提交」「下一轮」「平板当前」都是写下时的状态，§1、§7、§8 之后有变化的地方以那边为准。
> 以后每一轮的实现和验证记录接着往本文末尾加一节，不再往入口文档里堆。

以下是原文（原标题：交接：dsh-Go 本地引擎换容器（自用改造））。

> 2026-11-03 交接，同日第二、第三、第四个会话更新（第三个会话删掉了命名空间启动，加了 Docker/Podman 存储、备份恢复、卸载保护；第四个会话加了独立网络命名空间、Docker 默认网桥接管、宿主 loopback 中继和系统容器里的手机操控，见 §5b、§5c、§6b、§6c）。第五个会话审查并修正了第四个会话的改动，又按 App 里 agent 的自检结果改了提示词、Podman 默认网络、Docker 配置，修了容器重启的竞态（§5d、§5e、§6d、§6e）。前三个会话的改动是 `system-container` 上的 `09e4691`，第四、五个会话的改动是 `6e688aa`，之后修的中继泄漏（§5e）是 `1eaa590`，都已推送到 fork，本仓库不接受外部 PR，这是用户自用的改造。2026-11-05 定下补内核的方案（§9），那一轮没改代码；新内核 2026-11-07 刷进平板后，第六个会话按 §9 改完代码并在新内核上回归（见 §9 "本轮实现与验证"），2026-11-07 已提交并推送到 fork（`66bb44e`）。2026-11-10 用户和 App 里的 agent 又聊了几轮（§10「背景」），定下了下一轮要做的事。同日第七个会话按 §10 实现并在真机上验收（APK 11101624，见 §10「本轮实现与验证」），**还没提交**。
> **新会话先读本文（尤其 §1、§2、§10），再读 `CLAUDE.md`。§10 末尾是这一轮的实现、验收结果和顺带修的问题；§11 是给审查用的细节（逐处改动、设计取舍、没处理的情况、建议重点、复现脚本）；还没定的方向见 §10「还没定的方向」和 §8。**

## 1. 用户已经拍板的决定

- 自用分叉，不提 PR、不 commit/push，除非用户明确要求。
- **命名空间启动已删除**（第三个会话，用户决定）：那是 proot rootfs + `ns-launch.sh`、node 做 PID 1 的半成品，只用于测试。界面里不再有「命名空间」这种启动方式，只剩 系统容器 / 直接启动 / 兼容启动；检查页里「挂载命名空间」之类的技术项保留。
- **存储与备份（第三个会话拍板）**：Docker/Podman 存储是 32 GB 稀疏 ext4 镜像，放 App 私有目录（`files/system/storage.img`，不放 `/data/adb`），带日志，引擎页可「扩容 16 GB」；「删除系统容器」连存储一起删；「能备份的都备份」（容器系统 + 存储 + `/root`），只走 App 里的按钮；被别的 App 检测时，让用户先「退出 DSH Go（含容器）」，不用 `ksud kernel nuke-ext4-sysfs`（见 §4）；常见问题里写系统容器一节。
- **系统容器第一期：已实现，真机跑通**（见 §5、§6）：systemd 做 PID 1。需求包括数据库、Docker/Podman（"开发环境的一部分"），不需要图形界面。Docker/Podman 的存储已实现（§5b）；Podman 能跑；Docker 在 §5c 之后也能跑（默认网桥、外网、`-p` 已实测）。
  - **第二期（本次已实现，见 §5c）**：独立网络命名空间 + NAT；Docker 默认网桥、容器内 Docker、手机操控的 netns 可达性都靠它。容器默认走 App 当前的网络（VPN 开时跟 `tun0`），宿主 loopback 用 TCP 中继保持 App 的 127.0.0.1 来源；Docker 的 `daemon.json` 由 App 接管（引擎页开关，第五个会话改为默认开）。
  - 用新的预制 Ubuntu 镜像（LXC noble），不在现有 rootfs 上缝补。能从 Droidspaces（`../Droidspaces-OSS`，GPL-3.0，和本项目 AGPL-3.0 兼容）抄就抄，但不依赖它运行。
  - 基础镜像**打进 APK**，但要能**单独更新**；现有 proot 那套保留。
  - **proot 和系统容器共用 `/root`**。
  - 容器以**守护进程运行**，App 被杀、更新都不影响它；同时要一个明显的「退出 DSH Go（含容器）」按钮（放在引擎页和常驻通知），先确认 → 停引擎 → 让 systemd 正常关机（超时约 60 s 强杀）→ 关 App。
  - **改为补内核**（2026-11-05 用户决定，取代原来的"不补内核、这几项算警告"）：系统容器只面向自己补过内核的设备，不考虑原厂内核；新补的内核选项按**必需项**检查，缺了不让启动（不学 Droidspaces 做成可选）。用户自己编新内核，下一轮代码等新内核到了再改，见 §9。仍不做内存/CPU 限制（Android 把这些控制器放在 cgroup v1 上，补内核也解决不了），不用 k8s。
- **2026-11-10 定的一轮（详见 §10；第七个会话已实现、真机验收，未提交）**：
  - 容器里的 `/dev/net/tun` 和 `/dev/fuse` 改成由 monitor 自建、容器打得开的节点（用户："tun这个做法没什么问题"；FUSE 要"完整可用"，也照此做）。
  - 用户目录 `/home` 放进现有的 ext4 存储镜像（和 Docker 共用一块，**不另挂一块**），rootless Podman 就能用内核原生的 overlay。
  - 引擎页和常见问题里的「Docker 存储」改名为「容器存储」，相关文案一起改。
  - 用户要求这几件事放到下一轮、在新对话里做，2026-11-10 这一轮只讨论、写交接。第七个会话（同日）做完了。
- 安全模型：App 已在 SukiSU 授权 root；容器里的进程必须带 KernelSU 的 no-new-privs 标志（`su --ksu-no-new-privs`，或启动器调 ioctl `_IO('K',21)`）。真机已验证：带标志后，容器内执行 su 和 GRANT_ROOT 都会被拒。第三个会话补测：`su --ksu-no-new-privs` 本身就有效；被拒时 su **仍会执行、退出码 0**，只是以 App 身份运行、能力位全 0，所以不能拿退出码判断（`dshns trial` 看 `id -u`）。dshns 的 start/exec/run/trial 自己也调 ioctl 21。

## 2. 环境与操作要点

- 设备：一加平板 OPD2513，Android 16，内核 6.12 GKI，SukiSU Ultra **v4.3.0（版本号 41100）**，SELinux Enforcing，root 域 `u:r:ksu:s0`。
  - App 包名 `io.github.ccch1mneyyy.dsh_app`，uid **10346**，附加组 `3003 9997 20346 50346`，SELinux 上下文 `untrusted_app_27:s0:c90,c257,c512,c768`。
- USB 是用户手动挂给 WSL 的，开始前先 `~/android-sdk/platform-tools/adb devices` 确认设备在。
- **在设备上用 root 执行命令要写成 `adb shell su <<'EOF' ... EOF`**。`adb shell su -c 'a; b'` 只有 a 以 root 运行。
- 工具链：`source ~/dev/env.sh`，内含 JDK17、Flutter 3.47.7、`ANDROID_HOME=~/android-sdk`、`GRADLE_USER_HOME=~/dev/gradle-home`。用户的 `~/.gradle` 代理失效且缓存损坏，**不要用它**。
- 构建：`tooling/build-apk.sh -f arm64`，增量约 3 分钟，结束后 `git checkout app/pubspec.lock`（构建会把镜像地址改成国内源）。APK 约 310 MB（含 99 MB 系统镜像；2026-11-07 前是 347 MB / 134 MB）。
- 安装：`adb install` 在 ColorOS 上会卡住。改用 `adb exec-in "cat > /data/local/tmp/dshgo-<构建号>.apk" < apk`（346 MB 约 2 秒），两边 `sha256sum` 对上后在 su 里执行 `pm install -r -t` 并删除临时文件。
- `adb push` 经常**整个卡死**（第五个会话连续两次）；被中断的 push 之后还可能把目标文件删掉（连同后来用别的方式写进去的同名文件），所以换个文件名、用上面的 `exec-in`。不要重启 adb 服务（会丢掉 USB 权限，要用户用 root 重新拉起）；不要用 `pkill -f "adb push"`，它会把发起它的那条 shell 命令一起杀掉。
- 在用户的系统容器里跑命令（容器 root 身份）：把脚本放到 `files/system/rootfs/tmp/` 并 `chown 10346:10346`，然后在 `adb shell su` 里执行下面这段。`dshns exec` 的 stdin 一关就会结束命令（App 就是这样停引擎的），所以要用 fifo 撑住 stdin，脚本跑完再杀掉占位的 sleep：
  ```sh
  S=/data/user/0/io.github.ccch1mneyyy.dsh_app/files/system
  L=$(ls /data/app/*/io.github.ccch1mneyyy.dsh_app*/lib/arm64/libdshns.so | head -1)
  F=/data/local/tmp/cx.fifo; rm -f $F; mkfifo $F; sleep 600 > $F & H=$!
  su 10346 -c "su -W --ksu-no-new-privs -c '$L exec --name dsh --state $S/state -- /bin/bash /tmp/x.sh'" < $F
  kill $H; rm -f $F
  ```
  不要用 `</dev/null`（命令会立刻被结束，退出码 143），也不要在 `su -c` 里套多层引号。
- Git：这是用户自用的分叉，推到用户自己的 fork（公开）：远端 `fork` = https://github.com/gfbjngjibn/dsh-Go-namespace ，分支 `system-container`；`origin` 是上游，**绝不推送**。本仓库的 git 配置（只在本仓库）：`user.name gfbjngjibn`，`user.email 74477599+gfbjngjibn@users.noreply.github.com`（**不要用用户的真实邮箱**），`credential.helper` 是 Windows 的 Git Credential Manager。推送要在 Bash 沙箱外执行。只在用户要求时提交、推送；只暂存明确路径。第三个会话的全部工作是 `system-container` 上的一个提交 `09e4691`，第四、五个会话的是 `6e688aa` 和 `1eaa590`（中继泄漏），2026-11-03 都已推送；交接文档的补记是 `c512310`；第六个会话（新内核那一轮）是 `66bb44e`，2026-11-07 已推送。2026-11-10 的交接更新是 `635f493`（已推送）；同日第七个会话的改动（§10 末尾）**还在工作区，没提交**。
  - 第一次推送时带了用户的真实邮箱（提交 `30425e7`），已改成 noreply 并 force-push，但 GitHub 仍能按 SHA 查到这个旧提交。**用户决定不处理（2026-11-03："那个邮箱的事情不用管了"），不要再提起，也不要建议联系 GitHub Support 或删 fork**；新提交照样只用 noreply 邮箱。
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

## 5. 系统容器第一期：实现（已提交 09e4691）

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
- 产物：`runtime/out/arm64-v8a/assets/runtime/system-arm64-v8a.tar.xz`（2026-11-07 起 99 MB，解压后 475 MB：不再带 apt 索引，见 §9 末尾；之前 134 MB / 679 MB）、`system.json`、`jniLibs/arm64-v8a/libdshns.so`。

### App 侧（Kotlin）

- `SystemContainer.kt`：
  - `Layout`：路径都在 `files/system/{rootfs,rootfs.partial,state}`；
  - `bundled` / `installedVersion`：后者读镜像里的 `image.json`；
  - `running`：读 `init.pid`；
  - `install`：先校验 sha256，再并行解 xz，解完 **chmod 根目录 0755**，最后整体改名就位；
  - 命令构建：`startArgs` / `execCommand`；
  - `start`：比较「签名」，即 runtime 版本加启动参数，不含启动器路径。签名不同（runtime 升级、存储授权变了）就先停掉再起；
  - `stop`、`remove`（用 su 执行 `rm -rf`）。
- `RootShell.kt`：su 子进程统一从这里起（SystemCheck 也改用它）。
- `QuitActivity.kt`：原生的「退出 DSH Go（含容器）」流程，确认 → `RuntimeController.quit` → 结束所有任务 → kill 进程。
- `SystemContainer.guest()`（原 `NamespaceLaunch`）：从原来的 `command()` 里抽出来解析 proot argv，系统容器复用。
- `SystemCheck.runSystem()`：
  - 硬性要求：root、6 个命名空间（含 cgroup）、no-new-privs、cgroup2 + `cgroup.kill`、启动器、镜像；
  - 只算警告（`warn`）：5 个内核选项。
- `DshProcess`：
  - 启动方式优先级：系统容器 > 直接启动 > proot；
  - 停止：先关 stdin，不行再用 su 杀 `dshns exec` 报出来的宿主 PID。
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

- `runtime_bridge.dart`：状态字段、`SystemCheckItem.warn`、新方法。顺手把错位的 `RuntimeBridge` 文档注释放回了原位。
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
  - `SystemCheckTest.kernelOptions`；
  - `CrashClassifierTest` 的系统容器用例。
- Dart：settings / runtime / engine / diagnostics / safe_mode / l10n 共 451 个全过；新增系统容器开关的用例。
- `flutter analyze` 无问题；`l10n_merge --check`、`guide_sync --check` 通过。
- rtbuild 的 Python 测试（unittest）通过。
- `node --test runtime/tests/*.test.mjs` 有 10 个失败，原因是没设 `DSH_PREFIX`（环境问题），这次没改 patches/bridge，**未处理**。

## 5b. 第三个会话的改动（已提交 09e4691）

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

## 5c. 第四个会话：网络命名空间、Docker 接管、手机 bridge（已提交）

### dshns（`runtime/container/dshns.c`）

- `start` 默认 `--net`：`clone(CLONE_NEWNET)` 给容器自己的 netns。monitor 在写 `go` 前 `net_setup()`：
  - 宿主 `dsh-br0` 10.213.0.1/16；veth `dsh-v0`（宿主）/ `dsh-p0`（容器 netns，`IFLA_NET_NS_FD` 创建）；
  - 子进程 pivot 后 `child_network()` 把 `dsh-p0` 改名 `eth0`，配 10.213.0.2/16、默认网关 10.213.0.1、`lo up`；
  - `net_base_rules()`：`POSTROUTING -s 10.213.0.0/16 MASQUERADE`、`FORWARD -i/-o dsh-br0 ACCEPT`、`TCPMSS clamp`；
  - `net_policy()`：`ip rule 6060 to 10.213.0.0/16 lookup main`、`6070 from 10.213.0.0/16 lookup <app table>`；表由 `app_table_from()` 从宿主 `ip rule` 里找 App uid 的 `iif lo` 规则（VPN 开时是 `tun0`），找不到再用 `fwmark 0x0/0xffff iif lo` 的默认表（`ovnet0`）。所以容器的出站默认跟 App 当前网络。
  - monitor 每 2 秒 `net_reconcile()` 重加 base/policy；容器退出或下次启动 `net_cleanup()` 删规则、veth、bridge，`route_localnet=0`。
- `fix_iptables_legacy()`：`start` 第一次在 rootfs 里把 `iptables`/`ip6tables` alternatives 指到 legacy（内核没有 nf_tables），写 `/var/lib/dsh-go/iptables-legacy` 标记；Docker 之后用 legacy 后端。
- `exec --forward PORT`：不再用主机 DNAT。`cmd_exec` 在**宿主 netns** fork 一个 TCP 中继：监听 127.0.0.1:PORT，每个连接 fork 后 `setns` 进容器 netns，再连 127.0.0.1:PORT。引擎继续绑自己的 127.0.0.1（`dsh web` 硬拒绝 `0.0.0.0`），App 仍用 loopback origin；不需要 `route_localnet`/`iptables` 端口规则。中继随引擎 exec 生命周期启动/结束（`PR_SET_PDEATHSIG` + 退出时 kill）。
- `--docker-takeover`：只进签名，不改行为；`daemon.json` 由 App 写。
- `run`/备份恢复/`grow` 不变；`run` 不走容器 netns（它有自己的临时流程，不需要网络）。

### App（Kotlin/Dart）

- `SystemContainer.startArgs()` 追加 `--net`，接管时再加 `--docker-takeover`；签名因此变化一次（老容器会重启）。
- `SystemContainer.takeoverDocker()`：写 `etc/docker/daemon.json`：`{"bridge":"docker0","iptables":true,"ip6tables":false}`；只在开关打开时、每次 `prepareSystemContainer` 启动前写。
- `SystemContainer.execCommand(..., forwardPort)`：`dshns exec ... --forward <port> -- <guest>`；`DshProcess` 把当前端口传进去，引擎 argv 不改（仍是 `--host 127.0.0.1`）。
- `RuntimeController`：偏好 `dockerTakeover`，`setDockerTakeover`（下次启动生效），`snapshot` 里加 `dockerTakeover`，`prepareSystemContainer` 写配置并传参。
- `RuntimePlugin` / `runtime_bridge.dart`：`setDockerTakeover` 方法和状态字段。
- 引擎页系统容器区块：`SettingsSwitchTile`「接管 Docker 配置」（中英 l10n：`phoneSystemDockerTakeover` / `phoneSystemDockerHint`），在已安装镜像时显示；提示写「下次启动引擎时生效」。
- 手机操控：`PhoneBridge` 的 TCP 回退在 127.0.0.1 之外再监听 `10.213.0.1`（系统容器桥网关）；`addTcp()` 在容器晚于手机 bridge 启动时补绑；`PhoneGuest.RELAY_JS` 的目标顺序 abstract → 127.0.0.1 → 10.213.0.1；`PhoneControl` 的 5 秒兜底相应调整。原因：abstract unix socket 是 netns 作用域的，容器里的 relay 看不到 App 的 `@dsh-app.phone...`，容器里的 127.0.0.1 又是它自己的 loopback。
- 文案：`phone_zh.arb` / `phone_en.arb` 加两个键，跑了 `l10n_merge.py`。

### 验证（真机，2026-11-03 下午）

- 容器 netns：`ip -br link` 见 `dsh-br0`/`dsh-v0`；`ip rule` 见 6060/6070（6070 → `tun0`）；monitor 日志 `[net] container traffic follows the app's table tun0`、`[net] dsh-br0 10.213.0.1/16 <-> dsh-v0`、`[net] child: eth0 10.213.0.2/16 via 10.213.0.1`。
- 引擎：容器内 `dsh web --host 127.0.0.1 --port 3081`，宿主 127.0.0.1:3081 有中继；App 的 WebView 正常加载（截图确认对话/工具调用都在），引擎的模型列表能拉到 `api.deepseek.com`。
- 手机操控：App 日志 `phone bridge listening on @... and 127.0.0.1:43637 and 10.213.0.1:43637`；`/proc/net/tcp6` 里 `10.213.0.1:43637` 有一条来自 `10.213.0.2` 的 ESTABLISHED；容器里 `node /opt/dsh-app/phone/relay.cjs` 存活。
- Docker：`/etc/docker/daemon.json` 已是接管内容，`readlink /etc/alternatives/iptables` = `iptables-legacy`；`systemctl is-active docker` = active，`docker info` = server 29.1.3 / overlayfs；`docker run --rm alpine echo DOCKER_OK` = `DOCKER_OK`；`docker run --rm alpine wget -qO- http://example.com` 拿到 HTML（走容器 eth0 → 宿主 NAT → tun0）；`-p 18083:80` 的 busybox nc 服务在容器 netns 的 `curl 10.213.0.2:18083` 和宿主的 `curl 10.213.0.2:18083` 都返回 `DOCKER_HT`。
- 诊断命令的坑：`dshns exec` 把 stdin 当停止线，adb heredoc 直接跑会立刻 EOF 杀掉子进程（退出码 143/无输出）；用 `{ sleep N & } | dshns exec ...` 或 §2 的 fifo。App 的引擎 exec 有自己的管道，不受影响。
- 未做：`docker build` 这次没重跑；手机操控没有真正调一次 `mcp__phone__*`；VPN 开关/熄屏没测。

## 5d. 第五个会话：审查后的修正（已提交）

审查第四个会话的改动后按用户决定修改（审查结论见对话；R1–R6、Q1、Q2 是审查里的编号）：

- **R1 宿主 root 不再跟随容器里的软链接**：`fix_iptables_legacy` 挪进容器 1 号进程（pivot 之后、`apply_ownership` 旁边，路径都是容器自己的）；monitor 建目录、改属主、挂存储改用 `openat2(RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS)`（`open_in`/`dir_in`），`fchownat(AT_EMPTY_PATH)` 改属主，经 `/proc/self/fd/N` 挂载；`grow` 也在 `/proc/<init>/root` 里按同样方式解析。原来 `guest_dir` 跟随软链接 `chown` 的问题（`09e4691` 里就有）一起修了。App 写 `daemon.json` 时要求 `etc/docker` 的真实路径就是 rootfs 里那个目录，且不跟随最后一级软链接。
- **R2**：`exec --forward` 由 `exec` 自己先 bind/listen，失败直接 `die`（引擎启动失败），成功后把监听 fd 交给中继子进程。
- **R3**：覆盖 App uid 的第一条规则若是 prohibit/unreachable/blackhole（例如 VPN 锁定且 VPN 断开），6070 也写成同样的阻止规则，不再退回默认网络。**未实测**（用户决定不测）。
- **R4 「局域网访问容器」开关**（引擎页，默认关）：关时 FORWARD 进容器方向只放行 `-m state --state ESTABLISHED,RELATED`；开时无条件放行（热点客户端可直接访问 10.213.0.2；同 Wi-Fi 的电脑需加经本机到 10.213.0.0/16 的路由）。App 写/删 `state/lan`，monitor 每 2 秒对账时读它，立即生效；偏好 `systemLanAccess` 是真源，每次启动容器前同步一次标记文件。
- **Q2**：手机操控在 `10.213.0.1` 上只接受来源 10.213.0.0/16 的连接（`PhoneBridge.fromContainer`），dshns 另加 `INPUT -d 10.213.0.1 ! -i dsh-br0 -j DROP`。与 R4 开关无关，始终只对容器开放。
- **R5**：删掉写 `route_localnet` 的残留；`ip_forward` 启动前的值存到 `state/ip_forward.orig`，停容器时原值是 0 且没在共享网络（netd 的 `tetherctrl_FORWARD` 里没有 `-i/-o` 规则）才写回 0。
- **R6**：`--docker-takeover` 不再传给 dshns，改成 Kotlin 里签名的一项；关掉接管时，`daemon.json` 若仍是 App 写的原样就删除，改过的保留；提示文案按实际行为改写。
- **Q1**：所有 `iptables` 调用加 `-w`。
- 测试：新增 `PhoneBridgeContainerTest`，`SystemContainerTest` 的接管用例改为验证签名；Kotlin 789 个通过，`flutter analyze`、l10n 检查通过，dshns `-Werror` 编译通过。
- APK 11031714 已构建并装到平板；真机验证见 §6d。

## 5e. 第五个会话（续）：按 agent 自检修正（已提交）

在 App 的自检会话（"Runtime environment self-check with shell commands"，`session-633a3c02`）里让 agent 自检，发现提示词还是第二期之前的说法（"和手机共用网络"、"Docker 跑不起容器"）。按用户决定修改：

- **提示词**（`GuestSetup.kt` 的 `SYSTEM_ENVIRONMENT`/`SYSTEM_EXTRA`）：容器有自己的网络、经 NAT 上网、地址 10.213.0.2，手机上用这个地址访问，局域网默认访问不到；Docker 和 Podman 都能跑容器、默认网桥可用、`-p` 的端口用 `10.213.0.2:端口`；`daemon.json` 由 App 管理（「接管 Docker 配置」），出问题先请用户确认开关，不要手改。`GuestAgentsModeTest` 跟着改。
- **Podman 默认走网桥**：镜像的 `containers.conf.d/50-dsh-go.conf` 去掉 `netns = "host"`，只留 `pids_limit = 0`。
- **Docker 配置加 `"ip-forward-no-drop": true`**：Docker 默认把容器 netns 里 FORWARD 的策略改成 DROP，netavark 1.4 的 `NETAVARK_FORWARD` 只放行回包和出向，宿主/手机连 Podman 发布的端口会被丢掉。签名里带上整份 App 写的 `daemon.json`，以后改配置的 App 更新会重启容器让 Docker 读到。
- **镜像加 apt 钩子**（`/etc/apt/apt.conf.d/50dsh-go-iptables` → `/usr/lib/dsh-go/iptables-legacy`）：每次 apt 跑完 dpkg，若 `iptables`/`ip6tables` 指向 nft 就切到 legacy；切过且装了 Docker 就 `reset-failed` 并重启 docker。原来只有 dshns 在**开机时**切，新镜像里第一次 `apt install docker.io` 的那次启动 Docker 起不来（`failed to create NAT chain`），Podman 的网桥也一样。dshns 的开机切换保留。
- **修了容器重启的竞态**（`dshns.c`）：monitor 在容器启动后就放掉了 `lock`，`running_init()` 在 init 被回收的一刻就返回 0，`stop` 随即返回；这时 monitor 还在清网络、删 cgroup，存储也还挂在它的挂载命名空间里。App 换签名时 stop→start 正好撞上：旧 monitor 删掉了新容器的 cgroup（`id maps or cgroup: No such file or directory`），e2fsck 对仍挂着的存储回放了日志，App 回退到兼容启动。现在：
  - monitor 用 `waitid(WNOWAIT)` 发现 init 退出，**先拿锁再回收**（僵尸 init 仍算 running，新 start 只会回答 running），收尾后 `umount2(rootfs, MNT_DETACH)` 卸掉整棵树（含存储），退出时锁随进程释放；
  - `stop` 在 init 没了之后再等这把锁（最多 `TEARDOWN_WAIT_S` = 10 秒，App 给 stop 留了 timeout+20 秒），所以 stop 回答 stopped 时收尾已经完成；`start` 本来就阻塞等锁，`run`（备份/恢复）用的是非阻塞锁，靠 stop 的等待。
- 测试：`SystemContainerTest` 加一条签名带 `ip-forward-no-drop` 的断言；Kotlin 789 个通过；dshns `-Werror` 编译通过。
- **「接管 Docker 配置」改为默认打开**（用户决定）：`RuntimeController.dockerTakeover()` 的默认值、Dart `RuntimeStatus` 的占位默认值都改成 true。关着时 Docker 用不了默认网桥。
- **面向用户的文案**：FAQ（`docs/guide/{zh,en}/11-faq.md`）删掉"目前能跑容器的是 Podman、Docker 跑不起容器"，改成两者都能跑、默认网桥可用、接管默认打开，另加一条网络说明（10.213.0.2、手机上怎么访问、局域网默认访问不到和「局域网访问容器」开关）；开关说明 `phoneSystemHint`（中英）的"容器和手机共用网络"改成"容器有自己的网络（地址 10.213.0.2，经手机上网）"。跑过 `l10n_merge.py` 和 `guide_sync.py`，`--check` 都通过。
- APK 11032026（镜像 `s1-ubuntu-noble-20261018_1335-36684ad3a02b`）已装到平板；验证见 §6e。默认打开接管和上一条文案是之后改的，**没有重新打包上机**（平板上的接管偏好本来就开着）。
- **中继泄漏**（推送 `6e688aa` 后发现，修复是 `1eaa590`）：`exec --forward` 的中继是在包装进程切进容器 userns 之前 fork 的，以宿主 root 运行；包装进程之后是容器 root（宿主 uid 10346），`kill(relay, SIGTERM)` 被拒（EPERM），内核发父进程死亡信号（`PR_SET_PDEATHSIG`）也按包装进程的身份检查权限，同样发不出去。于是每停一次引擎就留下一个监听宿主 `127.0.0.1:<旧端口>`、转发进容器同一端口的中继：App 的端口一路涨（3080→3081→3082），而且以后容器里谁占了那个端口，手机上任何 App 都能连进去。修法：包装进程留一根管道的写端（CLOEXEC，不给容器命令），中继 `poll` 监听套接字和读端，包装进程无论怎么退出，读端一 HUP 中继就退出；去掉无效的 `kill` 和 PDEATHSIG。
- 检查：Kotlin 789 个通过；`flutter analyze` 无问题；`runtime/tests` 的 Python 测试通过，node 测试 238 个里 10 个失败，都是因为没设 `DSH_PREFIX`/`DSH_TEST_PREFIX`（需要打好补丁的 DSH 安装），与本次改动无关。

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

## 6c. 第四个会话的真机验证（2026-11-03 下午）

- 用新 APK（11031548，含手机 bridge 修复）安装后，App 启动 → 容器从旧签名重启一次 → 新 netns；`init.pid` 新的；`dsh-br0`/`dsh-v0` 在；策略 6070 指向 `tun0`。
- App 的 WebView 正常（截图：对话、工具调用、右侧详情都在）；uiautomator 只能看到 root 授权 toast，读不到 WebView 内容，属正常。
- 手机 relay 连接（见 §5c）；Docker 全部通过（见 §5c）。
- 设备状态：`systemEngine=true`、`dockerTakeover=true`；引擎端口 3081（App 的 `pickPort` 选的）；容器里 Docker 镜像 `alpine:latest` 已拉（约 3 MB），测试容器已删；宿主 `route_localnet=0`；`ip rule` 6060/6070。
- 注意：这次 `dockerTakeover` 是手改 shared_prefs 打开的（`dsh_runtime.xml`），不是从 UI 点的；UI 开关的代码路径（`setDockerTakeover` → pref → 下次 `prepareSystemContainer` 写配置）和状态字段已编译通过，但还没在界面上点一次。

## 6d. 第五个会话的真机验证（2026-11-03 晚，APK 11031714）

- **R1**：容器停着时在 rootfs 里埋软链接（`var/lib/containerd`、`var/lib/dsh-go/storage`、`etc/alternatives` 指向宿主 `/data/local/tmp/r1/...`，并删掉 iptables 标记）：
  - 目标在 rootfs 里不存在时，容器拒绝启动（`cannot create var/lib/dsh-go/storage in the rootfs`），宿主目录没被动；App 随后按启动失败回退到兼容启动并记了失败（`systemEngineFailedRuntime`，测试后手工清掉）；
  - 在 rootfs 里建好同名目录后再启动：宿主目录属主 `0:0 700`、内容不变，宿主 `alt/iptables` 仍指向 nft；切换只发生在 rootfs 里的 `alt/iptables`（→ legacy），标记写在 rootfs 里；monitor 的挂载表里 0 个落在宿主路径，2 个落在 rootfs 内。
- **R2**：引擎在跑（中继占着 3080）时再 `exec --forward 3080`：`[dshns] relay: cannot listen on 127.0.0.1:3080: Address already in use`，退出码 125，命令没执行。
- **R4**：默认只有 `-o dsh-br0 -m state --state RELATED,ESTABLISHED -j ACCEPT`；引擎页点开开关，4 秒内换成无条件 `-o dsh-br0 -j ACCEPT`，monitor 日志 `LAN access to the container on`；关掉又换回来（`... off`）。
- **Q2**：`INPUT -d 10.213.0.1/32 ! -i dsh-br0 -j DROP` 在；从宿主 lo 连 `10.213.0.1:<手机 bridge 端口>` 超时，`127.0.0.1` 照常；容器里的 relay（10.213.0.2）仍连着手机 bridge。
- **R5**：先把 `ip_forward` 设成 0 再启动容器，`state/ip_forward.orig` = 0；停容器后 `ip_forward` 回到 0，iptables/nat/`ip rule` 里没有残留，网桥删了，状态文件清掉。
- 容器 `running`、0 个失败单元；容器内上网 204；`docker run alpine echo` 正常；Podman 在。
- **未在真机验证**：R3（用户决定不测）；R6 关掉接管时删除 `daemon.json`（平板上接管仍开着）；手机 bridge 拒绝非容器网段来源的 App 层检查（只有单元测试，宿主侧由 INPUT 规则挡住）。
- 平板当前：引擎在系统容器里，「局域网访问容器」关，Docker 接管开；`/data/local/tmp` 下的测试文件已清理。

## 6e. 第五个会话（续）的真机验证（2026-11-03 晚，APK 11032026）

- **Podman 网桥**（旧容器里手工测）：`--network bridge` 拿到 10.88.0.x，DNS、出网通；容器里访问发布端口通。宿主连 `10.213.0.2:<发布端口>` 超时，`iptables -L FORWARD` 显示 policy DROP 计了 13 个包——就是 Docker 的 DROP；加 `ip-forward-no-drop` 并把策略改回 ACCEPT 后重启 Docker，策略保持 ACCEPT，宿主访问 Podman（`PODMAN_HT`）和 Docker（`DOCKER_HT`）的发布端口都通，两者出网都通。
- **竞态**：装 11031945（签名变了）后 App 停容器再启动，复现了 `id maps or cgroup` 失败和存储 "recovering journal"，引擎回退到兼容启动。修好后（11032003）从引擎页关掉/打开「接管 Docker 配置」各重启一次，两次都是 e2fsck `clean`、复用同一个 loop、容器和引擎正常。
- **R6 已验证**：关掉接管后重启，`/etc/docker/` 变空；打开后重启，`daemon.json` 按新内容写回。
- **新镜像从头来**：删掉系统容器 → 启动引擎重新解压（镜像 `…36684ad3a02b`，32 GB 新存储，首次启动设好 37 项属主）→ 不做任何手工处理，`apt install docker.io podman`：钩子把 iptables 切到 legacy，Docker 第一次启动失败的那一次被钩子重启，最后 `active`、`systemctl is-system-running` = running、0 个失败单元；`docker run` 正常；`podman run` 不加参数就在 10.88.0.0/16 网桥上并能出网；FORWARD 策略 ACCEPT。
- **手机访问**：平板上的 Chrome 打开 `http://10.213.0.2:18085` 显示 Podman 容器里的页面；DSH Go 的 uid 也能访问。（用 `su <别的 App uid>` 测不出来：那样连外网都不通，与容器无关。）
- **App 里 agent 的再次自检**（同一会话第 4 轮，2 分 34 秒）：逐条核对新 AGENTS.md，15 条里 14 条成立、1 条（局域网默认访问不到）它在容器里无法验证；Docker/Podman 同时跑、发布端口经 10.213.0.2 可达、`daemon.json` 早于 pid 1 写入都给了证据。它列的"还坏着"都是设计如此（没有 `/system`、`/data/adb`，SELinux 被遮住，time ns 共享但改不了时钟，三个目录共用一块存储，没有 IPv6 nat，`docker0` 空闲时 DOWN）。测试容器它自己清掉了。
- 装包时 `xz-utils` 的 `1ubuntu0.3` 在 ports.ubuntu.com 上 404，测试里临时钉了 noble 原版。**当时判断"镜像站索引没刷新"是错的**，真正原因是镜像自带的旧 apt 索引，2026-11-07 查清并修掉（§9 末尾）。
- **中继泄漏**：旧版 dshns 关 stdin 后中继仍在监听（对照测试复现）；新版关 stdin、`kill -9` 包装进程两种方式下中继都退出、端口释放。装 11032109 后清掉旧版留下的 3 个孤儿中继（父进程 1，3080/3081/3082），从引擎页停止→启动：停止后宿主没有中继，再启动仍是 3080，容器没重启。
- 平板当前：APK 11032109（含中继修复）；新镜像的容器在跑，引擎在容器里（端口 3080），装着 docker.io 29.1.3 和 podman 4.9.3（各拉了一个 alpine）；「局域网访问容器」关，Docker 接管开；`/data/local/tmp` 已清理；Chrome 里留着一个 `10.213.0.2:18085` 的标签页。

## 7. 未做 / 未验证 / 已知问题

- **Docker 的接管是开关，默认打开**（第五个会话改的）：关着时 App 不写 `daemon.json`，也不恢复镜像里的旧配置（App 写过且没被改过的文件会删掉），默认网桥是否可用取决于当时文件里的内容——**关着时的 Docker 没测过**。开关只在下次启动引擎（容器重启）时生效。
- ~~iptables 只能用 legacy、容器进程数没有上限、容器没有 IPv6~~：第六个会话随新内核解决（§9 "本轮实现与验证"）。
- ~~**`podman build` 的 RUN 步骤失败**~~（第七个会话随 tun 节点修好，不加参数就能过；提示词和常见问题里的 `--network host` 已删）（2026-11-07 发现，第五个会话去掉 `netns = "host"` 时引入）：容器在用户命名空间里，buildah 把自己当成 rootless，RUN 默认用 slirp4netns，而容器里没有 `/dev/net/tun`（`open("/dev/net/tun"): Permission denied`）；`--network bridge` 被拒（"cannot use networks as rootless"）。`--network host`、`--isolation chroot` 都能成功。`podman run` 不受影响，`docker build` 正常。**用户定的修法（2026-11-07）：不改环境，在提示词（`GuestSetup` 的 `SYSTEM_EXTRA`）和常见问题里写明 `podman build` 要加 `--network host`。** 2026-11-10 查清根因是容器里的 `/dev/net/tun` 谁都打不开（§10），下一轮修好后去掉这句说明。
- **Podman 容器的 1 号进程不理 SIGTERM 时，停容器会超时被强杀**：systemd 停 `libpod-*.scope` 最多等 90 秒（DefaultTimeoutStopSec），超过 dshns 的 60 秒，于是走 `cgroup.kill`（monitor 日志 "killed by signal 9"）。收尾照常、存储下次 `e2fsck` 是 clean，只是慢。2026-11-07 回归时由测试留下的 `podman run -d alpine sleep 300` 触发。
- ~~**容器里的 `/dev/fuse`、`/dev/net/tun` 打不开**~~（第七个会话已修，见 §10 末尾）（2026-11-10 实测）：dshns 把宿主节点原样绑进来，属主在容器里映射不到。FUSE 程序（sshfs、rclone mount、AppImage、squashfuse）、rootless Podman（存储和网络）、`podman build` 的默认网络、容器里的 VPN 客户端都因此用不了。之前说过"AppImage 走 FUSE 已经能用"，是错的。
- **Docker/Podman 的 `--device` 只支持同名映射**（2026-11-10 实测）：`--device /dev/null`、`--device /dev/fuse` 能用；`--device /dev/null:/dev/x` 这种改名映射，Docker 和 Podman（容器里两者都用 runc 1.3.4）都报 `error creating device nodes: mount src=/dev/x`：runc 在用户命名空间里不能 mknod，改为从宿主绑定节点，却拿容器里的路径当宿主路径。和 `CONFIG_CGROUP_DEVICE` 无关（cgroup2 的设备控制走 BPF，用户命名空间里 runc 本来就跳过设备控制），补那个选项也修不好。绕法：同名 `--device`，或 `-v /dev/a:/dev/b`。`--device-cgroup-rule` 正常。
- **`/sys` 还是没重挂**（用户决定这一版不修），Docker 启动时会警告缺 `docker0/bridge/stp_state`、`brif/*/state`；不影响默认网桥。
- **手机操控的 TCP 回退绑定在 `10.213.0.1`**（只对容器网段开放，见 §5d Q2）：abstract unix socket 是 netns 作用域的，容器里的 relay 连不到；App 的 fallback 因此在 loopback 之外再监听桥网关，relay 依次尝试 abstract → 127.0.0.1 → 10.213.0.1。这个监听对容器/宿主可达，仍有 32 字节 token；`/proc/net/tcp` 的 uid 校验对跨 netns 连接拿不到 uid（返回 null 放行），这是设计接受的弱化，和原来 loopback 回退一样。
- **VPN 开关切换没实测**：策略规则在启动时按 App 当前的表（现在是 tun0）写入，monitor 每 2 秒重看；netd 清规则后会在 2 秒内补回。熄屏/切 VPN 的实际表现待测。
- **`dshns exec` 的 stdin 是停止线**：从 adb 里直接 heredoc 跑诊断会立刻 EOF 把命令杀掉（退出码 143/无输出）；要用 §2 的 fifo 撑住 stdin。App 的引擎 exec 有自己的管道，不受影响。
- **引擎仍只绑 127.0.0.1**：`dsh web` 硬拒绝 `0.0.0.0`（"unspecified (wildcard) address"），所以宿主用 TCP 中继（每连接 setns 进容器 netns）而不是 DNAT；`route_localnet` 保持 0。
- **Docker/Podman `-p` 的宿主访问已测**（§6e，含手机上的 Chrome；2026-11-07 又测了 v4 和 v6）。
- **手机操控已实测**（2026-11-07）：新对话里让 agent 调 `mcp__phone__screen`，0.1 秒读回屏幕内容。第一次失败是因为 `pm install -r` 之后 ColorOS 没有重新绑定 App 的无障碍服务（设置里仍是启用，`dumpsys accessibility` 的 Bound services 里没有它），App 回的提示是对的；把 `enabled_accessibility_services` 去掉再写回原值后就绑上了。以后每次重装 APK 后要测手机操控，先看一下绑定状态。
- 没在真机上触发过「系统容器启动失败 → 自动回退」的路径（单元测试覆盖了分类逻辑）。
- 后台、Doze、低内存查杀下的表现没测过；v1 的 cgroup 还挂在 App 的 `apps`/`top-app` 组下（见 §4）。
- 容器里执行 `reboot`：init 被 SIGHUP 结束，monitor 当作退出处理，**不会自动重启**。
- 容器自己停掉（比如在里面 `poweroff`）时，引擎页的状态要等下一次状态事件才会刷新。
- 存储授权变化或 runtime 升级时，下次启动引擎会**自动重启容器**，容器里的服务会被打断，日志里有一行说明。
- `dsh-exec` cgroup 在 systemd 的 unit 体系之外，systemd 看不到引擎这个 unit。
- 系统容器模式下日志里会出现 "starting proot with incomplete process observation"（这句是原有措辞，不准确）。
- ping 的 file capability（xattr）没有保留。
- x86_64 没钉镜像，所以不显示系统容器功能。
- LXC 镜像只剩 `runtime/.cache/lxc/` 里这一份；上游的这个 serial 很快会消失，换 serial 时要同时更新 PIN。
- 第一份交接里未验证的项（命名空间模式的回退、su 强杀兜底、`WRITE_SECURE_SETTINGS`）仍然未验证。
- **App 从宿主侧读不到 `/home` 里的文件**（第七个会话起）：`GuestFiles`（App 打开、预览 guest 路径的文件）按系统容器的 rootfs 找文件，`/home` 现在在 ext4 存储里，只挂在 monitor 和容器的命名空间里，宿主上的 `rootfs/home` 是空的。以前 `/home/ubuntu` 是 0750、属主是容器里的 ubuntu，App 本来也读不进去，所以实际影响很小；`/var/lib/docker` 等一直如此。
- **`system.json` 里的 `launcher.sha256` 和 APK 里的 `libdshns.so` 字节不同**：gradle 打包时又 strip 了一次（`build.gradle.kts` 的 `keepDebugSymbols` 只列了 Termux 的库）。容器签名用的是 `system.json` 的值，它随 dshns 源码变化，用途上没问题；只是不能拿它去核对设备上的文件。
- **引擎页「容器存储」的用量不实时刷新**：要等下一次状态事件（启动、停止等）；往 `/home` 写 200 MB 时 `storage.img` 实际已涨到 280 MB，页面仍显示 80 MB。以前就这样。
- **Podman 默认网络没有 IPv6**：`-p` 发布的端口从宿主经 `[fd10:213::2]` 连不上（拒绝），经 `10.213.0.2` 可以；Docker 的两个都行。11-07 也只测过 Podman 的 v4。
- `podman build` 时会有几行 `can't raise ambient capability CAP_SETPCAP/CAP_SETUID/CAP_SYS_CHROOT` 警告，不影响构建。
- 启动失败后 `state/storage.dev` 会留下（monitor 正常收尾时才删），内容过时但没人用它判断状态。
- **第一次搬 `/home` 很久时会撞上 App 的 60 秒启动超时**（第七个会话分析，没实测超时）：见 §11.6 第 1 条。
- **`/home` 里有复制不了的文件时容器一直启动失败**：见 §11.6 第 2 条。
- **`dshns run`（备份/恢复）失败时可能留下 `dsh-go` 和根组的 `+pids`**：原来就有，没修，见 §11.6 第 5 条。
- App 的 `files/` 下还有一个 `ns-launch.sh`（命名空间启动留下的，第三个会话删功能时没清这个文件），与系统容器无关。

## 8. 建议的下一步

0. ~~**下一轮先做 §10**（tun/FUSE 节点、`/home` 放进容器存储、改名「容器存储」）~~：第七个会话已做完并在真机验收（§10 末尾），用户准备先用另一个模型审一轮（材料在 §11），之后再决定是否提交、推送。
   - 上一轮：§9 的代码改动已做完并在真机回归（2026-11-07，已提交）；`podman build` 按用户定的写进提示词和常见问题；热点不测（平板不能插卡，用户决定不管）（见 §9 "本轮实现与验证"）。
1. ~~在系统容器里真正调一次手机操控~~：2026-11-07 已测通（§7）。
2. 测 VPN 开关、熄屏/Doze、后台：容器和引擎是否跟着 App 的网络表，netd 清理规则后 2 秒内是否补回。
3. ~~重跑 `docker build`/`podman build`、备份 → 恢复~~：2026-11-07 已跑，`docker build` 和备份恢复正常，`podman build` 失败（§7）。
4. 视需要支持容器内 `reboot`（monitor 看到 SIGHUP 时重新启动容器），以及容器状态的定期刷新。
5. 第二期的收尾：把网络命名空间/Docker 接管/手机 bridge 的验证记录和已知问题继续补全。
6. App 里 agent 在自检会话第 5–12 轮提的、核实后可以做的几项（用户还没定）：重挂 `/sys`（现在容器里看得到宿主全部网卡和 wlan0 的 MAC）、时区跟随手机（容器是 UTC）、日志上限（`daemon.json` 的 `log-opts`、journald 的 `SystemMaxUse`）、`/sdcard` 只读档。它提的"宿主写 cgroup 就能限内存""把宿主 `/dev/loop*` 绑进容器""`lazy_itable_init=0`"核实下来是错的或不该做。
7. 2026-11-10 自检会话第 15–17 轮讨论过、用户还没定的方向（核实结论见 §10「还没定的方向」）：让 agent 以非 root 用户工作、给容器整体设内存上限、让容器里的 agent 能开发调试 Android App（adb 和工具链）。

## 9. 内核方案（2026-11-05 定，新内核 2026-11-07 已刷，代码已改并提交）

### 决定

- 系统容器只面向自己补过内核的设备，**不支持原厂内核**。用户的理由：命名空间这一套从来没打算支持原厂内核；平板现在的内核本来就是用户补过的（第一次补时按 Droidspaces 的检查补齐，缺的是当时 Droidspaces 没检查的项）；对后来的人来说，补几项都是改一次、编一次，不增加成本；做成可选要来回换内核测，测试成本太高。
- 新补的选项全部是**必需项**（选项 1），缺了不启动；不做 Droidspaces 那样"有就用、没有就少功能"的可选路径。
- 编内核在本机另一个项目、另一个对话里做，用户自己负责。给那边的提示词已在本对话里给过（选项清单、全部 `=y` 内建、核对最终 `.config`、比对符号 CRC 保证 vendor 模块能加载、刷入后的 6 项验证）。
- **验证过但不用的办法：RLIMIT_NPROC 按用户命名空间封顶**。6.12 的进程数按用户命名空间汇总计数，上限取自创建者的 RLIMIT_NPROC，前提是创建时真实 uid 不是 root。平板实测，上限设为 200：用非 root 真实 uid 创建时，容器 root fork 到 198 个被拦，容器里另一个 uid 一个也 fork 不出（按整个容器合计），容器 root 调不高（EPERM），宿主 root 把 exec 进程的上限调到 100000 也不影响；现在 dshns 用全局 root 创建，所以完全没有限制。用户决定改用内核的 pids 控制器，这个办法不做。

### OTA 与 KMI

- 刷的是 boot 分区。平板的版本串 `6.12.58-android16-6-…-ab15072794-4k` 是 Google 构建号的格式（不像一加 `-o-` 那种厂商自编的内核），原厂内核就是 Google 编的 GKI。
- 厂商基本改不了 KMI：高通/联发科和各家外设的 vendor 模块很多是二进制，KMI 一变整条供应链要重编，厂商没能力也没动机；KMI 代号只有 Google 在安全修复非改不可时才加 1，非常罕见；大版本在设备生命周期内不变。用户的态度："厂商要是有本事把 KMI 更新了那我认"。
- KMI 冻结后符号表**只增不减**：后来的 OTA 里 vendor 模块可能用到新加的符号，用旧源码编的内核会报 `Unknown symbol`。所以 OTA 后稳妥的做法是先看 `uname -r` 里的 `android16-6`，再用同一代号、不旧于 OTA 那版的 GKI 源码重编再刷，而不是把旧镜像原样刷回去。
- 会改 KMI 的是我们自己加的选项：`CGROUP_PIDS`（cgroup 子系统数）、`NF_TABLES`、`BRIDGE_NETFILTER` 都可能动核心结构体。Google 在 android14-6.1 上专门有 "ANDROID: KMI workaround for CONFIG_NETFILTER_FAMILY_BRIDGE" 这样的提交，打开 bridge netfilter 前先看 android16-6.12 有没有同类处理。

### 要补的选项（全部 `=y`）

- P0，dsh-go 直接要用：
  - `CGROUP_PIDS`：防止容器耗尽全机 PID，并让 Docker/Podman 的 pids 限制和 systemd 的 `TasksMax` 生效。Android 没有把 pids 挂到 v1 上，补上后出现在 v2 根上，可以委派。
  - `NF_TABLES` 及 `NF_TABLES_INET/IPV4/IPV6`、`NFT_CT/NAT/MASQ/REDIR/REJECT/COMPAT/FIB_*` 等：Docker 29、Podman 5、firewalld/ufw 默认走 nft，补上后去掉 legacy 切换。代价是 nf_tables 是近几年"用户命名空间提权到宿主 root"最常见的漏洞来源，容器 root 能直接碰到，内核要跟安全补丁。
  - `IP6_NF_NAT`、`IP6_NF_TARGET_MASQUERADE`：容器经手机用 IPv6 上网。
  - `BRIDGE_NETFILTER`：Docker 同一网桥上容器之间的隔离规则生效。它对宿主也生效，要验证用网桥实现的热点。
- P1：`MACVLAN`、`IPVLAN`、`CGROUP_DEVICE`（**最终没有编进内核**，见下面"新内核实际有什么"）、`CHECKPOINT_RESTORE`、`AUTOFS_FS`、`IP_SET`（加常用集合类型和 `NETFILTER_XT_SET`）、`VXLAN`。对非特权容器帮助很小（主要是 Droidspaces 和 k3s 一类用），但按决定一起补、一起定为必需。
- P2（可选，只对 Droidspaces 有用，用户命名空间里挂不了）：SQUASHFS、ISO9660、UDF、NTFS3、BTRFS、XFS。不进我们的检查。
- 补内核也解决不了：内存/CPU/IO 限制（v1 布局），mknod、loop、挂块设备文件系统、swap（用户命名空间不允许）。

### 新内核实际有什么（2026-11-07）

来源：编内核的那个对话（`/mnt/d/oneplus_sm8850` 项目，详情见那里的 `CLAUDE.md` "第二轮"一节）；用户要求写进本文档。

- **状态**：新内核已刷进平板，2026-11-07 真机检查全部通过（vendor 模块全部照常加载，root 正常，Wi-Fi、充电正常）。
- **版本串和旧内核完全相同**（`6.12.58-android16-6-g3edd99dd2b12-ab15072794-4k`），所以检查页不能靠 `uname -r` 区分新旧，只能看 `/proc/config.gz` 里的选项，以及运行时的状态（比如 `cgroup.controllers` 里有没有 `pids`）。
- **P0 全部有**：
  - `CGROUP_PIDS`
  - `NF_TABLES`、`NF_TABLES_INET/IPV4/IPV6`，以及 `NFT_CT`、`NFT_NAT`、`NFT_MASQ`、`NFT_REDIR`、`NFT_REJECT`、`NFT_COMPAT`、`NFT_FIB_INET/IPV4/IPV6`、`NFT_LOG`、`NFT_LIMIT`、`NFT_QUOTA`、`NFT_CONNLIMIT`、`NFT_HASH`、`NFT_NUMGEN`。counter 在 6.12 里已经内建在 nf_tables 核心，没有单独的选项，不要去检查 `NFT_COUNTER`。
  - `IP6_NF_NAT`、`IP6_NF_TARGET_MASQUERADE`、`BRIDGE_NETFILTER`
- **P1 除 `CGROUP_DEVICE` 外全部有**：
  - `MACVLAN`、`IPVLAN`、`CHECKPOINT_RESTORE`、`AUTOFS_FS`、`VXLAN`
  - `IP_SET`，加 `IP_SET_HASH_IP/NET/IPPORT/IPPORTNET`、`IP_SET_BITMAP_PORT`、`IP_SET_LIST_SET`，以及 `NETFILTER_XT_SET`
- **内核里没有、检查页不能要求的**：
  - **`CGROUP_DEVICE`：不要放进 `KERNEL_OPTIONS`，否则 dsh-go 在这个内核上会拒绝启动。** 不加的原因（用户 2026-11-05 决定）：
    - Droidspaces 源码从来不用它，它只出现在 Droidspaces"非 GKI 内核"的配置清单里。
    - cgroup v2 的设备控制走 BPF，内核已经有 `CGROUP_BPF=y`；devices 控制器只对 cgroup v1 有用。
    - 在 GKI 上加它，要再占一个 cgroup 子系统位置，还会让 freezer、net_cls、net_prio 的编号后移，vendor 模块会出问题。
  - **ipvlan 的 L3S 模式**（`CONFIG_IPVLAN_L3S`，以及它会带上的 `NET_L3_MASTER_DEV`）：它会往 `net_device` 中间插字段，所以内核里关掉了。ipvlan 的 L2、L3 模式正常可用（Docker 的 ipvlan 驱动默认就是 l2）；只有 `mode l3s` 用不了。检查页只查 `CONFIG_IPVLAN=y` 就行，不要查 `IPVLAN_L3S`。
  - P2 只有 `SQUASHFS`（含 XZ、ZSTD）、`ISO9660_FS`、`UDF_FS`；`NTFS3`、`BTRFS`、`XFS` 没加。P2 本来就不进我们的检查。
- **几个和 dsh-go 有关的运行时行为**：
  - **br_netfilter 只在宿主命名空间默认关闭**：内核补丁让宿主网络命名空间的 `net.bridge.bridge-nf-call-{arptables,iptables,ip6tables}` 默认是 0，宿主的行为和没有 br_netfilter 时一样，所以上面 `BRIDGE_NETFILTER` 那条"要验证热点"的顾虑在默认状态下不存在。其他网络命名空间默认仍是 1，所以系统容器里的 Docker 不用 dsh-go 做任何事，网桥隔离就会生效。**dsh-go 不要改宿主的这三个值。**
  - 刷完新内核后，`/sys/fs/cgroup/cgroup.controllers` 里有 `pids`，根组的 `cgroup.subtree_control` 仍然是空的。开不开 pids 由 dshns 自己管，见下面第 2 条。
  - 宿主 ip6tables 里可能会有一张空的 `nat` 表：droidspacesd 检测 NAT66 时会把它建出来（只有默认策略，没有规则）。dsh-go 做 IPv6（第 4 条）时不要把"已经有 nat 表"当成"已经配好了"。
  - nf_tables 的安全代价（见上面 P0 那条）依旧存在：这个内核的源码停在 6.12.58。
- **本对话 2026-11-07 在平板上复核（开机约 16 分钟时）**：版本串同上；`/proc/config.gz` 里上面的 P0、P1 抽查都是 `=y`，`CGROUP_DEVICE`、`IPVLAN_L3S` 都是 not set；`cgroup.controllers` = `pids`，根组 `subtree_control` 为空，`dsh-go`、`droidspaces` 两个组都不存在；宿主 `bridge-nf-call-{arp,ip,ip6}tables` 都是 0，`unshare -n` 新建的 netns 里都是 1；宿主 `ip6tables -t nat -S` 只有 4 条默认策略；`lsmod` 685 行，dmesg 里没有符号报错。容器在 13:49 启动过一次，systemd、Docker、Podman 都起来了，13:56 设备重启时随之结束（所以 monitor 日志没有收尾记录）；那次启动时设备没网络，所以日志里没有 "follows the app's table"。不确定 13:49 那次是否已经是新内核，下一轮回归时以 App 实际启动为准。

### 下一轮代码改动（计划；2026-11-07 已按此实现，见本节末尾）

1. **检查页**（`SystemCheck.kt`）：`KERNEL_OPTIONS` 现在是 5 项、只算警告。改成必需项，并按上面 P0、P1 补全（`IP6_NF_NAT`、`IP6_NF_TARGET_MASQUERADE`、nft 的子项、`MACVLAN`、`IPVLAN`、`IP_SET`、`VXLAN` 等；**不含 `CGROUP_DEVICE` 和 `IPVLAN_L3S`**，以"新内核实际有什么"那一节为准）；最好再实测 `/sys/fs/cgroup/cgroup.controllers` 里有 `pids`。改 `SystemCheckTest.kernelOptions`、类注释、检查页和 FAQ 里关于内核要求的文案（中英）。
2. **pids**：dshns 在 `dsh-go` 的 `cgroup.subtree_control` 打开 `+pids`，给容器写 `pids.max`（讨论时的建议：`pid_max` 的一半，这台是 16384，再设个顶，比如 65536；线程也算，不要定太低）；镜像的 `containers.conf.d/50-dsh-go.conf` 去掉 `pids_limit = 0`（镜像版本会变）。
   - **根组的 `+pids` 只在容器运行期间开着**（来源：2026-11-05 编内核的那个对话，`/mnt/d/oneplus_sm8850` 项目，讨论"相关程序不运行时宿主尽量保持原生"时定的；用户同意写进来）。要在 `dsh-go` 里开 pids，先得在 v2 根组 `/sys/fs/cgroup/cgroup.subtree_control` 写 `+pids`，否则 `dsh-go/cgroup.controllers` 里没有 pids。根组一开，Android 的每个顶层 cgroup（`uid_*` 等）都会带上 pids 计数，这是宿主状态的改变。平板上根组的 `subtree_control` 原生是空的（2026-11-05 实测）。原因：用户的目标是不跑容器、或关掉容器支持时宿主回到原生状态。内核只负责提供 pids 控制器，开不开由用户态决定，所以这件事只能 dshns 自己管。
   - 做法：启动时先读根组的 `subtree_control`，原来就有 `pids` 就不动，也不记账；原来没有才写 `+pids`，并记下"是 dshns 加的"。停止时（包括关掉容器支持、上次异常退出后的清理）先删掉 `dsh-go` 自己的组或在其中写 `-pids`，再对根组写 `-pids`，顺序反过来会因为子组还在用而失败。根组写 `-pids` 返回 EBUSY 时，说明别人还在用，按正常情况处理，不报错。
   - 别人也会改根组：Droidspaces 的容器设了 pids 上限时，会往根组写 `+pids`，而且从来不撤（Droidspaces-OSS `src/cgroup.c` 第 477–482 行）。所以只撤 dshns 自己加的那一次，不要一律写 `-pids`。
   - 补充（本对话核对内核和 Droidspaces 源码）：
     - 根组写 `-pids` 时，内核只在某个子组的 `subtree_control` 里也有 `pids` 时才返回 EBUSY。如果某个根下的直接子组只设了 `pids.max`、自己的 `subtree_control` 里没有 pids，它的上限会被**悄悄撤掉**。Droidspaces 不受影响：它在 `droidspaces/` 组的 `subtree_control` 里也写了 `+pids`，而且从不删这个组，所以它跑过之后我们撤会得到 EBUSY。
     - "是 dshns 加的"这个记录要带上本次开机的 `/proc/sys/kernel/random/boot_id`：cgroup 状态重启就没了，记录却留在 App 目录里。不比对 boot_id 的话，重启后别人加的 `+pids` 可能被当成我们加的撤掉。
     - Droidspaces 的 `force_cgroupv1` 容器会把还没人用的控制器挂到 v1 上，pids 也在其中。如果它在本次开机里先跑，v2 的 `cgroup.controllers` 里就没有 pids 了。dshns 启动时要再查一次，没有就明确报错退出，不要静默跳过；检查页过了不代表启动时一定还有。
3. **去掉 iptables-legacy 切换**：dshns 的 `fix_iptables_legacy()` 和标记 `iptables-legacy.done`，`rtbuild.py` 里的 `usr/lib/dsh-go/iptables-legacy` 和 `etc/apt/apt.conf.d/50dsh-go-iptables`。已经装好的容器里 alternatives 还指向 legacy，要决定是否切回 nft（同一 netns 里两种后端混用会出问题）。
4. **IPv6**：dshns 给网桥和容器分 IPv6 地址（ULA）、加 ip6tables 的 MASQUERADE/FORWARD 和 `ip -6 rule` 策略路由（跟 v4 一样跟 App 的表），`net_cleanup` 对应清理；`daemon.json` 的 `ip6tables` 改 true（签名会变，容器重启一次）；提示词和 FAQ 里的网络说明补上 IPv6。工作量最大的一项。
5. 改完在新内核上把启动、exec、备份恢复、Docker/Podman（含 `build`、`-p`、IPv6）、热点各回归一遍。容器 netns 里 `bridge-nf-call-iptables` 默认是 1，所以同一网桥上容器之间的流量现在会经过 FORWARD 链（Docker 的隔离规则、`ip-forward-no-drop`、netavark 的规则都会作用到它），要额外测一下 Docker 和 Podman 各自网桥上容器之间能不能互通。

### 本轮实现与验证（2026-11-07，第六个会话，APK 11071439，已提交）

改动（按上面 1–4 的顺序）：

1. **检查页**：`SystemCheck.KERNEL_OPTIONS` 改成 10 个必需项，每项带它要一起开的选项：`CGROUP_PIDS`；`NF_TABLES`（含 INET/IPV4/IPV6、`NFT_CT/NAT/MASQ/REDIR/REJECT/COMPAT/FIB_*`）；`IP6_NF_NAT`（含 MASQUERADE）；`BRIDGE_NETFILTER`；`IP_SET`（含 6 种集合和 `NETFILTER_XT_SET`）；`MACVLAN`；`IPVLAN`；`VXLAN`；`CHECKPOINT_RESTORE`；`AUTOFS_FS`。只认 `=y`（GKI 设备不会加载我们的模块）。缺了的项列在 `Item.missing` 里，界面显示"未开启：CONFIG_…"；读不到配置算不通过；`CGROUP_PIDS` 还要 `cgroup.controllers` 里真有 `pids`，没有时给 `unavailable`（可能被 cgroup v1 占了）。去掉了整个"警告"机制（Kotlin `Item.warn`、Dart `SystemCheckItem.warn`、黄色图标、`phoneSystemCheckPassedWarn` 和 5 条 `phoneSystemCheckOff*`），新增 `phoneSystemCheckOptionMissing`、`phoneSystemCheckPidsUnavailable`。`phoneSystemHint` 和 FAQ 写明要补过的内核、原厂内核不行。
2. **pids**（`dshns.c` 的 `pids_enable`/`cg_release`/`cg_prepare`）：`cg_prepare` 先查 `cgroup.controllers` 有没有 pids，没有就报错退出（"no pids controller on cgroup2 …"）；根组 `subtree_control` 没有 pids 才写 `+pids`，并在 state 目录写 `pids.root`（内容是本次开机的 boot_id；启动时发现是旧开机的就删掉）；再在 `dsh-go` 里开 `+pids`，容器组 `pids.max` = `pid_max / 2`，最多 65536（这台 16384）。`cg_release` 在删掉 `dsh-go` 之后、且记录的 boot_id 是本次开机时才对根组写 `-pids`，EBUSY 当正常处理，然后删记录；`cg_tidy`（monitor 收尾、start 失败）和 `run`（备份/恢复）都走它。镜像去掉 `containers.conf.d/50-dsh-go.conf`。
3. **去掉 legacy 切换**：删了 dshns 的 `fix_iptables_legacy()`、镜像的 `usr/lib/dsh-go/iptables-legacy` 和 `etc/apt/apt.conf.d/50dsh-go-iptables`；`phoneSystemDockerHint` 的"legacy iptables"改掉。镜像版本 `s1-ubuntu-noble-20261018_1335-a9e6d2a742ed`。
4. **IPv6**：网络部分改成按地址族各做一遍（`struct family`，`NET_FAMILIES` = v4、v6）：MASQUERADE、FORWARD 进出、局域网开关、网关 INPUT DROP、TCPMSS、`ip -4/-6 rule` 6060/6070（v6 同样跟 App 的表）、转发开关的记录与恢复（v6 的是 `state/ip6_forward.orig`，`forward_on` 只在值不对时才写，因为写 v6 的总开关会遍历所有接口）、清理。地址：网桥 `fd10:213::1/64`，容器 `fd10:213::2/64`。宿主上 Android 把接口的 IPv6 前缀路由放进按接口分的表（`accept_ra_rt_table = -1000`，见 `fe80::/64 dev dsh-br0 table 1023`），所以网桥地址加 `noprefixroute`，`fd10:213::/64 dev dsh-br0` 在网桥 up 之后手动加进 main（网桥没 up 时加会失败，第一版就是这样）。容器 netns 里该值是 0，不受影响。`daemon.json` 改为 `ip6tables: true`、`ipv6: true`、`fixed-cidr-v6: fd10:213:d0c::/64`（签名随之变，容器重启一次）。提示词和 FAQ 加了 IPv6 一句。
- 测试：Kotlin 791 个通过（新增 `SystemCheckTest` 3 条，改了 `GuestAgentsModeTest`、`SystemContainerTest`）；`flutter analyze` 无问题；`direct_engine_settings_test` 和 `test/runtime` 35 个通过（"警告也能开启"的用例改成"缺内核选项就开不了"）；`l10n_merge --check`、`guide_sync --check`、rtbuild 的 unittest 通过；dshns arm64 `-Werror` 和 x86 `-fsyntax-only` 通过。

已装好的容器（旧镜像解压的 rootfs）手工处理过：iptables/ip6tables 的 alternatives 切回 `--auto`（nft），删了 `iptables-legacy.done`，删了旧镜像带的 apt 钩子、`/usr/lib/dsh-go/iptables-legacy` 和 Podman 的 `pids_limit = 0`。新装的镜像本来就没有这些。**没有用新镜像从头装一次**（要删容器）。

真机验证（新内核，APK 11071430 / 11071439）：
- 检查页 25 项全部满足（"全部满足"）。
- pids：根组和 `dsh-go` 的 `subtree_control` = `pids`，`dsh-go/dsh/pids.max` = 16384（文件属主宿主 root，容器 root 写它 Permission denied），容器里的 systemd 自己往下开了 pids（`TasksMax` 有值了）；`pids.root` 的 boot_id 和本次开机一致。停容器（备份时）后根组 `subtree_control` 回到空、记录删掉、`dsh-go` 不在。`docker run --pids-limit 8` 里 `pids.max` = 8、超出时 `can't fork`；Podman 容器默认 `pids.max` = 2048。
- 容器：`systemctl is-system-running` = running、0 个失败单元；iptables `v1.8.10 (nf_tables)`；Docker active。
- IPv6：容器 `eth0` 有 `fd10:213::2/64`，默认路由经 `fd10:213::1`；`curl -6` 清华镜像站 200（远端 `2402:f000:1:400::2`），`ping -6 2400:3200::1` 通；宿主 ip6tables 的 nat/FORWARD/INPUT 规则、`ip -6 rule` 6060/6070（→ ovnet0）都在。
- Docker：默认网桥上的容器有 `fd10:213:d0c::2`，两个容器之间 v4/v6 都能互通（br_netfilter 打开后也通），v4/v6 出网通，`-p 18091` 在容器 netns 和宿主上经 `10.213.0.2` 和 `[fd10:213::2]` 都能访问；`docker build`（legacy builder）正常。
- Podman：两个容器之间互通、出网通、`-p 18092` 宿主可访问；`podman build` 失败（见 §7）。
- 备份 → 恢复：备份 619 MB，从点按钮到完成约 70 秒（其中停容器约 60 秒被强杀，原因见 §7 Podman 那条）；恢复约 30 秒；之后启动引擎，新 dshns 从头建网络，日志无报错（"dsh-br0 10.213.0.1/16 fd10:213::1/64 <-> dsh-v0"），恢复后的容器 running、iptables 仍是 nft、旧钩子没回来。停容器后宿主：没有 `dsh-br0`、规则、策略路由和 v6 路由，`ip_forward` 回到原值 0，IPv6 转发原值是 1、保持 1。
- 局域网开关：打开时 v4、v6 的 FORWARD 都换成无条件 `-o dsh-br0 -j ACCEPT`，关掉后都换回 `RELATED,ESTABLISHED`，monitor 日志各一行。
- 引擎在容器里（诊断页"当前运行方式 系统容器"），手机操控 relay 从 `10.213.0.2` 连到 `10.213.0.1`。
- **热点不测**：平板不能插卡，用户决定不管（br_netfilter 在宿主默认关闭，按理不受影响；v6 的转发恢复在开着热点时的判断也没测）。
- **手机操控**（APK 11071503）：新对话 "Phone control test…" 里 agent 调 `mcp__phone__screen` 成功（见 §7 手机操控那条）。
- **新镜像从头装**（APK 11071503）：引擎页停止容器 → 删除系统容器 → 启动引擎，17 秒内解压镜像 `…a9e6d2a742ed`、新建 32 GB 存储、首次启动设好 37 项属主、引擎就绪；旧叠加文件都不在，`pids.max` 16384。`apt-get install docker.io podman` 不做任何手工处理：iptables 默认就是 `nf_tables`，Docker 第一次就 active，`systemctl is-system-running` = running、0 个失败单元，Docker 的 IPv6 MASQUERADE 在。（装包时 `xz-utils 1ubuntu0.3` 又是 404，测试里钉了 noble 原版并 `--allow-downgrades`；原因和修法见下面"镜像不再带 apt 索引"，之后已升回 `0.4`。Docker Hub 在设备不挂代理时连不上，测试从 `docker.m.daocloud.io` 拉 alpine，再 `docker save | podman load`。）Docker 容器 v4/v6 出网、`docker build`、`-p` 宿主经 `10.213.0.2` 和 `[fd10:213::2]` 访问都通；Podman 出网、`podman build --network host`、`-p` 都通。Docker 容器默认 `pids.max` = 2457，是容器里 systemd 的 DefaultTasksMax（容器上限 16384 的 15%）；Podman 默认 2048。`/root/.dsh/AGENTS.md` 和手册技能的常见问题里都有 `podman build` 要加 `--network host`。

平板当前：APK 11071503；容器是新镜像从头装的（旧容器连同里面的 Docker/Podman 数据已删，今天的备份还在），装了 docker.io 和 podman（`xz-utils`/`liblzma5` 是降级后的 noble 原版），Docker 和 Podman 各有一个 `alpine`；引擎在容器里（3080）；「局域网访问容器」关，Docker 接管开；测试容器已删；对话列表里多了一个 "Phone control test…" 对话；`Download/DSH-Go/backups` 里多了一份 `dsh-system-20261107-144555.tar.gz`（619 MB，含对话和 API 配置）。

### 镜像不再带 apt 索引（2026-11-07，APK 11071546，已提交）

- **原因**：LXC 镜像带着它构建时（10 月 18 日）的 `/var/lib/apt/lists`（30 个文件，约 204 MB），我们原样打进了镜像；App 解压时不保留 mtime，所有文件的 mtime 都是解压时刻。`apt-get update` 拿本地文件的 mtime 发 If-Modified-Since，上游 InRelease 只要发布得比解压早，就回 304，apt 显示 `Hit`，继续用 10 月 18 日的索引；索引里的版本（如 `xz-utils`/`liblzma5 1ubuntu0.3`）在仓库里已被新版替换，下载就 404。所有"10 月 18 日之后更新过的包"都会这样，直到上游再发布一次索引。第五个会话那次 404 是同一原因（当时误判成镜像站没刷新）。
- **修法**（用户选的第一种）：`rtbuild.py` 的 `cmd_system_image` 丢掉 `var/lib/apt/lists/` 下的普通文件（只留目录，报告里记为 `apt-lists`），和 Ubuntu 官方容器镜像一样；提示词本来就让 agent 先 `apt-get update`。镜像 `s1-ubuntu-noble-20261018_1335-2b194891b81e`，解压后 679 → 475 MB，压缩包 134 → 99 MB，APK 347 → 310 MB；文案里的"约 680 MB"改成"约 480 MB"（`phoneSystemHint`、`phoneSystemStartInstallHint`、FAQ）。另一种做法（解压时保留 mtime）没采用。
- 平板上的容器（a9e6d2a742ed 解压的，仍带旧索引的那个）：删掉 `/var/lib/apt/lists/*` 后重新 update，拿到当天的索引，`xz-utils`/`liblzma5` 升回 `5.6.1+really5.4.5-1ubuntu0.4`；另有 15 个包可升级（镜像之后的正常更新，没动）。
- **新镜像从头装又验证了一遍**（APK 11071546，用户要求"确保新装 App 状态下不会磕磕绊绊"）：删除系统容器后启动引擎，15 秒就绪，rootfs 是 `…2b194891b81e`，`/var/lib/apt/lists` 里 0 个文件；没 update 直接 `apt-get install` 报 "Unable to locate package"（正常，提示词要求先 update）；`apt-get update && apt-get install -y docker.io podman` 不加任何参数，56 秒装完，没有 404，`xz-utils`/`liblzma5` 直接是 `1ubuntu0.4`；iptables `nf_tables`，Docker 第一次就 active，running、0 个失败单元。Docker v4/v6 出网、`docker build`、`-p` 宿主经 v4/v6 访问，Podman 出网、`podman build --network host`、`-p` 都通（alpine 仍从 daocloud 拉，设备没挂代理时 Docker Hub 不通）。测试容器已删。
- 平板当前：APK 11071546（重装后无障碍服务仍绑定着）；容器是 `…2b194891b81e` 从头装的，装了 docker.io 和 podman，Docker、Podman 各有一个 `alpine`；引擎在容器里（3080）；「局域网访问容器」关，Docker 接管开；`Download/DSH-Go/backups` 里有今天 14:46 的备份；对话列表里有 "Phone control test…" 对话。


## 10. tun/FUSE 设备节点、`/home` 放进容器存储（2026-11-10 定；第七个会话已实现、真机验收，未提交）

用户要求在**新对话**里做这一轮。本节是自包含的：背景、实测事实、决定、做法、文案、验收都在这里。代码位置以 `66bb44e` 为准。实际怎么做的、验收结果和偏离计划的地方见本节「本轮实现与验证」。

### 背景

- 2026-11-07 到 11-10，用户在 App 的自检会话（`session-633a3c02`，"Runtime environment self-check with shell commands"）里又和 agent 聊了 5 轮（第 13–17 轮）。会话文件在平板上的 `files/runtime/rootfs/root/.dsh/sessions/--root-workspace--/session-633a3c02-…/session.v4.jsonl.zstd`，要用 `adb exec-out su -c cat` 拉下来再用 zstd 解压；agent 的回复在 `assistant/message` 行的 `data` 里，嵌套的 `{"type":"text","text":…}`。
  - 第 13 轮：复查新内核下的环境，提示词里新写的说法都成立。
  - 第 14 轮：用户说 `CONFIG_CGROUP_DEVICE` 不做了（开了会改动大量符号，修不过来）。
  - 第 15 轮：离正常 Linux / agent 沙盒还差多少。
  - 第 16 轮：目标定为"非 root 用户启动、能装包和用 Docker、能用大部分资源"。
  - 第 17 轮：能不能"可以 adb，但不能 adb shell su"。
- 核对 agent 说法时实测出本节的问题；agent 说错的地方见本节「还没定的方向」和 §7。

### 实测事实（2026-11-10，APK 11071546，新内核，镜像 `…2b194891b81e`）

- **两个设备节点打不开**：`setup_dev()`（`dshns.c` 里 `nodes[] = {"null", …, "fuse", "net/tun"}`）把宿主节点原样绑进容器的 `/dev`。
  - 宿主上 `/dev/fuse` 是 `root:root 0600`（`u:object_r:fuse_device:s0`），`/dev/net/tun` 是指向 `../tun` 的软链接，`/dev/tun` 是 `system:vpn 0660`（`tun_device`）。
  - 属主在容器里映射不到（显示为 nobody/nogroup），所以容器 root 也打不开：`exec 3<>/dev/fuse`、`/dev/net/tun` 都失败，`/dev/null` 能打开。
  - 容器的 `/dev` 是容器进程自己挂的 tmpfs（`uid=10346`，带 nosuid、noexec）。
- **受影响的**：
  - `podman build` 默认的 RUN 网络：slirp4netns 报 `open("/dev/net/tun"): Permission denied`。
  - rootless Podman（以 ubuntu 用户、`XDG_RUNTIME_DIR` 指向自建目录来测）：
    - 默认存储：`fuse: failed to open /dev/fuse: Permission denied`，连 `--network none` 都跑不起来。home 在 `/data` 的 f2fs 上，f2fs 开了 casefold，overlay 不能用，Podman 退回 fuse-overlayfs。
    - vfs 存储：`--network none` ✅；默认网络 ❌（slirp4netns 同上）；`--network host` ✅ 能出网；`podman build --network host` ❌ `while running runtime: exit status 1`，原因没查。
    - 测完已执行 `podman system reset` 并删掉 `~/.local/share/containers`。
  - FUSE 程序（sshfs、rclone mount、squashfuse、AppImage）、容器里的 VPN 客户端都用不了。
- **前提条件都在**：
  - 内核：`CONFIG_FUSE_FS=y`、`CONFIG_FUSE_PASSTHROUGH=y`、`CONFIG_VIRTIO_FS=y`。
  - 镜像：带 `fuse3`、`/usr/bin/fusermount3`（`fusermount` 是指向它的软链接）、`/etc/fuse.conf`。setuid 位按 `ownership.tsv` 在首次启动时恢复，rootfs 的 nosuid 已被 monitor 去掉（sudo 能用就是证明）。
  - seccomp 只拦 KernelSU 的提权调用；容器进程在 ksu 域，该域是 permissive。
- **`/dev/null` 能打开**：说明"把宿主侧建好的节点绑进容器"这条路可行，只要节点所在的文件系统不是在容器的用户命名空间里挂的（那样会被内核当作 nodev），也不在 `/data`（挂载时带 nodev）上。
- **`--device`**：只支持同名映射，与 `CGROUP_DEVICE` 无关（§7）。

### 决定

1. **tun**：照下面的做法改（用户："tun这个做法没什么问题"）。
2. **FUSE**：要完整可用，做法和 tun 一样，一起做。
3. **`/home`**：放进现有的 32 GB ext4 存储镜像，和 Docker、containerd、containers 共用一块，**不另挂一块**（用户："用户目录单挂ext4…和docker那个合并"）。rootless Podman 因此能用内核原生的 overlay，fuse-overlayfs 只是后备。
4. **改名**：「Docker 存储」改成「容器存储」，相关文案一起改（下面「文案」）。
5. **`/root` 不动**：它和兼容启动共用，仍是 proot rootfs 的绑定。

### 做法

#### 1. 设备节点（`dshns.c`）

- **建节点**：由 monitor 来建，它在宿主上是真 root，而且在 `private_rootfs()` 之后已有自己私有的挂载命名空间。
  - 在一个只有 monitor 和容器子进程看得见的地方挂一块很小的 tmpfs，挂载时**不要**带 `MS_NODEV`。位置放在 rootfs 之外，pivot_root 之后容器就看不到；例如 state 目录下的子目录：state 在 `/data` 上，但新挂的 tmpfs 用的是自己的挂载标志，不继承 `/data` 的 nodev。实现时要验证节点确实能打开。
  - 在里面 `mknod`：`fuse` 为 `c 10 229`，`net/tun` 为 `c 10 200`，权限 0666，属主 `opt.uid:opt.gid`（即容器 root）。
- **绑节点**：`setup_dev()` 绑定 `fuse`、`net/tun` 时，改成绑这两个自建节点，不再绑宿主的。宿主上原有的节点**不要 chmod、chown**，否则会改变 Android 的状态。
- **失败处理**：建节点失败时只记一行日志，容器里就不出现这个节点，不要再退回去绑那个打不开的宿主节点。节点不存在时，不少程序会自己换办法；存在却打不开，只会报权限错误。
- **两条启动路径都要有**：`start` 和 `run`（备份/恢复）都经过 `spawn()` → `container_main()` → `setup_dev()`。tmpfs 随 monitor 的挂载命名空间一起消失，不用清理。
- **安全**：
  - 容器里任何用户都能开 tun，但 tun 只能建在容器自己的网络命名空间里（建接口需要该网络命名空间所属用户命名空间里的 `CAP_NET_ADMIN`）。
  - 任何用户都能经 setuid 的 `fusermount3` 挂 FUSE，挂载只在容器里可见。
  - 这和 LXC 无特权容器的常见配置一样。代价是容器能碰到更多 FUSE 内核代码；卡住的 FUSE 进程会让访问它挂载点的进程挂起，但影响只在容器内。

#### 2. `/home` 放进容器存储（`dshns.c`）

- **现状**：`storage_attach()` 在 monitor 里把存储挂到 `var/lib/dsh-go/storage`，再把子目录 `docker`、`containerd`、`containers` 绑到 `var/lib/…`（用 `open_in`、`dir_in`，`RESOLVE_IN_ROOT`，不跟随软链接，见 §5d R1）。任何一步失败都 `die`，启动直接失败。所以 `/home` 放进去之后，不会出现带着空 `/home` 启动的情况，这一点要保持。
- **加 `home`**：在存储里加子目录 `home`，绑到 `/home`。
- **首次搬迁**：存储里还没有 `home` 时，要先把 rootfs 现有的 `/home` 搬过去，再绑。镜像自带 `/home/ubuntu`（skel 文件），老容器里可能还有用户文件。
  - 建议在容器里做（pid 1，pivot 之后、exec systemd 之前，和 `apply_ownership()` 同一时机），以容器 root 身份进行，路径都是容器自己的。可以 fork 执行容器里的 `cp -a --preserve=all /home/. <存储>/home/`，或者自己实现复制；复制完写一个标记，再 bind。
  - 如果在 monitor 里以宿主 root 复制，就要处理 rootfs 里的软链接（R1 的问题）。
  - 这样的话，`/home` 的绑定也挪到容器里做，monitor 只负责建好存储里的目录。这是建议的分工，实现时可以再权衡。
- **备份/恢复**：
  - 现在的打包方式是先 `--one-file-system` 打 `.`，再单独打 `./var/lib/dsh-go/storage`（`SystemContainer.BACKUP_SCRIPT`）。`/home` 绑在存储上之后，打 `.` 时会被跳过，存储那部分会把它带上，不会丢、也不会重复。
  - 要实测三件事：新格式备份能恢复；**旧格式备份**（`/home` 还在 rootfs 里的）恢复到新版本后，`/home` 的内容在、属主对；恢复后再启动不会重复搬迁。
- **扩容和删除**：`grow`、删除容器不用改：`/home` 的内容随存储一起扩容、一起删除。和以前一样，删除容器本来就会删掉 rootfs 里的 `/home`，只是以前没写出来，这次文案里要写明。
- **rootless Podman**：home 在 ext4 上以后，`podman info` 里应该是 `overlay`，而且原生 overlay 生效（如 "Native Overlay Diff: true"）。内核 6.12 支持在用户命名空间里挂 overlay；配合 tun 节点，pasta/slirp4netns 的默认网络也应该能通。都要实测。
  - 没有用户级 systemd 会话时会有警告，可选择执行 `loginctl enable-linger ubuntu`。这不在本轮范围。

#### 3. 提示词、常见问题、测试

- **`GuestSetup.kt`**（`SYSTEM_EXTRA`）：
  - 在"镜像和数据在单独的存储里"后补上"`/home` 也在这块存储上，空间不够时请用户在引擎页扩容"。
  - 默认的 `podman build` 实测通过后，删掉"`podman build` 要加 `--network host`…"那句；rootless Podman 和 FUSE 实测可用后，再各补一句。
  - `GuestAgentsModeTest` 里有对 `--network host` 那句的断言，要跟着改。
- **常见问题**：`docs/guide/{zh,en}/11-faq.md` 改完后，跑 `python3 app/tool/guide_sync.py` 同步到手册技能。

### 文案（`app/lib/l10n/parts/phone_{zh,en}.arb`，改完跑 `python3 app/tool/l10n_merge.py`）

| 键 | 现在（中文） | 改成 |
|---|---|---|
| `phoneSystemStorage` | Docker 存储 | 容器存储（Container storage） |
| `phoneSystemGrowHint` | 提高 Docker 和 Podman 存储的上限。… | 提高容器存储的上限（Docker、Podman 和 /home 共用）。… |
| `phoneSystemDeleteHint` | …含 Docker 存储；/root 和 DSH 的数据保留。 | …含容器存储（Docker、Podman 和 /home）；/root 和 DSH 的数据保留。 |
| `phoneSystemBackupHint` | 把系统、Docker 存储和 /root 打包… | 把系统、容器存储和 /root 打包… |
| `phoneSystemRestoreBody` | 软件、数据库和 Docker 数据会被删除… | 软件、数据库、/home 和 Docker 数据会被删除… |
| `phoneSystemHint` | Docker 和 Podman 的数据放在单独的存储里 | Docker、Podman 的数据和 /home 放在单独的存储里 |

英文的键同步改（`Docker storage` → `Container storage` 等）。常见问题里要改：

- 存储那条：改成"Docker、Podman 的数据和用户目录 `/home` 放在单独的容器存储里…"，并补一句"`/root` 不在里面，它和兼容启动共用"。
- 备份那条："Docker 存储" → "容器存储"。
- 删除那条："软件和 Docker 数据" → "软件、`/home` 和 Docker 数据"。

### 验收（真机，新内核）

1. **节点**：容器 root 能打开 `/dev/net/tun`、`/dev/fuse`；宿主上 `/dev/fuse`、`/dev/tun` 的属主和权限没变。
2. **构建**：`podman build` 不加参数能过 RUN 那一步；`docker build` 照常。
3. **FUSE**：
   - 容器 root 挂 sshfs（比如连容器自己的 sshd）；
   - 普通用户经 `fusermount3` 挂、卸；
   - squashfuse 挂一个镜像；
   - 跑一个 AppImage（老式的需要 `apt install libfuse2t64`）。
4. **rootless Podman**（ubuntu 用户）：
   - `podman info` 是原生 overlay；
   - `podman run` 默认网络能出网；
   - `-p` 能访问；
   - `podman build` 能过（之前 vfs 下 `exit status 1` 的原因若仍在，要查）；
   - 强制用 fuse-overlayfs 也能跑。
5. **`/home`**：
   - 老容器升级后，内容、属主、权限、setuid/xattr 都对，只搬一次；
   - 新装的容器里 `/home/ubuntu` 有 skel 文件；
   - 引擎页"容器存储"的用量包含 `/home`；
   - 存储故意弄坏时启动失败，不会带着空 `/home` 起来。
6. **备份/恢复**：新格式能恢复；旧格式（`/home` 在 rootfs 里）恢复到新版本后正常；恢复后启动不重复搬迁。
7. **回归**：引擎、手机操控 relay、Docker/Podman 的 v4/v6、`-p`、局域网开关、pids 上限都正常；停容器后宿主恢复原样（§9 本轮验证里列的那几项）。
8. **测试与检查**：Kotlin 单元测试、`flutter analyze`、相关 Dart 测试、`l10n_merge.py --check`、`guide_sync.py --check`、dshns `-Werror`。

### 不要做的

- 不要 chmod、chown 宿主的 `/dev/fuse`、`/dev/tun`；不要把宿主的 `/dev/loop*`、`/dev/block` 绑进容器。
- 不要为了 `--device` 去补 `CGROUP_DEVICE`（§7）。
- 不要把 `/root` 挪进存储。

### 本轮实现与验证（2026-11-10，第七个会话，APK 11101624，未提交）

改动：

1. **设备节点**（`dshns.c` 的 `make_nodes`、`setup_dev`）：`private_rootfs()` 末尾（`start`、`run` 都经过）在 monitor 的命名空间里把一块 16k 的 tmpfs（`MS_NOSUID|MS_NOEXEC`，不带 nodev）挂到 `state/dev`，在里面 `mknod` `fuse`（10,229）和 `net/tun`（10,200），0666，属主容器 root；`setup_dev()` 只从宿主绑 null/zero/full/random/urandom/tty，`fuse`、`net/tun` 改绑自建的。建不出来只记一行日志、容器里就没有这个节点；绑定失败时删掉占位文件。宿主节点不动。宿主上 `state/dev` 只是个空目录（挂载只在 monitor 的命名空间里）。
2. **`/home`**（`dshns.c` 的 `home_on_storage`）：在容器 1 号进程里做，pivot 之后、`apply_ownership()` 之后，`start` 和 `run` 都做。存储里没有 `home` 时：`rm -rf home.partial` → `cp -a /home <存储>/home.partial` → `syncfs` → `rename` 成 `home` → `syncfs` → `rm -rf --one-file-system /home`（rootfs 里的原件）；然后建空的 `/home` 挂载点，`touch /var/lib/dsh-go/home-on-storage`（rootfs 里的标记），再 bind。存储里没有 `home`、rootfs 里却有标记，说明存储丢过 `/home`，这时启动失败（"the container storage has lost /home: restore a backup, or delete the system container"），不会带着空 `/home` 起来。复制失败也是启动失败，原件不动，下次重试。cp/rm 是容器自己的程序，以容器 root 身份运行，路径都是容器自己的；**seccomp 过滤器挪到了它们之前**（原来在 `child_network()` 之后）。monitor 侧不用改：存储挂在 rootfs 的 `var/lib/dsh-go/storage`，容器里本来就看得到。
3. **签名带上 launcher**（计划外，第七个会话加的）：`runtimeVersion` 不含 `runtime/container/`，只改 dshns 时签名不变，已经在跑的老容器不会重启，也就用不上新的 `/dev` 和 `/home`。现在 `SystemContainer.Image.launcher` 读 `system.json` 的 `launcher.sha256`，`signature()` 把它放在 runtime 后面，换了 dshns 的 App 更新后，第一次启动引擎会重启一次容器（日志 "runs with an older runtime or other binds; restarting it"）。`SystemContainerTest` 加了断言。
4. **启动失败时的清理**（计划外；验收"存储弄坏"时发现，两处都是原来就有的问题，这一轮新加的失败路径也会走到）：
   - 容器 1 号进程失败（`spawn()` 读到 booted 消息）时先 `net_cleanup()`：以前会留下 `dsh-br0`、v4/v6 的 NAT 和 FORWARD 规则、`ip rule` 6060/6070，`ip_forward` 停在 1。
   - `start` 的 monitor 在报 ok 之前失败（如 e2fsck 失败）时，父进程先 `cg_remove(dsh-go/<name>)` 再 `cg_tidy()`：以前 `cg_prepare` 建的容器 cgroup 留着，`cg_release` 删不掉 `dsh-go`，根组的 `+pids` 和 `pids.root` 也就撤不掉。
5. **手机操控在容器重启后断开**（计划外；回归时发现，原来就有的竞态，第 3 条让它在 dshns 有改动的 App 更新后必然出现）：引擎启动时 phone bridge 先起来，这时旧容器还在，绑上了 `10.213.0.1`；随后 App 因签名变化重启容器，`dsh-br0` 被删，那个监听 socket 的 accept 报 "Socket closed"，但 `tcpBridge` 还指着它，5 秒后的 `addTcp()` 以为已经绑好，于是容器里的 relay 一直连不上（`ECONNREFUSED`），直到下次重启引擎。修法：`PhoneBridge.acceptTcp` 对网关 socket 失败时调 `forgetBridge(s)`（同一个对象才清，`@Synchronized`），`addTcp()` 照冷启动那条路重新绑。没有单元测试（测试机上绑不了 10.213.0.1），真机复现并验证过（见下）。
6. **文案、提示词、常见问题**：按上面「文案」表改了 6 个键，另外 `phoneSystemDeleteBody`（中英）也补了 `/home`；跑过 `l10n_merge.py`。`GuestSetup.SYSTEM_EXTRA`：补"`/home` 也在这块存储上"、"普通用户也能用 rootless Podman"，删掉 `podman build --network host` 那句，新加一条"`/dev/fuse` 和 `/dev/net/tun` 可用：sshfs、squashfuse、AppImage 等 FUSE 程序能用（老式 AppImage 要先装 `libfuse2t64`），root 可以建 tun 接口（VPN 客户端要用）"；`GuestAgentsModeTest` 跟着改。FAQ（中英）按「文案」那三条改，并删掉 `podman build` 要加 `--network host`；跑过 `guide_sync.py`。`SystemContainer.kt`、`system_container_section.dart` 里的注释同步改名。

检查：Kotlin 791 个通过；`flutter analyze` 无问题；`flutter test test/engine test/runtime test/l10n test/diagnostics test/engine_settings` 484 个通过（144 个跳过）；`l10n_merge.py --check`、`guide_sync.py --check` 通过；dshns arm64 `-Werror` 和 x86_64 `-fsyntax-only -Werror` 通过。`node --test runtime/tests` 没跑（没改 bridge/patches）。Kotlin 和 Dart 是在 11101551 那版代码上跑的；之后只改了 `dshns.c`（`syncfs` 顺序），重编了 dshns（`-Werror`）并在设备上重跑了 T1–T4，Kotlin/Dart 代码没变、没重跑。

真机验收（新内核；APK 11101511，之后 11101542 加了第 4 条，11101551 加了第 5 条，11101624 把第 2 条的 `syncfs` 改成 rename 前后各一次，见 §11.4「第一版的顺序问题」）：

1. **节点**：容器里 `/dev/fuse`、`/dev/net/tun` 是 `crw-rw-rw- root root`，容器 root 和 nobody 都能打开；宿主 `/dev/fuse` 仍是 `root:root 0600 fuse_device`，`/dev/tun` 仍是 `system:vpn 0660 tun_device`。
2. **构建**：`podman build` 不加参数，RUN 里能联网；`docker build` 照常。
3. **FUSE**：容器 root 用 sshfs 挂容器自己的 sshd（临时用户 fusetest）、读文件、`umount`；ubuntu 用户经 setuid 的 `fusermount3` 挂 squashfuse 和 sshfs、`fusermount3 -u` 卸；root 挂 squashfuse；AppImage：新式（appimagetool continuous，静态 runtime）直接跑；老式（AppImageKit 12 的 appimagetool，runtime 要 `libfuse.so.2`）装了 `libfuse2t64` 后能跑（它自己还要 `libz.so`，装了 `zlib1g-dev`，和 FUSE 无关）。root 能建、删 tun 接口；nobody 建不了（`TUNSETIFF` EPERM，要 `CAP_NET_ADMIN`，和 LXC 一样）。
4. **rootless Podman**（ubuntu，`su - ubuntu` 自带 `XDG_RUNTIME_DIR=/run/user/1000`，不用 linger）：`podman info` 是 `overlay`、`Native Overlay Diff: true`、Backing Filesystem `extfs`；默认网络（slirp4netns，`tap0 10.0.2.100`）出网；`-p 18093` 在容器里、宿主（App uid 经 `10.213.0.2`）都能访问；`podman build` 能过（之前 vfs 下的 `exit status 1` 没再出现）；强制 `overlay.mount_program=/usr/bin/fuse-overlayfs`（单独的 `--root`）能跑，容器根是 `fuse.fuse-overlayfs`。
5. **`/home`**：
   - 老容器升级（APK 11101511 第一次启动）：先在 `/home/ubuntu/mig` 放了测试文件（属主 ubuntu 的 setuid、root:ubuntu 的 setgid、`user.*` xattr、访问 ACL 和默认 ACL、硬链接、软链接、fifo、100 MB 稀疏文件、0600 文件、属主 1234:5678 的文件、2001 年的 mtime、带空格的文件名）。搬迁后逐项比对：属主、权限、大小、链接数、mtime 全部一致（只有目录大小 3452→4096），xattr、ACL、SELinux 标签逐个文件一致，硬链接还是同一个 inode，稀疏文件仍只占 4 KB；存储上的 setuid 生效（nobody 运行 root 的 4755 程序得到 euid 0），默认 ACL 照常继承。rootfs 的 `/home` 清空，标记写好。
   - 只搬一次：之后的每次启动 monitor 日志都没有 "moved"。
   - 新装：删除系统容器后启动，`first boot: set owner and mode of 37 image entries` 之后 `/home moved to the container storage`，`/home/ubuntu` 里是 skel 的三个文件、属主 ubuntu。
   - 用量：往 `/home` 写 200 MB，`storage.img` 从 80 MB 到 280 MB，删掉后回到 80 MB（discard）。
   - 失败路径（用单独的测试容器 `files/systest`：从 APK 里的镜像以 App uid 解出，`dshns run --name dshtest`，和用户的容器互不影响；脚本见 §11.8 的 `systest.sh`，测完已删）：
     - T1 新 rootfs、没有存储：建存储 → `first boot: set owner…` → `/home moved…`；`/home` 在 `loop50[/home]` 上，skel 三个文件在，标记写好，宿主 `rootfs/home` 为空；`/dev/fuse`、`/dev/net/tun` 在 `run` 里也能打开。
     - T2 再跑一次：没有 "moved"。
     - T3 复制失败（rootfs `/home/ubuntu` 里放一个宿主 root 所有、0600 的文件，容器 root 读不了）：`cp: cannot open '/home/ubuntu/secret' for reading: Permission denied`、`copying /home to the container storage failed (exit 1)`，退出码 125；rootfs 里的原件都在，标记没写。
     - T4 把那个文件改成 0644 后重跑：搬迁成功，存储里只有 `home`（`home.partial` 已被清掉）；那个文件复制后属主是 `nobody:nogroup`（原属主映射不到）。
     - T5 删掉 rootfs 的 `/home` 挂载点（模拟崩在删原件和 mkdir 之间）：照常 bind。
     - T6 删掉标记、存储里有 `home`：照常启动并补写标记。
     - T7 标记在、存储是新建的：`the container storage has lost /home…`，退出码 125。
     - 11101624 的 dshns（`syncfs` 顺序改过之后）又跑了 T1–T4，结果相同。
     - 每一步之后宿主上没有 `dsh-go/dshtest`，根组 `pids` 和用户容器的 `pids.max` 不受影响；dshns 用的 loop 都带 autoclear、用完即释放。（我自己用 toybox `mount -o loop` 去看存储内容时，路径超过 64 字节，toybox 挂载失败却留下了两个没有 autoclear 的 loop，已 `losetup -d`；这和 dshns 无关。）
   - 复制速度（为了估计 11.6 第 1 条）：容器里从 rootfs（f2fs）`cp -a` 到 `/home`（存储）再 `sync`：2 GiB 大文件 1.2 s，2 万个小文件 0.6 s（页缓存是热的）。
   - 存储弄坏（从 adb 用 App 的参数直接调 dshns，原存储挪开保留，测完放回）：全零的镜像 → `e2fsck ... exit 8; the storage needs repair`；镜像不见了（dshns 新建一个空的）→ "the container storage has lost /home…"。两种都不启动；修了第 4 条之后，两种失败后宿主都干净（没有 `dsh-br0`、规则、策略路由，`ip_forward` 回原值，根组 `subtree_control` 空，没有 `dsh-go`、`pids.root`）。
6. **备份/恢复**：
   - 旧格式备份 X（`dsh-system-20261110-151413.tar.gz`，旧 APK 做的，`/home` 在 `./home/ubuntu/...`）恢复到新版本：约 24 秒；rootfs 的 `/home` 为空，内容在存储里，和搬迁前的记录逐项一致；恢复后启动不再搬。
   - 新格式备份 Y（`dsh-system-20261110-152554.tar.gz`）：`./home/` 只有目录项，内容在 `./var/lib/dsh-go/storage/home/...` 下，没有重复，`./var/lib/dsh-go/home-on-storage` 在；恢复约 24 秒，内容、xattr、ACL、setuid、硬链接、稀疏都对，启动不再搬。
7. **回归**：引擎在容器里（3080）；手机操控：无障碍服务绑定着，relay 从 `10.213.0.2` 连到 `10.213.0.1:<端口>`（ESTABLISHED）；第 5 条修之前复现了断开，修后用「接管 Docker 配置」关、开各重启一次引擎（每次都是 bridge 先绑、容器再重启），两次都是 "Socket closed" 之后重新绑上、relay 连上。Docker：v4 出网、容器有 `fd10:213:d0c::2`、`ping -6` 和按域名的 https 通，`-p 18091` 宿主经 `10.213.0.2` 和 `[fd10:213::2]` 都通；Podman：v4 出网，`-p 18092` 宿主经 v4 通（v6 见 §7）。局域网开关：开时 v4/v6 都换成无条件 `-o dsh-br0 -j ACCEPT`，关时换回 `RELATED,ESTABLISHED`，monitor 日志各一行。pids：根组和 `dsh-go` 的 `subtree_control` = `pids`，`pids.max` 16384。停容器后：没有 `dsh-br0`/`dsh-v0`、v4/v6 的规则和策略路由、v6 路由，`ip_forward` 和 IPv6 转发回到原值（这次都是 0），根组 `subtree_control` 空，没有 `dsh-go`，loop 释放；mangle 里剩下的那条 TCPMSS 是 netd 的 `tetherctrl_mangle_FORWARD`。
- **没做的**：没有在新对话里让 agent 真正调一次 `mcp__phone__*`（只看了无障碍绑定和 relay 的 TCP 连接）；没有测 rclone mount（提示词里没写它）；`node --test runtime/tests` 没跑。

测试时的副作用（都已恢复）：
- UI 自动化用子串匹配点 "停止" 时命中了整个系统容器区块，点在区块中心，正好关掉了「接管 Docker 配置」，下一次启动时 App 删了 `daemon.json`（Docker 没有 IPv6、FORWARD 策略变回 DROP，Podman 的 `-p` 从宿主连不上）；同一次还点开了「退出 DSH Go（含容器）」的确认框，已取消。开关已重新打开，`daemon.json` 已重写，偏好是 `dockerTakeover=true`、`systemLanAccess=false`。以后点按钮按精确文字或 checkable 节点的 bounds 点。
- 测 FUSE 时装过 acl、attr、sshfs、squashfuse、squashfs-tools、openssh-server、libfuse2t64、file、zlib1g-dev（共 58 个包，含依赖），已按 apt 历史全部 purge，`ssh.socket` 已停，临时用户 fusetest 已删；测试文件（`/home/ubuntu/mig`、`/root/mig-*.txt`、容器 `/tmp` 里的）已删；ubuntu 的 rootless Podman 存储已 `system reset`。
- 11101624 装上后打开 App：签名里的 launcher 变了，容器自动重启一次（又出现一次 bridge 的 "Socket closed"，随后重新绑上、relay 连上）；`/home` 仍在存储上、没有重搬，节点、systemd 正常，`daemon.json` 在，`dockerTakeover=true`、`systemLanAccess=false`。
- 平板当前：APK 11101624；容器是备份 Y 恢复的（新布局：`/home` 在存储上），装着 docker.io 和 podman（各有 alpine）；引擎在容器里（3080），手机操控 relay 连着；「局域网访问容器」关，Docker 接管开；`Download/DSH-Go/backups` 里多了两份今天的备份：X（15:14，旧格式，装着上面那些测试包和 mig 测试文件）、Y（15:26，新格式，测试包已卸、mig 文件还在）；两份都含对话和 API 配置，要不要删由用户决定。

### 还没定的方向（第 15–17 轮讨论，核实后的结论，留给用户决定）

- **内存/CPU 限制**：agent 两次说"内核都有，只差宿主写 `subtree_control` 委派"，这是**错的**。Android 把 memory、cpu、cpuset 都挂在 cgroup v1 上（`/dev/memcg`、`/dev/cpuctl`），cgroup2 的 `cgroup.controllers` 里只有 pids（§4）。`docker --memory`、`--cpus` 要改 Android 的 cgroup 布局才可能生效。可行的是另一件事：dshns 在 v1 memcg 里给整个容器设一个内存总上限（防止容器把手机内存吃光），这样 `docker --memory` 依然不生效。容器内装 earlyoom 是另一种兜底。
- **agent 以非 root 用户工作**（第 16 轮）：agent 说只差"加 docker 组、开 linger、`/sdcard` 走 sudo"，漏了最大的一件：
  - **引擎身份**：引擎由 `dshns exec` 以容器 root 启动，数据在 `/root/.dsh`（和兼容启动共用），工作区是 `/root/workspace`。换成 ubuntu 用户要改 App 启动引擎的身份、HOME/`DSH_HOME`、工作区和文件属主。
  - **手机存储**：非 App uid 访问 `/sdcard` 报 "Bad address"（Android 的存储层只认 App 自己的 uid，只有容器 root 映射到它），"访问手机文件"会断。
  - **权限等价**：有免密 sudo（镜像里 `/etc/sudoers.d/90-incus`）和 docker 组时，"非 root"只是默认身份，不是安全边界；真正的边界仍是用户命名空间。
  - **rootless Podman**：本节修完后才可能可用。
- **让 agent 开发调试 Android App**（第 17 轮）：
  - **adb 和 su 分不开**：SukiSU 按 uid 决定给不给 root。容器里 adb 进来的和用户在 WSL 里 adb 进来的都是 shell uid 2000，没法区分；拒绝 shell 的 su，会连我们自己的 `adb shell su` 调试流程（§2）一起断掉。
  - **adbd 暴露**：容器要连 adbd，得让它在网络上监听（无线调试或 `adb tcpip`），局域网也就能连到它。
  - **更合适的做法**：在 App 已有的手机操控通道（有 token，只对容器网段开放，已有读屏、点击）上加白名单式的开发工具：安装自家包、启动、按 pid/uid 看日志、`run-as`。不要放进 dshns：它以宿主 root 运行，应保持最小。
  - **工具链**：Android SDK 的 `aapt2`、`zipalign`，以及 Gradle 从 Maven 拉的 aapt2，在 Linux 上都只有 x86_64 版本；arm64 容器里构建 APK 要另找 arm64 的 aapt2，并用 `android.aapt2FromMavenOverride` 覆盖。apt 里有 arm64 的 `adb`。
- **agent 别的说法**（第 15 轮的百分比表是它估的，不是测的）：
  - "`/lib/modules` 不存在所以 `=m` 不可用"无关紧要：容器本来就不能加载内核模块。
  - "`fstrim` 失败"不影响：存储挂载时带 discard，删除的空间已实测会还给手机。

## 11. 第七个会话的审查材料（2026-11-10）

用户要用另一个模型审这一轮，要求记录尽量全。§10「本轮实现与验证」是结果摘要；本节是细节：改了哪里、为什么这样改、没采用的办法、没处理的情况、建议重点看的地方，以及复现用的脚本。

### 11.1 范围与基线

- 基线是 `635f493`（`system-container` 分支 HEAD，已推送到 fork）。本轮改动全在工作区，**没提交**：`git diff 635f493`，20 个文件，含本文和生成物。
- 手写的源码和输入：
  - `runtime/container/dshns.c`
  - `app/android/app/src/main/kotlin/io/github/ccch1mneyyy/dsh_app/runtime/SystemContainer.kt`、`RuntimeController.kt`（一行）、`GuestSetup.kt`
  - `app/android/app/src/main/kotlin/io/github/ccch1mneyyy/dsh_app/phone/PhoneBridge.kt`
  - 测试：`app/android/app/src/test/.../runtime/SystemContainerTest.kt`、`GuestAgentsModeTest.kt`
  - 文案输入 `app/lib/l10n/parts/phone_{zh,en}.arb`；文档 `docs/guide/{zh,en}/11-faq.md`；注释 `app/lib/ui/engine/system_container_section.dart`
- 生成物（由工具生成，不用逐行审）：`app/lib/l10n/app_{zh,en}.arb`、`app/lib/l10n/gen/*`（`python3 app/tool/l10n_merge.py`）；`app/assets/skills/dsh-go-guide/{zh,en}-11-faq.md`（`python3 app/tool/guide_sync.py`）。
- 没改：镜像（`rtbuild.py`，镜像版本仍是 `s1-ubuntu-noble-20261018_1335-2b194891b81e`）、bridge 和补丁、Dart 逻辑。
- 构建：`runtime/build-runtime.sh --system-only arm64-v8a`（重编 `libdshns.so`、重写 `system.json`，产物在不进 git 的 `runtime/out`），再 `tooling/build-apk.sh -f arm64`，之后 `git checkout app/pubspec.lock`。设备上是 APK 11101624（launcher sha256 `c5971ed7…`）。

### 11.2 `dshns.c` 逐处改动

- **常量**：`STORAGE_DIR` 从存储一节挪到文件开头（容器 1 号进程也要用）；新增 `HOME_STORE`（`/var/lib/dsh-go/storage/home`）、`HOME_MOVED`（rootfs 里的 `/var/lib/dsh-go/home-on-storage`）；加 `<sys/sysmacros.h>`（`makedev`）。
- **`run_with(argv, envp)`、`run_tool(argv)`、`guest_tool(argv)`**：原来的 `run_tool` 挪到前面并把环境变量参数化。宿主工具（mke2fs、e2fsck、resize2fs）照旧继承 `environ`；容器自己的程序用干净的环境（只有 `PATH`、`LANG`），不带 su 传下来的 Android 环境。子进程的 stdout 仍重定向到 stderr（`run` 的 stdout 是备份数据流）。
- **`OWN_NODES[]`、`own_dev[]`、`make_nodes()`**：`private_rootfs()` 末尾调用。这是 `start` 和 `run` 都经过、且 monitor 已经有了私有挂载命名空间的位置。在 `state/dev` 建目录、`chown` 给容器 root，挂 `tmpfs`（`MS_NOSUID|MS_NOEXEC`，`mode=755,size=16k`，不带 nodev）；逐个建父目录、`mknod`、再 `chmod 0666`（不受 umask 影响）、`chown`。任何一步失败只 `say` 一行，`own_dev` 置空或删掉那个节点。
- **`setup_dev()`**：从宿主绑定的列表去掉 `fuse`、`net/tun`，剩下的六个仍是失败即返回 -1（原来的 `i < 6` 就是这个意思）；之后对 `own_dev` 里存在的节点 `bind_into`，失败时删掉 `bind_into` 先 touch 出来的占位文件，免得留下一个不是设备的 `/dev/fuse`。
- **`home_on_storage()`**：见 11.4（`syncfs` 的顺序在写审查材料时改过一次，见 11.4 末尾）。
- **`child_abort()`**：从 `child_fail()` 拆出来的"写消息、退出"，给不带 errno 的消息用。
- **`container_main()`**：顺序改成 pivot → `apply_ownership()` → `seccomp_guard()` → `home_on_storage()`（有 `--storage` 时）→ `child_network()` → exec。seccomp 提前，是为了让 cp/rm 这些容器程序也在过滤器下跑；`child_network()` 只用 netlink 和 ioctl，不受过滤器影响（它只拦 KernelSU 的魔法 reboot）。
- **`spawn()`**：从 booted 管道读到失败消息时，`die` 之前先 `net_cleanup()`（`opt.net` 只有 `start` 会设，`run` 不受影响）。
- **`cmd_start()` 的父进程**：monitor 没回 ok 时，先 `cg_remove(dsh-go/<name>)` 再 `cg_tidy()`。
- 注释：存储一节的开头、`storage_attach()`、`private_rootfs()`、`setup_dev()`。

### 11.3 App 侧逐处改动

- `SystemContainer.Image` 加 `launcher: String? = null`，`bundled()` 读 `system.json` 的 `launcher.sha256`。
- `SystemContainer.start(context, args, runtime, launcher, dockerTakeover, log)`、`signature(runtime, startArgs, dockerTakeover, launcher)`：签名的第二行是 launcher 的 sha256（`listOfNotNull`，读不到就不加）。签名格式因此变了：**升级到这一版后，第一次启动引擎一定重启一次容器**（旧签名里没有这一行），老容器就是这样用上新 `/dev` 和 `/home` 的。
- `RuntimeController.prepareSystemContainer()`：把 `image.launcher` 传给 `start`。
- `SystemContainer` 的注释：`STORAGE_SIZE`；`BACKUP_SCRIPT` 补了一句"dshns 从存储绑进来的（`/home`、`/var/lib/docker` 等）是挂载点，`.` 会跳过，由存储那部分带上"。
- `GuestSetup.SYSTEM_EXTRA`、`GuestAgentsModeTest`：见 §10 第 6 条。
- `PhoneBridge.forgetBridge(s)`（`@Synchronized`，只在 `tcpBridge === s` 时关掉并置空）；`acceptTcp()` 的 catch 里，`containerOnly`（网关那个 socket）时调用它。
- `SystemContainerTest.anAppUpdateKeepsTheContainerButNewBindsRuntimeOrLauncherRestartIt`（改名并加了 launcher 不同的断言）。

### 11.4 `/home` 的状态和处理（`home_on_storage()`，容器 1 号进程，pivot 之后）

| 存储里有 `home` | rootfs 里有标记 | 处理 | 实测 |
|---|---|---|---|
| 没有 | 没有 | 首次搬迁：`rm -rf home.partial` → `cp -a -- /home <存储>/home.partial` → 对存储 `syncfs` → `rename` 成 `home` → 再 `syncfs` → `rm -rf --one-file-system -- /home`（失败只记日志）→ 写标记 → `mkdir /home` → bind | T1；老容器升级；新装 |
| 没有 | 有 | 启动失败："the container storage has lost /home: restore a backup, or delete the system container" | T7；`start` 也测过 |
| 有 | 没有 | 写标记 → bind（崩在 rename 和写标记之间时，或者旧格式备份恢复，见下） | T6 |
| 有 | 有 | `mkdir /home`（挂载点不在时，崩在删原件和 mkdir 之间）→ bind | T2、T5 |

- 复制失败（cp 返回非 0）：启动失败，原件不动、不写标记，`home.partial` 留在存储里，下次启动先清掉再重来（T3、T4）。
- **恢复备份**：`restore()` 先删容器、重新解镜像，再用 `run` 解备份。`run` 里会先把**新镜像的 skel** 搬进新存储，然后 tar 才解备份：新格式备份的 `./var/lib/dsh-go/storage/home/...` 覆盖上去；旧格式备份的 `./home/...` 经刚 bind 好的 `/home` 解进存储。所以旧格式恢复后，skel 里有、备份里没有的文件（`.bashrc` 等三个）会留着，和 `restore()` 注释里"the image's own files it no longer had stay"是同一个语义。
- **备份**：`.` 带 `--one-file-system`，`./home` 是挂载点，tar 只记它的目录项（属性取自存储里的 `home` 根），不进去；内容由第二个操作数 `./var/lib/dsh-go/storage` 带上。实测见 §10 第 6 条。
- **崩溃窗口**（只做了分析，没实测断电）：
  - 第一次 `syncfs` 完成之前崩：存储里只有 partial（或什么都没有），下次清掉重来；原件在。
  - 第一次 `syncfs` 之后、第二次之前崩（rename 可能进了日志，也可能没进）：复制的数据已经落盘，所以要么是完整的 `home`（按第三行处理，原件还在被盖住的 rootfs `/home` 里，只是白占空间），要么仍是 partial（重来）。
  - 第二次 `syncfs` 之后、删原件中途崩：存储里的 `home` 完整，rootfs 里残留的部分原件被盖住。
  - 删完原件、`mkdir` 之前崩：第四行的 `mkdir`（T5）。
- **第一版的顺序问题**（写这份审查材料时发现，11101624 修掉）：11101511–11101551 的顺序是 cp → rename → `syncfs`。ext4 的日志每 5 秒提交一次，开着延迟分配时，rename 这条元数据可能比复制出来的文件数据先提交；那时断电，重启后存储里已经有 `home`、被当成搬完了，里面的文件却可能是空的或截断的（原件没删，因为删原件在 `syncfs` 之后，但不会自动换回来）。现在是 rename 前先 `syncfs`，rename 后再 `syncfs`，再删原件。用户的容器是用旧顺序搬的；搬的过程中没有崩溃，搬完逐项比对过一致，所以不受影响。

### 11.5 设计取舍（为什么这样做、没采用的办法）

- **节点建在 monitor 的 tmpfs 里，而不是容器自己的 `/dev`**：容器在自己用户命名空间里挂的 tmpfs 带 `SB_I_NODEV`，里面的节点打不开；`/data` 带 nodev。monitor 是 init 用户命名空间里的真 root，它挂的 tmpfs 没有这个限制，节点绑进容器后仍能打开，和 `/dev/null` 能打开是同一个道理。挂在 `state/dev`：在 rootfs 之外，pivot 之后容器看不到；挂载只在 monitor 的命名空间里，宿主上只是个空目录；`dshns exec` 是 setns 进容器挂载命名空间的，也看不到。
- **不动宿主节点**：用户定的（§10「不要做的」）。
- **0666**：容器里所有用户都能打开。tun 接口只能建在容器自己的 netns（`TUNSETIFF` 按 netns 所属的用户命名空间检查 `CAP_NET_ADMIN`，nobody 实测 EPERM）；FUSE 挂载只在容器的挂载命名空间里可见。和 LXC 无特权容器的常见配置一样。代价：容器能碰到更多 FUSE 内核代码；卡住的 FUSE 守护进程会让访问它挂载点的进程挂起（只影响容器）。
- **搬迁放在容器 1 号进程，用容器自己的 cp/rm**：路径都是容器自己的，不会出现 rootfs 里的软链接把宿主 root 引到外面的问题（§5d R1）；以容器 root 身份运行，没有宿主权限。没选在 monitor 里用宿主 root 复制：那样每一级都要 `RESOLVE_IN_ROOT`，还得自己实现 xattr、ACL、硬链接、稀疏文件。没自己写复制：`cp -a` 这些都处理了，实测全对。
- **复制而不是 rename**：rootfs 在 f2fs，存储是 ext4，跨文件系统。
- **删原件**：用户说的是"搬"。不删的话原件会被 bind 盖住，备份也带不上（`.` 不进挂载点），只是白占 `/data` 的空间。删之前先 `syncfs`。
- **rootfs 里的标记**：用来区分"从来没搬过"和"搬过但存储丢了 `/home`"。没有它的话，存储被换成新的时候（比如手动删了 `storage.img`，dshns 会新建一个），会把已经空了的 rootfs `/home` 当成原件"搬"过去，静默地起一个空 `/home`；有了它就明确失败。代价是这种情况下要恢复备份、删容器，或者手动删掉标记。
- **复制失败就启动失败**：不做"搬了一部分也照样启动"，免得丢文件。代价见 11.6 第 2 条。
- **签名带上 launcher**：`runtimeVersion` 的输入哈希只覆盖 `patches bridge boot direct`，不含 `container/`；镜像版本的输入也不含 `dshns.c`。也考虑过"只改 dshns 时不重启"（让用户自己停容器），但那样老容器会一直用旧的挂载和 `/dev`，不符合 `start()` 注释里"its mounts would still be the old ones"的原则。用 `system.json` 里的值，而不是在设备上现算 `libdshns.so` 的哈希：省一次 470 KB 的读，而且两者本来就不一样（gradle 又 strip 了一次，见 §7）。NDK 的构建是确定性的：同一份源码，我在 scratchpad 里手编的和 `build-runtime.sh` 编出来的 sha 都是 `f92bbc56…`，所以只重建 runtime 的其他部分不会误触发重启。
- **失败清理**：只在已知的两处失败点补清理，没做通用的退出钩子（`die` 散在各处，改动面大）。`run` 那条路没补（11.6 第 5 条）。
- **PhoneBridge**：只在网关 socket 的 accept 失败时把字段清掉，让现有的 `addTcp()`（引擎就绪 5 秒后）去重新绑定；没改引擎启动顺序（把 bridge 挪到容器准备好之后，影响面大）。为什么地址被删时 accept 会报 "Socket closed"，内核行为没去查；两次复现都是这样。就算别的内核上监听不会失败，旧 socket 绑的地址被重新加回来以后应该也还能用。

### 11.6 已知限制和没处理的情况（大致按严重程度）

1. **第一次搬迁的耗时和 App 的 60 秒启动超时**：`SystemContainer.start()` 用的是 `RootShell.run(..., timeoutMs = 60_000)`。超时后 App 会 `destroy` 掉 `dshns start` 的父进程，并记一次系统容器启动失败（`systemLaunchFailed`：之后改用直接或兼容启动，直到用户重新打开开关）。monitor 是 setsid 的，会继续搬完、把容器起来。按实测速度（2 GiB 1.2 s，2 万个小文件 0.6 s，页缓存热），只有 `/home` 有几十 GB 或几百万个文件时才会碰到。没处理。可选的办法：放宽 start 的超时；或者 dshns 搬迁时先往 ready 管道报进度，App 据此延长等待。
2. **`/home` 里有复制不了的文件时，容器会一直启动失败**：比如设备节点（用户命名空间里 mknod 不了），或者属主映射不到、其他人又不可读的文件（T3）。错误信息说的是复制失败，monitor.log 里有 cp 的原话；要从宿主（adb root）处理掉那个文件，或者删容器。正常的容器里基本不会有这种文件。
3. **属主映射不到的文件，复制后属主变成 nobody**（T4）：`cp -a` 看到的属主就是 overflow uid 65534，而 65534 在容器里是映射得到的真实用户。
4. **没测的**：
   - `trusted.*` 的 xattr：容器 root 设不了，`cp -a` 对 xattr 失败默认不报错，结果应该是静默丢掉。
   - 断电或崩溃窗口：11.4 只做了分析。
   - `make_nodes()` 的失败路径：建不出节点时，容器里就没有这个节点。
   - 这一版上的在线扩容（`grow`）没重测（代码没动）。
   - 没在新对话里让 agent 真调一次 `mcp__phone__*`；rclone mount 没测；`node --test runtime/tests` 没跑。
5. **`dshns run`（备份、恢复）失败时，仍可能留下 `dsh-go` 和根组的 `+pids`**：`cmd_run` 里 `storage_attach()` 或 `spawn()` 失败会直接 `die`，不走 `cg_release()`。只在没有别的容器在跑、`run` 自己打开了 pids 时出现。原来就有，这一轮没修（只修了 `start` 的两处）。
6. **`make_nodes()` 的 `mkdir`/`chown`/`mount` 会跟随 `state/dev` 上的软链接**：`state` 目录属于 App，App 可以在那里放软链接，让宿主 root 把 tmpfs 挂到别处、或改别的文件的属主。但 App 本来就有 root（它就是用 su 启动 dshns 的），这不构成新的越权边界；容器看不到 `state`。如果要收紧，可以改成先 `lstat` 确认是目录，再 `lchown`。
7. **签名带 launcher 之后**：每次 dshns 有变化，App 更新后都会重启一次容器，容器里的服务会被打断。另外 `systemLaunchFailed` 记的是 `runtimeVersion`（不含 launcher），所以如果某个 dshns 版本启动失败，换一个修好的 dshns（runtime 没变）不会自动清掉这条记录，要用户手动重新打开开关。
8. 其余见 §7 这一轮新加的几条：`GuestFiles` 读不到 `/home`、`launcher.sha256` 和 APK 里的字节不同、引擎页用量不实时刷新、Podman 默认网络没有 IPv6、`podman build` 的 ambient 警告、启动失败后残留 `storage.dev`。

### 11.7 建议重点审查

- `home_on_storage()` 的状态表和崩溃窗口有没有遗漏；"复制失败就启动失败""存储丢了 `/home` 就启动失败"这两个选择合不合适。
- 搬迁的持久化顺序（`syncfs` → rename → `syncfs` → 删原件，11.4 末尾），以及 rootfs 那边的标记不做 fsync 是否够用（标记只用来判断"存储丢了 /home"，丢了标记会在下次启动补写）。
- seccomp 挪到 `apply_ownership()` 之后、容器程序之前，有没有破坏什么。
- 自建节点 0666 向整个容器开放的安全面（11.5）。
- 签名带 launcher 的代价（11.6 第 7 条）。
- 两处失败清理的顺序：`spawn()` 里先 `cg_remove` 再 `net_cleanup`；`cmd_start` 父进程 `cg_remove` 的时候 monitor 已经退出，锁还在父进程手里，不会和另一个 start 并发。
- `PhoneBridge.forgetBridge()` 的并发：`stop()` 或 `start()` 关旧 socket 时，旧的 accept 线程调 `forgetBridge` 按对象身份比较，不会清掉新绑的 socket；它和 `addTcp()` 都在实例锁里。
- 备份和恢复的 tar 语义（11.4）。

### 11.8 复现

设备操作的约定见 §2。下面是这一轮实际用的脚本，放在会话 scratchpad 里、没进仓库，这里原样记下（路径按本机：`~/android-sdk/platform-tools/adb`，App 包名、uid 10346 写死）。

- 在用户的系统容器里以容器 root 跑一个本地脚本：`cx.sh SCRIPT [TIMEOUT]`（就是 §2 的 fifo 做法；脚本跑完 `dshns exec` 会结束它起的所有进程，后台起的东西也会一起没了，所以要从宿主访问的服务得在脚本里 `sleep` 撑住，同时从外面去连，见 §10 rootless Podman 那条）。
- UI：`ui.py`（dump、按文字点）、`sw.py`（按开关的 checkable 节点读或切状态）、`stop-engine.sh`、`restart-engine.sh`。**按钮一律用 `tapx`（整段文字精确匹配）或 checkable 节点的 bounds 去点**：子串匹配会命中整个区块，点在区块中心（这一轮就是这样误关了「接管 Docker 配置」）。引擎页的"停止"要先滚回顶部；确认框打开时，底下页面的节点不在 dump 里。
- `/home` 搬迁的比对：`prep.sh`（旧 APK 下装测试工具，在 `/home/ubuntu/mig` 放测试文件，记 `/root/mig-before*.txt`）、`check1.sh`（升级后比对 manifest 和节点）、`check2.sh`（按文件比对 xattr 和 ACL，测存储上的 setuid）。注意 `prep.sh` 装的包这一轮已经全部卸掉了，重跑前要重新装。
- 失败路径：`systest.sh`（在设备上以 root 跑：`adb exec-in "su -c 'cat > /data/local/tmp/systest.sh'" < systest.sh`，再 `adb shell su` 里 `sh /data/local/tmp/systest.sh`）。前提是先把 APK 里的镜像以 App uid 解到 `files/systest/rootfs`：
  ```sh
  A=/data/user/0/io.github.ccch1mneyyy.dsh_app/files
  adb shell su <<EOF
  mkdir -p $A/systest/rootfs $A/systest/state; chown -R 10346:10346 $A/systest; chmod 700 $A/systest
  EOF
  printf 'cd %s/systest/rootfs || exit 9\ntar -x\n' $A | adb exec-in "su 10346 -c 'cat > $A/systest/x.sh'"
  unzip -p app-arm64-release.apk assets/runtime/system-arm64-v8a.tar.xz | xz -dc | adb exec-in "su 10346 -c 'sh $A/systest/x.sh'"
  adb shell su -c "chmod 755 $A/systest/rootfs"   # App 解完镜像也会这样做
  ```
  （`su <uid> -c 'a && b'` 和 `su -c` 一样只有第一条以那个身份执行，所以要包成脚本；设备上没有 `xzcat`，在本机解压后再传。）测完 `rm -rf $A/systest`。
- 复制速度：`speed.sh`（经 `cx.sh` 在用户容器里跑，会写 2 GiB 再删掉）。
- 检查命令：`cd app/android && ./gradlew :app:testArm64DebugUnitTest`；`cd app && flutter analyze`；`DSH_TEST_FONT_DIR=<字体目录> env -u http_proxy -u https_proxy -u HTTP_PROXY -u HTTPS_PROXY flutter test test/engine test/runtime test/l10n test/diagnostics test/engine_settings`（沙箱外；字体目录放 `/usr/share/fonts/opentype/noto/NotoSansCJK-{Regular,Bold}.ttc`，Light 用 Regular 的软链）；`python3 app/tool/l10n_merge.py --check`；`python3 app/tool/guide_sync.py --check`；dshns：`$NDK/aarch64-linux-android26-clang -static -O2 -Wall -Wextra -Werror -s -o /tmp/libdshns.so runtime/container/dshns.c` 和 `$NDK/x86_64-linux-android26-clang -fsyntax-only -Wall -Wextra -Werror runtime/container/dshns.c`（`NDK=~/android-sdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/linux-x86_64/bin`）。

#### 脚本原文

拆分时挪到了 [`runtime/container/tests/`](../runtime/container/tests/README.md)：`cx.sh`、`ui.py`、`sw.py`、`stop-engine.sh`、`restart-engine.sh`、`prep.sh`、`check1.sh`、`check2.sh`、`systest.sh`、`speed.sh`，内容和原来代码块里的一样。

## 12. 审查和审查后的小改（2026-11-10，第八个会话，APK 11102027，已提交 4017336）

用户让另一个会话审第七个会话的工作（材料是 §11），之后按审查结果拆了交接文档，并做了三处小改。

### 审查结论

主线没有走歪，第七个会话的实现可以提交。逐项核对过、认为没问题的：设备节点的做法、`/home` 的四种状态和崩溃窗口、`syncfs` 的顺序、"复制失败或存储丢了 `/home` 就不启动"、seccomp 提前（原来容器里的 `ip` 是在过滤器之前跑的，提前是净改进）、两处失败清理的顺序和加锁、`PhoneBridge.forgetBridge()` 的并发、文案中英同步。发现的问题和处理见下面。

### 用户的决定

- **签名带 launcher**：接受（原话见入口文档 §1）。
- **agent 以非 root 工作**：用户原以为 agent 已经是非 root。讨论后定为引擎保持 root，提示词里告诉 agent 某个程序拒绝 root 时用 `su - ubuntu -c '命令'`。整体换成非 root 会碰到的问题记在入口文档 §6「agent 以非 root 用户工作」。
- **交接文档拆分、测试脚本进仓库**：同意，已做。

### 改动

1. **提示词**（`GuestSetup.SYSTEM_EXTRA` 末尾加一条，`GuestAgentsModeTest` 加断言）：某个程序拒绝以 root 运行时用 `su - ubuntu -c '命令'`；`ubuntu` 进不了 `/root`（包括工作区）和 `/sdcard`，文件要复制过去、结果复制回工作区，用户在 App 里打不开 `/home` 下的文件；其他时候照常用 root。
2. **monitor 忽略 SIGPIPE**（`dshns.c` 的 `cmd_start`）：在回 `ok` 之前 `signal(SIGPIPE, SIG_IGN)`。放在 `spawn()` 之后，容器的 1 号进程是之前 clone 的，不会继承；monitor 之后起的宿主工具（iptables、ip）会继承，没有影响。
3. **`dshns run` 失败时清理**（`dshns.c`）：`die()` 退出前调 `on_die`（函数指针，只有 `cmd_run` 在通过"容器在跑或忙"的检查之后才登记）；登记的是 `run_release()` = `cg_remove(dsh-go/<名字>)` + `cg_release()`，正常结束也走它。容器 1 号进程那一侧不调 `die()`（用 `child_fail`/`_exit`），不会从容器里触发。
4. **文档**：入口文档重写成现状文档（207 行起），本文是拆分前的全文加本节；脚本挪到 `runtime/container/tests/`，新增 `runfail.sh`。

### 评估后不改的

- **首次搬迁时没有 `/home`、或 `/home` 是相对软链接**：首次搬迁今后只会从干净的镜像开始（新装；恢复备份时先解镜像、`run` 先搬再解包），用户平板上的旧布局容器已经搬完，所以走不到；处理软链接要加第五种状态并上机测。
- **60 秒启动超时**：同样走不到；而且 App 杀不掉 `dshns start`（见入口文档 §5）。
- **软链接 `state/dev`、`systemLaunchFailed` 不含 launcher** 等 §11.6 里已列的：不变。

### 检查和验证

- dshns：arm64 `-static -O2 -Wall -Wextra -Werror` 编过（sha256 `d37e6799…`，手编的；没跑 `build-runtime.sh`，`system.json` 和 APK 都没重做），x86_64 `-fsyntax-only -Werror` 通过。
- Kotlin：`./gradlew :app:testArm64DebugUnitTest` 791 个通过。`l10n_merge.py --check`、`guide_sync.py --check` 通过（这一轮没改文案和手册）。Dart 测试、`flutter analyze` 没跑（没改 Dart）。
- 真机（平板上仍是 APK 11101624，用户的容器一直在跑、没动）：
  - 提示词里的说法：经 `cx.sh` 在用户的容器里以 ubuntu 实测——`/root` 是 `root:root 700`、`/root/workspace` 进不去，`ls /sdcard` 报 "Bad address"，`su - ubuntu` 有 `XDG_RUNTIME_DIR=/run/user/1000`、`HOME=/home/ubuntu`，`sudo -n true` 成功。
  - `run` 的清理：把手编的 dshns 传到 `/data/local/tmp`，用 `runfail.sh` 的做法（容器名 `dshtest`、空 rootfs）对比。旧版（APK 里的）在 `storage_attach` 失败后留下 `dsh-go/dshtest`；新版在 `storage_attach` 失败、容器 1 号进程失败两种情况后都没有 `dsh-go/dshtest`。前后用户容器的 `init.pid`、`pids.max` 不变，根组 `subtree_control` 仍是 `pids`。测试文件已删。
  - 传上去的文件直接执行会报 "Text file busy"（adb 的写入端还没放手），要先 `cp` 一份再执行。
- 以上是打包前做的；打包后的验收见下一小节。

### 打包上机（APK 11102027，用户同意后做）

`runtime/build-runtime.sh --system-only arm64-v8a`（launcher sha256 `d37e679963b1…`，和手编的一致；镜像仍是 `…2b194891b81e`）→ `tooling/build-apk.sh -f arm64` → `adb exec-in` 传上去、两边 sha256 一致 → `am force-stop` → `pm install -r -t`。装之前把旧版 `libdshns.so`（APK 11101624 的）留了一份在 `/data/local/tmp` 做对照，测完已删。

- **先停用户的容器**（新版 `dshns stop`，1 秒）：停后宿主干净（没有 `dsh-br0`、v4/v6 规则、`dsh-go`，根组 `subtree_control` 空，`ip_forward` 和 IPv6 转发都是 0，loop 释放）。
- **`run` 失败的回收**（`runfail.sh`，用户的容器停着）：
  - 旧版：`storage_attach` 失败后 `dsh-go/dshtest` 在、根组 `pids`、`pids.root` 在；容器 1 号进程失败后 `dshtest` 没了（`spawn()` 自己删的），但 `dsh-go`、根组 `pids`、记录都留着。测完手动 `rmdir dsh-go`、根组写 `-pids`。
  - 新版：两种失败后 `dsh-go` 不在、根组 `subtree_control` 空、没有 `pids.root`。
- **SIGPIPE**（`sigpipe.sh`，测试容器 `dshtest`：APK 里的镜像解到 `files/systest/rootfs`，不带存储和绑定，systemd 能起来）：脚本在 monitor 进了自己的 cgroup 之后立刻 `kill -9` 调用方（调用方没来得及打印 `running`）。
  - 旧版：容器起来了，**monitor 没了**（`MONITOR alive: NO`）；`stop` 等满 30 秒后强杀（"container did not halt in 30s"），之后宿主上 `dsh-br0`、`ip rule`、`dsh-go/dshtest`、`monitor-dshtest`、根组 `pids`、`pids.root` 都留着，`ip_forward` 停在 1。比审查时估计的更糟：不只是不收尾，连正常停止都要等超时。
  - 新版（接着上面的残留跑）：启动时把残留清掉重建；容器起来后 monitor 还在；`stop` 立刻返回，monitor.log 有 "container init killed by signal 2"；之后宿主干净（网桥、v4/v6 的规则和 NAT、`dsh-go`、根组 `pids`、记录都没有，`ip_forward` 和 IPv6 转发回到 0）。
  - 第一版脚本用 `[ -s cgroup.procs ]` 等 monitor，cgroup 文件大小永远是 0，等了 5 秒才去杀，调用方早已正常退出，那一次什么也没测到；改成读内容后才有上面的结果。
  - 顺带确认：没有存储、没有绑定的测试容器可以完整地 `start`/`stop`，只要用户的容器停着。
- **回到用户的容器**：删掉 `files/systest` 和 `/data/local/tmp` 下的测试文件；打开 App，引擎自动启动，容器由 App 启动（`started` 里的 launcher 是 `d37e6799…`）。经 `cx.sh` 查：`systemctl is-system-running` = running、0 个失败单元；`/home` 在 `loop49[/home]`（ext4），monitor.log 没有 "moved"；`/dev/fuse`、`/dev/net/tun` 是 `crw-rw-rw- root root`；`/root/.dsh/AGENTS.md` 里有"某个程序拒绝以 root 运行时"那一条；手机操控 relay 从 `10.213.0.2` 连到 `10.213.0.1:44559`（冷启动：bridge 先起来时还没有网桥，5 秒后 "phone bridge: added 10.213.0.1:44559"）；Docker active、`daemon.json` 在；`pids.max` 16384；出网 204；无障碍服务绑定着；宿主 `127.0.0.1:3080` 只有一个中继在监听。
- **没做的**：没有在新对话里让 agent 真用一次 `su - ubuntu`，也没有真调 `mcp__phone__*`；Dart 测试和 `flutter analyze` 没跑（没改 Dart）；`node --test runtime/tests` 没跑。
- 副作用：打包前那次探测以 ubuntu 跑过 `sudo -n true`，在 `/home/ubuntu` 留下一个空的 `.sudo_as_admin_successful`（sudo 自己建的标记）。

## 13. agent 以 `ubuntu` 跑程序、手机操控的实测（2026-11-10 晚，第九个会话，APK 11102027，只改文档）

做的是入口文档 §6 第 1 步：在 App 里开新对话，让 agent 真用一次 `su - ubuntu`，顺便真调一次 `mcp__phone__*`。没改代码、没重新打包；平板上仍是 APK 11102027。

### 怎么发消息

`ui.py tapx 新对话` → 点输入框 → `adb shell "input text '...'"`（空格写成 `%s`）→ `ui.py has` 核对全文 → `ui.py tapx 发送`。搜狗输入法没有把 `input text` 当拼音（试过 `abc%sdef`，原样进了输入框）。中文发不了（设备上的 `cmd clipboard` 是 "No shell command implementation"，没装 ADBKeyBoard），所以消息用英文写、末尾加 "Please reply in Chinese."。引擎的 HTTP 接口要启动 token 换 cookie，没走那条路。

agent 的每一步从会话记录里读：`/root/.dsh/sessions/--root-workspace--/session-<id>/session.v4.jsonl.zstd`（zstd 压缩的 JSONL，事件类型有 `user/message`、`assistant/message`、`tool/call`、`tool/result`、`turn/end` 等），经 `cx.sh` 以 base64 取回本机解压。已做成 `runtime/container/tests/trace.sh`（带 `trace.py`）。界面上的轨迹会把命令和输出截断。

### 测试 1：PostgreSQL `initdb`（对话「搭建临时 PostgreSQL 测试实例」，`session-f9d05e63`）

消息（故意不提 `ubuntu`、`su`、`/home`）："Set up a throwaway PostgreSQL instance for testing: make a fresh cluster with initdb, not the Debian managed cluster or its systemd service, run it on port 5433, create a table, insert a row and read it back. Then tell me how to connect to it. Please reply in Chinese."

1 分 01 秒、9 步完成。经过：

1. 第一步的思考里直接引用了 AGENTS.md："initdb refuses to run as root; use `su - ubuntu -c '...'`. Also ubuntu can't access /root, so put data dir in /home/ubuntu."。同一步就跑了 `id ubuntu` 和 `su - ubuntu -c 'id -u; echo $HOME'` 确认，还 `df -h /home` 看了存储（`/dev/block/loop49 32G`）。**从头到尾没有以 root 跑 `initdb` 再被拒**。
2. `apt-get install -y postgresql postgresql-contrib`（33 秒），Debian 的 `main` 集群随之建好（5432）；它 `systemctl stop` + `disable` 了这个服务，免得混淆。
3. `install -d -o ubuntu -g ubuntu -m 700 /home/ubuntu/pgtest ...`，然后 `su - ubuntu -c 'initdb -D /home/ubuntu/pgtest/data -U postgres --auth-local=trust ...'`。以 root 用 heredoc 写的 `postgresql.conf`、`pg_hba.conf` 随后 `chown ubuntu:ubuntu`——审查后担心的「root 复制进 `/home/ubuntu` 的文件 ubuntu 改不了」，这次它自己处理了。
4. `su - ubuntu -c 'pg_ctl ... -w start'`，监听 `0.0.0.0:5433` 和 `[::]:5433`；建库、建表、插一行、读回；分别经 `127.0.0.1:5433` 和 `10.213.0.2:5433` 用 TCP 连上。
5. 回复：连接信息表、读写结果、启停命令（注明要以 `ubuntu` 跑）、`trust` 只做测试的提醒、清理办法。

**发现的问题：`su -` 丢掉了时区。** apt 以 root 建的 `main` 集群选了 `Asia/Shanghai`，`su - ubuntu` 下的 `initdb` 选了 `Etc/UTC`，读回的 `created_at` 是 `+00`。原因：
- App 启动引擎时把手机的时区放进环境（`RuntimeController.kt:508`，`env["TZ"] = TimeZone.getDefault().id`），引擎和 relay 的进程环境里都是 `TZ=Asia/Shanghai`，agent 的 root shell 继承了它。
- 容器的系统时区是 `Etc/UTC`（`/etc/localtime -> ../usr/share/zoneinfo/Etc/UTC`，`/etc/timezone` 也是）。`su -` 是登录 shell，清掉环境，`ubuntu` 下的程序就回到 UTC。
- agent 发现后用 `ALTER DATABASE demo SET timezone = 'Asia/Shanghai'` 补了一下。
- 实测（容器里，`TZ=Asia/Shanghai`）：`su - ubuntu -c date` 是 `UTC+0000`；`su - ubuntu -w TZ -c date` 是 `CST+0800`（util-linux 2.39.3 的 `-w`/`--whitelist-environment`）；`systemd-run --wait --pipe date` 是 `UTC+0000`。也就是说，**不经过 agent shell 的程序——systemd 服务、cron、Docker 的守护进程——本来就一直是 UTC**，`su -` 只是又多了一处。根治是让容器的系统时区跟手机（入口文档 §6 第 4 条，用户还没定）。

清理（经 `cx.sh`）：`pg_ctl stop`；删 `/home/ubuntu/pgtest`；按 `/var/log/apt/history.log` 里这次安装的 `Install:` 行 purge 了 17 个包（postgresql、postgresql-16、-contrib、-client-16、-common、-client-common、libpq5、libllvm17t64、libxslt1.1、sysstat、ssl-cert、libsensors5、libsensors-config 和 4 个 perl 库）；删 `/var/lib/postgresql`、`/etc/postgresql*`、`/var/log/postgresql`、用户 `postgres`、组 `ssl-cert`。之后没有相关包、没有 5432/5433 的监听、没有 postgresql 的 unit，`systemctl is-system-running` = running。`/home/ubuntu` 里的 `.cache`、`.config`、`.local`、`t`、`.sudo_as_admin_successful` 是之前几轮留下的，没动。

### 测试 2：`mcp__phone__screen`（对话「用手机控制工具读取屏幕内容」「读取手机屏幕内容并总结」）

消息："Use the phone control tools to read what is on the screen right now, and summarize it in two or three sentences. Please reply in Chinese."（发之前看过 `dumpsys accessibility`，Bound services 里有「DSH 手机操控」。）

- **第一次（`用手机控制工具读取屏幕内容`）被中止**：`mcp__phone__screen` 0.1 秒内返回了 32 项的屏幕结构（就是 DSH Go 自己的对话页），回复写到一半，这一轮以 `aborted`/`user` 结束，界面显示「已停止」。原因是我在用 `ui.py has` 轮询：每次 `uiautomator dump` 都注册一个 UiAutomation（logcat：`Registering UiTestAutomationService (id=com.android.server.accessibility/UiAutomation, flags=0x0)`），不带 `FLAG_DONT_SUPPRESS_ACCESSIBILITY_SERVICES`，系统就把别的无障碍服务断开，紧接着 `DshA11y: accessibility service gone: 无障碍服务已关闭`，约 1 秒后 `accessibility service connected`。`PhoneA11yService.teardown()` → `onStopRequested()` → 正在跑的手机操控任务被停掉，这一轮跟着中止。是测试方法的问题，不是 App 的问题（正常使用时没有人跑 uiautomator）。
- **第二次（`读取手机屏幕内容并总结`）**：发完消息等 40 秒不碰屏幕再看。1 次工具调用（返回 33 项），回复完整、内容对（"当前屏幕显示的是 DSH 手机 App 本身……"），`turn/end` 的 reason 是 `completed`。
- 两次的会话开始时都有一条 "The approval policy changed from "ask" to "never" (changed by the user)"：新对话先是 `workspace-write`/`ask`，随即被 App 切到 `danger-full-access`/`never`（界面上「已切换为完全权限」），这是用户的默认设置。

### 结论

- 提示词那一条够用：agent 没被提醒就选了 `su - ubuntu`，数据放进 `/home/ubuntu`，自己处理了属主。审查后建议的「复制后 `chown`」这次没有需要，先不加。
- 新问题只有时区，归到入口文档 §6 第 4 条「时区跟随手机」一起定。可选的做法：提示词改成 `su - ubuntu -w TZ -c`（只修 `su` 这一处）；或者容器启动时把 `/etc/localtime` 设成手机的时区（systemd 服务、cron、日志都对，手机换时区后要等下次启动容器）。
- 测试方法：agent 用手机操控时不能 dump 界面；进度用 `trace.sh` 看。

### 本轮改的文件（都是文档和测试脚本，和 §14 的时区改动一起提交）

- `HANDOFF-system-container.md`：§0、§2（界面自动化）、§5、§6 第 1 条和第 4 条。
- 本文：本节。
- `runtime/container/tests/`：新增 `trace.sh`、`trace.py`；`README.md` 加了这两个、发消息的办法、uiautomator 断开无障碍的注意事项；`ui.py` 的说明加了同一条警告。

## 14. 容器时区跟随手机（2026-11-10 定、2026-11-12 验完，第九个会话，APK 11102106）

§13 发现 `su - ubuntu` 下的程序是 UTC。给用户的两种做法：提示词改成 `su - ubuntu -w TZ -c`（只修这一处），或者容器的系统时区跟手机（systemd 服务、cron、日志也对）。用户选了第二种。

### 改动

- `SystemContainer.followTimezone(context, log)`：取 `TimeZone.getDefault().id`，调下面这个，变了就记一行 "[dsh-app] system container time zone: <zone>"，抛异常就记 "[dsh-app] system container keeps its time zone: <原因>"。
- `SystemContainer.followTimezone(rootfs, zone)`（`internal`，单测直接调）：
  - `zone` 要匹配 `[A-Za-z0-9_+-]+(/[A-Za-z0-9_+-]+)*`（挡住 `..`、`GMT+08:00` 这类）；`rootfs/etc` 必须是 rootfs 自己的目录（`canonicalPath == path`，和 `dockerConfig` 同一个规矩：rootfs 里的东西归容器，里面的软链接不能让 App 写到别处）；`usr/share/zoneinfo/<zone>` 解析后必须是 zoneinfo 下面的普通文件（tzdata 里的链接名，比如 Android 还在报的 `Asia/Calcutta` → `Kolkata`，可以用）。
  - `etc/localtime` 不是指向 `../usr/share/zoneinfo/<zone>` 的软链接时：先删 `etc/.localtime.dsh-app`，在那里建软链接，再 `ATOMIC_MOVE` + `REPLACE_EXISTING` 改名过去（rename(2)，替换的是链接本身，不跟随）。
  - 镜像有普通文件 `etc/timezone`、内容不同时：同样走临时名 `etc/.timezone.dsh-app`，用 `CREATE_NEW` + `NOFOLLOW_LINKS` 写（容器抢先放一个同名软链接只会让这次失败，不会让 App 写到别处），再改名。没有 `etc/timezone` 的镜像不新建。
  - 返回是否改了东西。
- `RuntimeController`：在写/收回 Docker 配置之后、`setLanAccess` 和 `SystemContainer.start` 之前调 `followTimezone`。每次走系统容器路径启动引擎都会调，不管容器要不要重启。
- **不进容器签名**：rootfs 就是容器的 `/`（monitor 只是把它自绑定一次），App 在宿主侧改的链接容器里立刻可见；手机换时区不该为此重启容器（会打断 Docker 里跑的东西）。代价：已经在跑的进程要看它自己什么时候重读（glibc 在 `localtime()` 时会检查 `/etc/localtime` 有没有变，systemd 的 1 号进程监视这个文件），新起的进程一定是新值。
- 引擎进程的 `TZ`（`RuntimeController.kt` 里原来那行）保留。提示词、文案、FAQ 都没改。
- 测试（`SystemContainerTest`）：
  - `theContainerTakesThePhonesTimeZoneFromItsOwnTzdataOnce`：UTC → `Asia/Shanghai`（链接和 `etc/timezone` 都改、第二次返回 false、`etc` 里不留临时文件）；`Asia/Calcutta` 这种 tzdata 里的链接名；没有 `etc/timezone` 的镜像不新建。
  - `aZoneTheImageLacksOrAnEtcOutsideTheRootfsChangesNothing`：`Mars/Olympus`、`Asia`（目录）、`Asia/Out`（指到 zoneinfo 外面的软链接）、`../../../etc/passwd`、`GMT+08:00`、空串都抛异常且什么都不改；`etc` 被换成指向 rootfs 外面的软链接时抛 `IllegalStateException`，外面的目录不多出文件、原文件不变。

### 检查和验证

- Kotlin：`./gradlew :app:testArm64DebugUnitTest` 793 个通过（原 791 + 新 2）。没改 Dart、文案、手册，`flutter analyze`、Dart 测试、`l10n_merge --check`、`guide_sync --check` 没跑。dshns 没改。
- 打包：`tooling/build-apk.sh -f arm64` → APK 11102106（310 MB），`git checkout app/pubspec.lock`；`exec-in` 传上去、两边 sha256 一致（`d47d1be8…`）、`pm install -r -t`。
- 2026-11-10 21:09 装上、打开 App：引擎照常起来，**容器没重启**（`init.pid` 装前装后都是 `11074 28305725`，dshns 没变所以签名没变）；`rootfs/etc/localtime -> ../usr/share/zoneinfo/Asia/Shanghai`、`etc/timezone` = `Asia/Shanghai`、`etc` 里没有临时文件；`files/runtime/logs/dsh.log` 有 "system container time zone: Asia/Shanghai"。
- 接着平板崩溃重启（见下一小节），容器内的检查到 2026-11-12 才做。那时容器已经被 App 起停过几次，最近一次由 App 正常启动（`system container running 18199`）；这次启动没有时区日志，因为链接已经是 `Asia/Shanghai`，符合"没变就不写"。经 `cx.sh`：
  - `unset TZ` 的 root shell、`su - ubuntu`、`systemd-run --wait --pipe date` 都是 `CST+0800`；`timedatectl` 是 `Asia/Shanghai`；`journalctl` 的时间戳是 `+08:00`；引擎进程仍有 `TZ=Asia/Shanghai`；`systemctl is-system-running` = running。
  - 容器在跑时改链接是否立刻生效：在容器里把 `/etc/localtime` 临时指到 `Europe/Berlin`，新起的 systemd 服务和 `su - ubuntu` 都是 `CET+0100`；改回 `Asia/Shanghai` 后新服务是 `CST+0800`。
- 没测的：真的在手机上换时区（会改用户的系统设置）；`keeps its time zone` 那条路只有单测。

### 我在设备上把平板搞崩了一次（2026-11-10 21:11）

- **经过**：装好 APK 后，为了确认 App 写了日志，我在 `adb shell su` 里跑了 `grep -rls 'system container time zone' /data/user/0/io.github.ccch1mneyyy.dsh_app/files`。这条命令找到了 `dsh.log`，接着读到 `files/system/storage.img`（名义 32 GB，实际只占 75 MB，其余是零、没有换行）。toybox 的 `grep` 按行读，把整个文件当一行往内存里读。`adb shell` 起的进程（含 `su` 之后）继承 adbd 的 `oom_score_adj -1000`，内核杀不掉它。
- **内核日志**（`/data/persist_log/backup/SYSTEM_LAST_KMSG.txt`，上一次开机的 kmsg；同一份在 `.../minidump/SYSTEM_LAST_KMSG@...dat.gz`）：uptime 47410 秒（21:11:28）起持续回收内存，47423–47429 秒 OOM 依次杀了容器里的 `MainThread`（引擎）、`dockerd`、`containerd`、`systemd`，然后是蓝牙、systemui、电话、`system_server`、`iptables`、`libdshns.so`（monitor）……杀到没有可杀的进程，47429.94 秒（21:11:48）"Kernel panic - not syncing: System is deadlocked on memory"，随后 "Causing a QCOM Apps Watchdog bite!"。被杀进程的 anon-rss 都只有几十 MB 以内，被杀列表里没有 grep。21:12:22 开机完成，`persist.sys.oplus.abnormalreboot_type` = `dump`（内核崩溃），`ro.boot.bootreason` = `reboot`。我那条命令返回 exit 255（连接断在执行中），时间对得上。
- **验证**：重启后实测 `adb shell` 和 `adb shell su` 的 `oom_score_adj` 都是 -1000；toybox `grep -c nomatch` 读一个 200 MB 的稀疏文件，峰值 RSS 约 390 MB。
- **后果**：容器的存储在崩溃时是挂着的；下一次启动时 `e2fsck -p` 打出 "dsh-storage: recovering journal"，随后 "clean"，容器正常起来。`/home` 早就搬完，不在搬迁中。没有发现别的损失。
- **以后**：入口文档 §2 加了警告——不要在设备上对 App 的数据目录跑递归 `grep`；找文件用 `find -name`，搜内容先限定目录或排除 `storage.img`；崩溃记录在哪看。拉回本机的 45 MB minidump（内存镜像，可能有隐私数据）已删，只留了 kmsg 文本在 scratchpad。
- 另外一个容易犯的错（这一轮犯了两次）：`adb shell su -c 'a; b'` 只有 a 以 root 执行，后面的 `[ -d /proc/$P ]`、`ls` 以 shell 身份跑，看不到别人的进程、读不了 App 的目录，会误判"容器没起来""权限不够"。一律用 `adb shell su <<'EOF'`（入口文档 §2 早就写了）。

## 15. 合并上游的难度评估（2026-11-12，第九个会话，**没合**）

用户要求"先看一波合并难度，这一轮只看，不真的合"。只做了 `git fetch origin`（更新了 `origin/*` 和 tag v1.0.11–v1.0.15）和 `git merge-tree` 预演，没有动分支和工作区。

### 怎么复现

```sh
git fetch origin
git merge-base HEAD origin/main                      # bf62b596（2026-11-02，v1.0.9-2）
git rev-list --count $(git merge-base HEAD origin/main)..origin/main
git merge-tree --write-tree --name-only --messages HEAD origin/main   # 第一行是树，之后是冲突文件，空行后是 CONFLICT 说明
git show <树>:<文件>                                  # 看带冲突标记的合并结果
```

比较文件清单时用 `LC_ALL=C sort` + `comm`（否则 `comm` 报 "not in sorted order"，结果不全）。

### 规模（`origin/main` = `708bdc32`，2026-11-12 13:48）

- 上游：1337 个提交（first-parent 234），v1.0.9 → v1.0.15，3012 个文件，+381688/−36672。
- 我们（到 `7c717ce`）：9 个提交，66 个文件，+8203/−212。
- 两边都改过：34 个文件，16 个冲突（15 个 content、1 个 add/add），18 个自动合并；没有删改冲突。
- 只有我们改过、上游没碰的 32 个：`dshns.c`、`SystemContainer.kt`、`SystemCheck.kt`、`RootShell.kt`、`QuitActivity.kt`、`LaunchDiagnostics.kt`、`system_container_section.dart`、`direct_engine_settings.dart`、`system_check_sheet.dart`、`runtime/locks/ubuntu-noble-arm64-system.json`、`runtime/container/tests/*`、交接文档和对应测试。

### 冲突文件（冲突块数 / 冲突区行数 / 我们的改动 / 上游的改动）

| 文件 | 块 | 行 | 我们 | 上游 | 性质 |
|---|---|---|---|---|---|
| `runtime/RuntimeController.kt` | 5 | 275 | +218/−3 | +388/−91 | 第 1 块：我们 161 行系统容器函数 vs 上游新加的 `setWorkspaceRecovery`，保留两边；第 4 块：重置前停容器 vs 上游的 `reapStrays()`，保留两边；**第 2、3、5 块**：引擎启动、启动失败、退出分类，上游重写成 `startupError`/`publishStartupFailure`/`classifyAfterTokenTimeout`（#75），我们的 `prepareSystemContainer`、`systemLaunchFailed`、`p.start(..., system.requested, ...)`、`SafePolicy.directLaunchFailed(f, source.system)` 要重新嵌进去 |
| `runtime/lib/rtbuild.py` | 6 | 290 | +352/−157 | +359/−74 | 两边都把写 tar 抽成函数：我们 `normalize_tree` + `write_tar(..., owners)`（系统镜像要保留属主），上游 `write_archive(entries, out, args, layered=)` + `partition_entries`、基础层缓存、`origins` 统计。要以上游的为准，把 `owners` 和系统镜像那条路（`cmd_system_image`）接上去 |
| `files/GuestFiles.kt` | 4 | 41 | +13/−4 | +19/−13 | 上游加了 l2s 绝对目标的重定位和 `phoneRoot` 参数；我们加了系统容器 rootfs（`hostOf(rootfs, system, …)`）。参数并起来 |
| `phone/PhoneBridge.kt` | 1 | 28 | +63/−7 | +44/−13 | 上游 `acceptTcp` 改成 `while (current(token))`，去掉了 `running`；我们的 `containerOnly` + `forgetBridge` 要按新生命周期改 |
| `runtime/DshProcess.kt` | 2 | 11 | +64/−8 | +54/−11 | `start()` 两边都加了参数；日志行上游加了 `redactor.line(line)`，我们加了 `SystemContainer.execPid(line)` |
| `runtime/CrashClassifier.kt` | 1 | 7 | +5/−1 | +25/−6 | 一个条件式：上游加 `PROOT_EXEC_FAILED`/`PROOT_ASSERT_FAILED`，我们加 `SYSTEM_FAILED` |
| `res/values{,-zh}/i18n_strings.xml` | 1+1 | 15+15 | +8/−0 | +36/−3 | 上游把 `service_stop_phone`/`service_stop` 改了文案；我们加了退出对话框的 6 个串和 `service_quit` |
| `test/.../files/GuestFilesTest.kt` | 1 | 178 | +27（新建） | +158（新建） | add/add：两边都新建了这个文件，合成一份 |
| `test/.../runtime/ProcessLaunchGuardTest.kt` | 1 | 14 | +4 | +45 | 末尾各加了测试 |
| `docs/guide/{zh,en}/11-faq.md` | 1+1 | 29+29 | +18 | +35/−4 | 末尾各加一节（我们「用系统容器跑引擎」、上游 headless 的 `MISSING_CREDENTIAL`），保留两边 |
| `app/assets/skills/dsh-go-guide/{zh,en}-11-faq.md` | | | | | 生成物，合完 `python3 app/tool/guide_sync.py` |
| `app/assets/skills/dsh-go-support/SKILL.md` | 1 | 5 | +4/−3 | +21/−7 | 上游改写了诊断内容那一句；我们在旧句子里加了"启动方式（proot/direct/system）"，挪进新句子 |
| `app/lib/ui/engine/engine_screen.dart` | 2 | 11 | +2 | +23/−6 | import 和 widget 各一处：我们 `SystemContainerSection`，上游 `WorkspaceRecoverySettings`，保留两边 |

### 自动合并、合完要核对的 18 个

- 生成物：`app/lib/l10n/app_{zh,en}.arb`、`app/lib/l10n/gen/*.dart`（上游动了几万行）。不要信自动合并的结果，合完跑 `python3 app/tool/l10n_merge.py`，再 `--check`。
- `parts/phone_{zh,en}.arb`（上游 +192/+206）：我们的「容器存储」等键要还在、中英同步。
- `GuestSetup.kt`（提示词，上游 +18/−11）和 `GuestAgentsModeTest`（上游 +51）：`SYSTEM_EXTRA` 的位置和措辞、断言。
- `AndroidManifest.xml`（上游 +50；我们加了 `QuitActivity`）、`DshService.kt`（上游 +45/−13；常驻通知的「退出」）、`RuntimePlugin.kt`、`PhoneControl.kt`（上游 +590/−120）、`PhoneGuest.kt`、`CrashClassifierTest`、`runtime_bridge.dart`、`diagnostics_page.dart`（上游 +109；我们的开关和检查表单）。
- `runtime/build-runtime.sh`（上游 +115/−48，接通了双层构建）：合并结果里 `PROOT_VERSION` 还是我们的 `5.1.107.102`（上游仍是 `.98`，那个版本已从 Termux 镜像下架）；`--system-only`、NDK、LXC 缓存那段要能跑。

### 上游的分层运行时和系统容器

上游 11-09 到 11-12 的几个提交（`70337878` preserve installed system across engine upgrades、`b49b185e` 安装器按层解压、`55919bae` rtbuild 按层分区、`1aad6b73` 构建脚本接通双层、`7a704a72` 删 `rootfs.old`）把运行时改成基础层 + 引擎层：

- 设备上的 proot rootfs 仍是 `files/runtime/rootfs` 一个目录，`/root` 保留；首次安装按层解压。
- 引擎升级由 `EngineUpgrade` 做：只替换 `ENGINE_PATHS`（`opt/dsh`、`opt/node`、`opt/dsh-go`、`etc/dsh-app`、`etc/profile.d/dsh-app.sh`、`usr/local/lib/node_modules/pnpm`、`usr/local/bin/{dsh,node,npm,npx,corepack,pnpm,pnpx,pn,pnx}`），旧的 rename 走、新的 rename 进来，之后删掉。
- 我们的 `SystemContainer.SHARED` 绑定 `root`、`opt/dsh`、`opt/node`、`opt/python`、`opt/dsh-go`、`opt/dsh-app`、`usr/local`、`etc/dsh-app`、`var/lib/dsh-app`（不检查存在与否；上游的 rootfs 里 `opt/python`、`opt/dsh-app` 都还在）。
- 推断：容器开着时升级引擎，容器里的 `/opt/dsh` 等还指着被换走的旧目录（删掉后是空的），直到容器重启；签名带 runtime 版本，下次启动引擎时会先重启容器，所以原理上兼容。`usr/local` 是整个目录绑进去的，里面的文件替换容器里立刻可见。**这条路要上机测**：容器开着 → 装一个 runtime 版本不同的 APK → 启动引擎。
- 上游重置前的 `reapStrays()`：要确认它不会因为看不到容器里的进程（ksu 域）而误判或卡住。

### 建议

- **merge，不 rebase**：把 `origin/main` 合进 `system-container`，一次解决冲突；rebase 要改写 fork 上已推送的 9 个提交并强推。
- 顺序：先解决三个保留两边的大类和生成物，再做 `PhoneBridge`/`GuestFiles`/`DshProcess`/`CrashClassifier`，最后 `RuntimeController` 和 `rtbuild.py`（先读上游这两处的完整新版本）。
- 合完的验证：Kotlin 全量、Dart 全量（本机有 43 个截图/外网测试本来就失败，要对照）、`flutter analyze`、`l10n_merge --check`、`guide_sync --check`、`node --test runtime/tests`；`runtime/build-runtime.sh arm64-v8a`（上游的分层构建第一次跑可能要下载新的钉死包）和 `--system-only`；打 APK；真机：系统容器全套（历史 §10、§12 的验收）、兼容启动、启动失败回退、引擎升级。
- 工作量：解决冲突大半天，加构建和真机回归，一个完整会话以上。上游每天几十个提交，开工前重新 fetch、重跑 `merge-tree`。
