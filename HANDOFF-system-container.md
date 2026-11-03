# 交接：dsh-Go 本地引擎换容器（自用改造）

> 2026-11-03 交接，同日第二、第三、第四个会话更新（第三个会话删掉了命名空间启动，加了 Docker/Podman 存储、备份恢复、卸载保护；第四个会话加了独立网络命名空间、Docker 默认网桥接管、宿主 loopback 中继和系统容器里的手机操控，见 §5b、§5c、§6b、§6c）。第五个会话审查并修正了第四个会话的改动，又按 App 里 agent 的自检结果改了提示词、Podman 默认网络、Docker 配置，修了容器重启的竞态（§5d、§5e、§6d、§6e）。前三个会话的改动是 `system-container` 上的 `09e4691`，第四、五个会话的改动是 `6e688aa`（已推送到 fork）；之后修的中继泄漏（§5e 末条）**未提交**，本仓库不接受外部 PR，这是用户自用的改造。
> 新会话先读本文，再读 `CLAUDE.md`。

## 1. 用户已经拍板的决定

- 自用分叉，不提 PR、不 commit/push，除非用户明确要求。
- **命名空间启动已删除**（第三个会话，用户决定）：那是 proot rootfs + `ns-launch.sh`、node 做 PID 1 的半成品，只用于测试。界面里不再有「命名空间」这种启动方式，只剩 系统容器 / 直接启动 / 兼容启动；检查页里「挂载命名空间」之类的技术项保留。
- **存储与备份（第三个会话拍板）**：Docker/Podman 存储是 32 GB 稀疏 ext4 镜像，放 App 私有目录（`files/system/storage.img`，不放 `/data/adb`），带日志，引擎页可「扩容 16 GB」；「删除系统容器」连存储一起删；「能备份的都备份」（容器系统 + 存储 + `/root`），只走 App 里的按钮；被别的 App 检测时，让用户先「退出 DSH Go（含容器）」，不用 `ksud kernel nuke-ext4-sysfs`（见 §4）；常见问题里写系统容器一节。
- **系统容器第一期：已实现，真机跑通**（见 §5、§6）：systemd 做 PID 1。需求包括数据库、Docker/Podman（"开发环境的一部分"），不需要图形界面。Docker/Podman 的存储已实现（§5b）；Podman 能跑；Docker 在 §5c 之后也能跑（默认网桥、外网、`-p` 已实测）。
  - **第二期（本次已实现，见 §5c）**：独立网络命名空间 + NAT；Docker 默认网桥、容器内 Docker、手机操控的 netns 可达性都靠它。容器默认走 App 当前的网络（VPN 开时跟 `tun0`），宿主 loopback 用 TCP 中继保持 App 的 127.0.0.1 来源；Docker 的 `daemon.json` 由 App 接管（引擎页开关，默认关，当前设备已开）。
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
- Git：这是用户自用的分叉，推到用户自己的 fork（公开）：远端 `fork` = https://github.com/gfbjngjibn/dsh-Go-namespace ，分支 `system-container`；`origin` 是上游，**绝不推送**。本仓库的 git 配置（只在本仓库）：`user.name gfbjngjibn`，`user.email 74477599+gfbjngjibn@users.noreply.github.com`（**不要用用户的真实邮箱**），`credential.helper` 是 Windows 的 Git Credential Manager。推送要在 Bash 沙箱外执行。只在用户要求时提交、推送；只暂存明确路径。第三个会话的全部工作是 `system-container` 上的一个提交 `09e4691`，第四、五个会话的是 `6e688aa`（2026-11-03 已推送）。
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
- **中继泄漏**（推送 `6e688aa` 后发现，修复未提交）：`exec --forward` 的中继是在包装进程切进容器 userns 之前 fork 的，以宿主 root 运行；包装进程之后是容器 root（宿主 uid 10346），`kill(relay, SIGTERM)` 被拒（EPERM），内核发父进程死亡信号（`PR_SET_PDEATHSIG`）也按包装进程的身份检查权限，同样发不出去。于是每停一次引擎就留下一个监听宿主 `127.0.0.1:<旧端口>`、转发进容器同一端口的中继：App 的端口一路涨（3080→3081→3082），而且以后容器里谁占了那个端口，手机上任何 App 都能连进去。修法：包装进程留一根管道的写端（CLOEXEC，不给容器命令），中继 `poll` 监听套接字和读端，包装进程无论怎么退出，读端一 HUP 中继就退出；去掉无效的 `kill` 和 PDEATHSIG。
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
- 装包时 `xz-utils` 的 `1ubuntu0.3` 在 ports.ubuntu.com 上 404（镜像站已是 `0.4`，索引没刷新），测试里临时钉了 noble 原版；与本项目无关。
- **中继泄漏**：旧版 dshns 关 stdin 后中继仍在监听（对照测试复现）；新版关 stdin、`kill -9` 包装进程两种方式下中继都退出、端口释放。装 11032109 后清掉旧版留下的 3 个孤儿中继（父进程 1，3080/3081/3082），从引擎页停止→启动：停止后宿主没有中继，再启动仍是 3080，容器没重启。
- 平板当前：APK 11032109（含中继修复）；新镜像的容器在跑，引擎在容器里（端口 3080），装着 docker.io 29.1.3 和 podman 4.9.3（各拉了一个 alpine）；「局域网访问容器」关，Docker 接管开；`/data/local/tmp` 已清理；Chrome 里留着一个 `10.213.0.2:18085` 的标签页。

