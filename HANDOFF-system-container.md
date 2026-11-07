# 交接：dsh-Go 本地引擎换容器（自用改造）

> 2026-11-03 交接，同日第二、第三、第四个会话更新（第三个会话删掉了命名空间启动，加了 Docker/Podman 存储、备份恢复、卸载保护；第四个会话加了独立网络命名空间、Docker 默认网桥接管、宿主 loopback 中继和系统容器里的手机操控，见 §5b、§5c、§6b、§6c）。第五个会话审查并修正了第四个会话的改动，又按 App 里 agent 的自检结果改了提示词、Podman 默认网络、Docker 配置，修了容器重启的竞态（§5d、§5e、§6d、§6e）。前三个会话的改动是 `system-container` 上的 `09e4691`，第四、五个会话的改动是 `6e688aa`，之后修的中继泄漏（§5e）是 `1eaa590`，都已推送到 fork，本仓库不接受外部 PR，这是用户自用的改造。2026-11-05 定下补内核的方案（§9），那一轮没改代码；新内核 2026-11-07 刷进平板后，第六个会话按 §9 改完代码并在新内核上回归（见 §9 "本轮实现与验证"），2026-11-07 已提交并推送到 fork（`system-container` 分支上 `c512310` 之后的那个提交）。2026-11-07 新内核已刷进平板并通过真机检查，里面实际有哪些选项见 §9「新内核实际有什么」（没有 `CGROUP_DEVICE`、没有 ipvlan L3S 模式），可以开始做 §9 的下一轮代码改动。
> 新会话先读本文，再读 `CLAUDE.md`。

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
- Git：这是用户自用的分叉，推到用户自己的 fork（公开）：远端 `fork` = https://github.com/gfbjngjibn/dsh-Go-namespace ，分支 `system-container`；`origin` 是上游，**绝不推送**。本仓库的 git 配置（只在本仓库）：`user.name gfbjngjibn`，`user.email 74477599+gfbjngjibn@users.noreply.github.com`（**不要用用户的真实邮箱**），`credential.helper` 是 Windows 的 Git Credential Manager。推送要在 Bash 沙箱外执行。只在用户要求时提交、推送；只暂存明确路径。第三个会话的全部工作是 `system-container` 上的一个提交 `09e4691`，第四、五个会话的是 `6e688aa` 和 `1eaa590`（中继泄漏），2026-11-03 都已推送。
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
- **`podman build` 的 RUN 步骤失败**（2026-11-07 发现，第五个会话去掉 `netns = "host"` 时引入）：容器在用户命名空间里，buildah 把自己当成 rootless，RUN 默认用 slirp4netns，而容器里没有 `/dev/net/tun`（`open("/dev/net/tun"): Permission denied`）；`--network bridge` 被拒（"cannot use networks as rootless"）。`--network host`、`--isolation chroot` 都能成功。`podman run` 不受影响，`docker build` 正常。**用户定的修法（2026-11-07）：不改环境，在提示词（`GuestSetup` 的 `SYSTEM_EXTRA`）和常见问题里写明 `podman build` 要加 `--network host`。**
- **Podman 容器的 1 号进程不理 SIGTERM 时，停容器会超时被强杀**：systemd 停 `libpod-*.scope` 最多等 90 秒（DefaultTimeoutStopSec），超过 dshns 的 60 秒，于是走 `cgroup.kill`（monitor 日志 "killed by signal 9"）。收尾照常、存储下次 `e2fsck` 是 clean，只是慢。2026-11-07 回归时由测试留下的 `podman run -d alpine sleep 300` 触发。
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

## 8. 建议的下一步

0. §9 的代码改动已做完并在真机回归（2026-11-07，已提交）；`podman build` 按用户定的写进提示词和常见问题；热点不测（平板不能插卡，用户决定不管）（见 §9 "本轮实现与验证"）。
1. 在系统容器里真正调一次手机操控（`mcp__phone__screen` 等），确认 token/uid/回退路径。
2. 测 VPN 开关、熄屏/Doze、后台：容器和引擎是否跟着 App 的网络表，netd 清理规则后 2 秒内是否补回。
3. ~~重跑 `docker build`/`podman build`、备份 → 恢复~~：2026-11-07 已跑，`docker build` 和备份恢复正常，`podman build` 失败（§7）。
4. 视需要支持容器内 `reboot`（monitor 看到 SIGHUP 时重新启动容器），以及容器状态的定期刷新。
5. 第二期的收尾：把网络命名空间/Docker 接管/手机 bridge 的验证记录和已知问题继续补全。
6. App 里 agent 在自检会话第 5–12 轮提的、核实后可以做的几项（用户还没定）：重挂 `/sys`（现在容器里看得到宿主全部网卡和 wlan0 的 MAC）、时区跟随手机（容器是 UTC）、日志上限（`daemon.json` 的 `log-opts`、journald 的 `SystemMaxUse`）、`/sdcard` 只读档。它提的"宿主写 cgroup 就能限内存""把宿主 `/dev/loop*` 绑进容器""`lazy_itable_init=0`"核实下来是错的或不该做。

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