## 7. 未做 / 未验证 / 已知问题

- **Docker 的接管是开关，默认打开**（第五个会话改的）：关着时 App 不写 `daemon.json`，也不恢复镜像里的旧配置（App 写过且没被改过的文件会删掉），默认网桥是否可用取决于当时文件里的内容——**关着时的 Docker 没测过**。开关只在下次启动引擎（容器重启）时生效。
- **Docker 的 `iptables` 仍是 legacy**：内核没有 nf_tables。镜像的 apt 钩子在每次装包后切到 legacy（见 §5e），dshns 开机时也切一次（标记 `/var/lib/dsh-go/iptables-legacy.done`，新镜像不带标记）。用 `dpkg -i` 绕过 apt 装 iptables 时，要到下次启动容器才切。
- **`/sys` 还是没重挂**（用户决定这一版不修），Docker 启动时会警告缺 `docker0/bridge/stp_state`、`brif/*/state`；不影响默认网桥。
- **手机操控的 TCP 回退绑定在 `10.213.0.1`**（只对容器网段开放，见 §5d Q2）：abstract unix socket 是 netns 作用域的，容器里的 relay 连不到；App 的 fallback 因此在 loopback 之外再监听桥网关，relay 依次尝试 abstract → 127.0.0.1 → 10.213.0.1。这个监听对容器/宿主可达，仍有 32 字节 token；`/proc/net/tcp` 的 uid 校验对跨 netns 连接拿不到 uid（返回 null 放行），这是设计接受的弱化，和原来 loopback 回退一样。
- **VPN 开关切换没实测**：策略规则在启动时按 App 当前的表（现在是 tun0）写入，monitor 每 2 秒重看；netd 清规则后会在 2 秒内补回。熄屏/切 VPN 的实际表现待测。
- **`dshns exec` 的 stdin 是停止线**：从 adb 里直接 heredoc 跑诊断会立刻 EOF 把命令杀掉（退出码 143/无输出）；要用 §2 的 fifo 撑住 stdin。App 的引擎 exec 有自己的管道，不受影响。
- **引擎仍只绑 127.0.0.1**：`dsh web` 硬拒绝 `0.0.0.0`（"unspecified (wildcard) address"），所以宿主用 TCP 中继（每连接 setns 进容器 netns）而不是 DNAT；`route_localnet` 保持 0。
- **Docker/Podman `-p` 的宿主访问已测**（§6e，含手机上的 Chrome）；`docker build`/`podman build` 在新镜像上没有重跑，建议再跑一次。
- 备份（App 先 stop 再 `dshns run`）在竞态修复后没有重跑；stop 现在会等 monitor 收尾，按理不再撞车。
- **手机操控只验证到 relay 连接**（tcp6 里 `10.213.0.1:43637` ← `10.213.0.2` 的 ESTABLISHED，relay 进程存活）；没有从 DSH 里真正调一次 `mcp__phone__*` 工具。
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

1. 在系统容器里真正调一次手机操控（`mcp__phone__screen` 等），确认 token/uid/回退路径。
2. 测 VPN 开关、熄屏/Doze、后台：容器和引擎是否跟着 App 的网络表，netd 清理规则后 2 秒内是否补回。
3. 重跑一次 `docker build`/`podman build`，以及一次备份 → 恢复。
4. 视需要支持容器内 `reboot`（monitor 看到 SIGHUP 时重新启动容器），以及容器状态的定期刷新。
5. 第二期的收尾：把网络命名空间/Docker 接管/手机 bridge 的验证记录和已知问题继续补全。
