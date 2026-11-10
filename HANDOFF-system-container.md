# 交接：dsh-Go 本地引擎换容器（自用改造）

> 2026-11-10 拆分。本文只写**现在的状况**：决定、环境、关键事实、实现概览、已知问题、下一步。
> 每一轮怎么做的、验证记录、设计取舍、审查材料都在 [handoff/system-container-history.md](handoff/system-container-history.md)，节号保持原来的 §1–§11，本文里写「历史 §n」就是指它。真机测试脚本在 [runtime/container/tests/](runtime/container/tests/README.md)。
> **新会话先读本文，再读 `CLAUDE.md`；要动某一块时再翻历史里对应的节。** 新一轮的实现和验证记录写进历史文件末尾，本文只更新状态。

## 0. 现在到哪了

- 分支 `system-container`，工作区干净，都已提交并推送到 fork：`09e4691`（第一期 + 存储、备份）、`6e688aa`（独立网络、Docker/Podman）、`1eaa590`（中继泄漏）、`c512310`、`66bb44e`（补内核后的一轮：必需内核项、pids、nft、IPv6）、`635f493`（交接）、`4017336`（2026-11-10 的一轮，见下一条），以及紧随其后的一个 docs 提交（交接拆分、`runtime/container/tests/` 的测试脚本）。
- **`4017336` 里有什么**：第七个会话的改动（tun/FUSE 设备节点、`/home` 放进容器存储、签名带 launcher、启动失败时的清理、`PhoneBridge` 重绑、「容器存储」文案；历史 §10 末尾和 §11），加上审查后的三处小改（提示词加了"拒绝 root 的程序用 `ubuntu` 跑"一条、monitor 忽略 SIGPIPE、`dshns run` 失败时也清理 cgroup；历史 §12）。平板上的 APK 11102027（launcher `d37e6799…`）就是这个提交的代码，已在真机验收。
- **审查结论（2026-11-10，第八个会话）**：主线没有走歪。审查发现的问题和处理结果记在 §5 开头。
- 平板当前：APK 11102027；容器是备份 Y 恢复的（`/home` 在存储上），装着 docker.io 和 podman（各有一个 alpine）；引擎在容器里（3080），手机操控 relay 连着，无障碍服务绑定着；Docker 接管开。`Download/DSH-Go/backups` 里有 11-07 14:46 的一份和 11-10 的两份（X 15:14 旧格式、Y 15:26 新格式），都含对话和 API 配置，要不要删由用户决定。

## 1. 用户已经拍板的决定

- 自用分叉，不提 PR；不 commit、不 push，除非用户明确要求。
- 启动方式只有 系统容器 / 直接启动 / 兼容启动。命名空间启动已删（历史 §3）。
- **系统容器**：systemd 做 PID 1，要能跑数据库和 Docker/Podman，不需要图形界面。用预制的 Ubuntu noble（LXC）镜像，打进 APK 但能单独更新；可以抄 Droidspaces（GPL-3.0，和本项目 AGPL-3.0 兼容），但不依赖它运行。现有 proot 那套保留。
- **proot 和系统容器共用 `/root`**（引擎数据、工作区都在这里）。
- **容器是守护进程**：App 被杀不影响它。「退出 DSH Go（含容器）」在引擎页和常驻通知里：确认 → 停引擎 → systemd 正常关机（约 60 秒超时强杀）→ 关 App。
- **dshns 有变化的 App 更新会重启一次容器**（2026-11-10 定）：容器签名里带 launcher 的 sha256。用户的原话："带上就带上吧，这个认下来，因为不认的话加了新特性最终还是要重启，真有不应该重启的时候，更新时间应该让位置"——不想被打断时，由用户自己挑更新的时机。只换 App、dshns 没变的更新仍然不重启。
- **只面向自己补过内核的设备**，不支持原厂内核；新补的内核选项是必需项，缺了不让启动。不做内存/CPU 限制（Android 把这些控制器放在 cgroup v1），不用 k8s，不补 `CGROUP_DEVICE`（历史 §9）。
- **存储与备份**：容器存储是 32 GB 稀疏 ext4 镜像（`files/system/storage.img`，不放 `/data/adb`），引擎页可「扩容 16 GB」；「删除系统容器」连存储一起删；备份是 容器系统 + 存储 + `/root`，只走 App 里的按钮。被别的 App 检测时让用户先「退出 DSH Go（含容器）」，不用 `ksud kernel nuke-ext4-sysfs`。
- **网络**：容器有自己的网络命名空间，经 NAT 上网，默认跟 App 当前的网络（VPN 开时跟 `tun0`）；宿主 loopback 用 TCP 中继；Docker 的 `daemon.json` 由 App 接管（默认开）；「局域网访问容器」默认关。
- **设备节点**：容器里的 `/dev/net/tun`、`/dev/fuse` 由 monitor 自建。不 chmod、chown 宿主的节点；不把宿主的 `/dev/loop*`、`/dev/block` 绑进容器。
- **`/home` 在容器存储里**，和 Docker 共用一块，不另挂；`/root` 不挪进存储。界面上叫「容器存储」。
- **这一版不做 / 不测**：`/sys` 不重挂；热点不测（平板不能插卡）；R3（VPN 锁定时的阻止规则）不测。
- **安全模型**：App 已在 SukiSU 授权 root；容器里的进程必须带 KernelSU 的 no-new-privs 标志（`su --ksu-no-new-privs`，dshns 自己也调 ioctl `_IO('K',21)`）。带标志后容器内执行 su 和 GRANT_ROOT 都会被拒；被拒时 su 仍会执行、退出码 0，只是以 App 身份运行，所以不能拿退出码判断（`dshns trial` 看 `id -u`）。
- 第一次推送带了真实邮箱的那个旧提交：用户决定不处理，**不要再提起**，也不要建议联系 GitHub Support 或删 fork；新提交只用 noreply 邮箱。

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
  `runtime/container/tests/cx.sh` 是这段的封装。
- Git：这是用户自用的分叉，推到用户自己的 fork（公开）：远端 `fork` = https://github.com/gfbjngjibn/dsh-Go-namespace ，分支 `system-container`；`origin` 是上游，**绝不推送**。本仓库的 git 配置（只在本仓库）：`user.name gfbjngjibn`，`user.email 74477599+gfbjngjibn@users.noreply.github.com`（**不要用用户的真实邮箱**），`credential.helper` 是 Windows 的 Git Credential Manager。推送要在 Bash 沙箱外执行。只在用户要求时提交、推送；只暂存明确路径。已推送的提交见 §0。
  - 第一次推送时带了用户的真实邮箱（提交 `30425e7`），已改成 noreply 并 force-push，但 GitHub 仍能按 SHA 查到这个旧提交。**用户决定不处理（2026-11-03："那个邮箱的事情不用管了"），不要再提起，也不要建议联系 GitHub Support 或删 fork**；新提交照样只用 noreply 邮箱。
- 界面自动化：`uiautomator dump` 能拿到 Flutter 的语义节点（文字和坐标），比截图可靠。脚本在 `runtime/container/tests/`（`ui.py`、`sw.py`、`stop-engine.sh`、`restart-engine.sh`）。**按钮一律按整段文字精确匹配（`ui.py tapx`）或按开关的 checkable 节点去点**：子串匹配会命中整个区块、点在区块中心（第七个会话就这样误关过「接管 Docker 配置」）。
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

## 3. 实测得到的关键事实（不要再踩）

第一、二个会话测到的：

- App（`untrusted_app_27`）**看不到 ksu 域进程的 /proc，也不能给它们发信号**（被 SELinux 拦截）。所以要停容器，只能关闭 su 进程的 stdin，或再通过 su 去杀。
- 必须用 **`su -W`**，否则 KernelSU 的 fd 包装会吞掉 stdin 的 EOF。
- toybox 的坑：`pkill -P` 不带进程名时什么都匹配不上，要用 `kill $(pgrep -P ..)`；`mount -o bind` 绑定一个文件时会当成镜像去建 loop 设备；`remount` 要用绝对路径；toybox 有 `pivot_root`。
- ksu 域创建的文件标签是 `app_data_file:s0`（没有 MLS 分类），App 仍然可以读、写、删。
- 不属于任何 App 的 uid（如 1100000000），亮屏时联网正常；后台没测。
- cgroup v2（`/sys/fs/cgroup`）**一个控制器都没开**，内存、CPU、cpuset 都在 `/dev/...` 下的 v1 层级。所以容器做不了资源限制。
- 预制镜像内容：有 systemd、dbus、udev、sudo、curl、cron、python3、apt、resolved；**没有 git、iptables**。有 26 个非 root 属主的条目、18 个 setuid/setgid 文件、2 个带 xattr 的条目（ping、journal）。

第二个会话（做系统容器时）实测到的：

- `/data` 的 f2fs 开了 **casefold**，overlayfs 直接拒绝（dmesg：`overlay: case-insensitive capable filesystem ... not supported`）。**Docker/Podman 的 overlay 存储不能放在 `/data` 下面**，所以有 ext4 的容器存储镜像（§4）。
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

之后几轮测到的（出处在括号里）：

- **新内核的版本串和旧内核完全相同**，分不出新旧；只能看 `/proc/config.gz` 和运行时状态（如 `cgroup.controllers` 里有没有 `pids`）。（历史 §9）
- **宿主的 `bridge-nf-call-*` 默认是 0，别的网络命名空间默认是 1**（内核补丁）。dsh-go 不要改宿主这三个值。（历史 §9）
- **根组的 `+pids` 只在容器运行期间开着**，是 dshns 加的才撤，记录带本次开机的 boot_id；Droidspaces 加了从不撤。（历史 §9）
- **Android 把接口的 IPv6 前缀路由放进按接口分的表**，所以网桥地址加 `noprefixroute`，路由在网桥 up 之后手动加进 main。（历史 §9）
- **Docker 默认把 FORWARD 策略改成 DROP**，宿主连 Podman 发布的端口会被丢，所以 `daemon.json` 里有 `ip-forward-no-drop`。（历史 §5e）
- **容器 root 杀不掉宿主 root 的进程，PDEATHSIG 也发不出去**：`exec --forward` 的中继靠管道的 HUP 退出。（历史 §5e）
- **`dshns exec` 的 stdin 是停止线**：从 adb 直接 heredoc 跑会立刻被结束（退出码 143），要用 fifo 撑住（§2、`cx.sh`）。
- **monitor 放锁、`stop` 返回的时机**：monitor 先拿锁再回收 init，收尾完才退出放锁；`stop` 等这把锁。（历史 §5e）
- **镜像不能带 apt 索引**：App 解压不保留 mtime，旧索引会让 `apt-get update` 拿到 304，装包 404。（历史 §9 末尾）
- **容器自己的用户命名空间里挂的 tmpfs 是 nodev，`/data` 也带 nodev**：设备节点只能建在 monitor（宿主真 root）挂的 tmpfs 里再绑进容器。宿主的 `/dev/fuse` 是 `root 0600`、`/dev/tun` 是 `system:vpn 0660`，容器里映射不到。（历史 §10）
- **非 App 的 uid 访问 `/sdcard` 报 "Bad address"**：Android 的存储只认 App 自己的 uid，只有容器 root 映射到它。（历史 §10「还没定的方向」）
- **runc 在用户命名空间里，`--device` 只支持同名映射**，和 `CGROUP_DEVICE` 无关。（历史 §7）
- **`pm install -r` 之后 ColorOS 可能不重新绑定无障碍服务**：重装 APK 后测手机操控前先看 `dumpsys accessibility`。（历史 §7）
- **toybox `mount -o loop` 路径超过 64 字节会失败并留下没有 autoclear 的 loop**。（历史 §10）

## 4. 现在的实现（概览）

细节和每一处的理由在历史 §5–§5e、§9、§10、§11。

- **启动器 `runtime/container/dshns.c`**（约 2300 行，以宿主 root 经 `su -W --ksu-no-new-privs -c` 调用）：子命令 `start | exec | stop | status | run | grow | trial`。
  - `start`：拿锁 → fork 出 monitor（setsid）→ 建 cgroup、开 pids 并封顶（`pid_max / 2`，最多 65536）→ 私有挂载命名空间、rootfs 去 nosuid、在 `state/dev` 的 tmpfs 里建 `fuse`、`net/tun` 节点 → 挂存储（`e2fsck -p`、loop、挂到 `var/lib/dsh-go/storage`，把 `docker`、`containerd`、`containers` 绑到 `var/lib/` 下）→ clone 出容器 1 号进程（user、mount、pid、uts、ipc、net 命名空间）→ 写 uid 映射、宿主侧网络。
  - 容器 1 号进程：挂 proc、sys、cgroup2、`/dev`、`/run`，绑定共享目录 → pivot_root → 首次启动恢复镜像的属主和 setuid → seccomp（只拦 KernelSU 的魔法 reboot）→ `/home` 绑到存储（首次搬迁）→ 配 eth0 → exec systemd。
  - monitor：读控制台到 `console.log`；每 2 秒对账网络规则、看 4 个哨兵（App 数据被删就 `cgroup.kill`）；init 退出后先拿锁再回收，清网络、cgroup、pids，卸载，退出时放锁。
  - `run`：同样的命名空间和存储，但前台跑一条命令代替 systemd（备份、恢复），没有网络。
  - `exec --forward PORT`：在宿主 netns 起 TCP 中继（127.0.0.1:PORT → 容器的 127.0.0.1:PORT）。
- **uid 映射**：容器 root = 宿主上的 App uid（10346）；容器里的其他用户 = 宿主 `2100000000` 起的一段（容器 uid N → `2100000000 + N - 1`，ubuntu 是 `2100000999`），不属于任何 App。
- **网络**：宿主网桥 `dsh-br0` `10.213.0.1/16`、`fd10:213::1/64`；容器 `10.213.0.2`、`fd10:213::2`；MASQUERADE、FORWARD、TCPMSS，`ip rule` 6060/6070 跟 App 的路由表；v4、v6 各做一遍。
- **`/home` 的四种状态**（`home_on_storage()`；`HOME_MOVED` 是 rootfs 里的 `/var/lib/dsh-go/home-on-storage`）：

  | 存储里有 `home` | rootfs 里有标记 | 处理 |
  |---|---|---|
  | 没有 | 没有 | 首次搬迁：`cp -a` 到 `home.partial` → `syncfs` → rename → `syncfs` → 删原件 → 写标记 → bind |
  | 没有 | 有 | 启动失败（存储丢了 `/home`：恢复备份，或删除系统容器） |
  | 有 | 没有 | 补写标记 → bind |
  | 有 | 有 | bind |

  复制失败也是启动失败，原件不动，下次重来。
- **镜像**：`runtime/lib/rtbuild.py system-image`，以 LXC noble rootfs 为基础，不带 apt 索引；`runtime/build-runtime.sh --system-only arm64-v8a` 约 1 分钟。当前镜像 `s1-ubuntu-noble-20261018_1335-2b194891b81e`。已解压的旧镜像不会被新镜像替换。
- **App（Kotlin）**：`runtime/SystemContainer.kt`（安装、启动、签名、存储、备份恢复、Docker 配置）、`SystemCheck`（检查页 25 项，其中 10 个必需内核选项）、`RootShell`、`QuitActivity`、`RuntimeController`（偏好、失败回退）、`DshProcess`、`GuestSetup`（给 agent 的环境说明）、`phone/PhoneBridge.kt`（在 `10.213.0.1` 上只对容器网段监听）。
- **App（Dart）**：`lib/ui/engine/system_container_section.dart`（引擎页的系统容器区块）、诊断页的开关和检查表单；文案在 `lib/l10n/parts/phone_{zh,en}.arb`。
- **容器签名**（不同就先停再起）= runtime 版本 + launcher 的 sha256（取自 `system.json`）+ 启动参数（不含启动器路径）+ 接管 Docker 时的整份 `daemon.json`。
- **启动失败**：App 记下失败时的 runtime 版本，之后改用直接或兼容启动，直到用户重新打开「系统容器启动」开关。

## 5. 已知问题 / 没验证的

### 2026-11-10 审查发现的

- **monitor 没有忽略 SIGPIPE**——**已改，真机验过**（`sigpipe.sh`：旧版调用方一死 monitor 就没了，之后停容器要等超时强杀，网桥、规则、cgroup、根组 `+pids`、`ip_forward=1` 全留在宿主上；新版 monitor 还在，停止后宿主干净）。原来 `signal(SIGPIPE, SIG_IGN)` 只在 `cmd_exec` 里：`dshns start` 的父进程如果在 monitor 回 `ok` 之前没了（比如在 adb 里手动跑时 Ctrl-C），monitor 写 ready 管道时会被杀，留下一个没有 monitor 的容器（没有网络对账、没有卸载保护、停止时不收尾）。现在 monitor 在回 `ok` 之前忽略 SIGPIPE；放在 `spawn()` 之后，所以容器的 1 号进程不会继承这个设置。
- **`dshns run` 失败不清理 cgroup**——**已改，真机验过**。原来 `cmd_run` 里 `storage_attach()`、`spawn()`（第七个会话起还包括 `home_on_storage()`）失败会直接 `die`，留下 `dsh-go/<名字>`，没有别的容器在跑时还留下 `dsh-go` 和根组的 `+pids`。现在 `die()` 会先调 `run` 登记的 `run_release()`（和正常结束走同一个函数）。真机上用 `runfail.sh` 对比过，用户的容器跑着、停着各一遍：旧版失败后留下 `dsh-go/dshtest`，容器停着时还留下 `dsh-go`、根组的 `+pids` 和 `pids.root`；新版两种失败后什么都不留。
- **首次搬迁撞上 60 秒启动超时**——**不改**。历史 §11.6 第 1 条说 App 会 destroy 掉 `dshns start` 的父进程，但 App 给 ksu 域的进程发不了信号（§3 第一条），多半杀不掉。更可能的后果是：App 记一次失败、回退到兼容启动，容器稍后在后台照常起来，下次打开开关时因为签名文件没写而再重启一次。这条路现在走不到（见下一条的理由）。
- **两个边角情况**——**评估后不改**。首次搬迁时 rootfs 里没有 `/home`，会一直启动失败（`cp` 找不到源）；`/home` 是相对路径的软链接时，搬完后软链接在存储里指向不存在的地方，之后一直报"存储丢了 `/home`"。不改的理由：首次搬迁今后只会从干净的镜像开始（新装，或恢复备份时先解镜像再搬，镜像里 `/home` 是普通目录、只有 skel 的三个文件）；唯一带着旧布局的容器（用户平板上那个）已经搬完。处理软链接要给状态表加第五种状态，还得上机测，不值得。真遇到了，monitor.log 里有 `cp` 的原话，删掉系统容器重来即可。
- **`PhoneBridge` 那个 "Socket closed" 的原因**（审查时凭对 AOSP 的记忆，没验证）：多半是 netd 在接口地址被删时销毁绑在该地址上的 socket，监听 socket 变成关闭状态，Android 把 `accept` 的 EINVAL 报成 "Socket closed"。如果是这样，旧 socket 确实不能再用，"清掉重绑"是正解。
- **`/home` 放进存储的收益原来没人用到**：agent 以容器 root 在 `/root`（f2fs）工作，`/home/ubuntu` 默认没人用。现在提示词里让 agent 在程序拒绝 root 时切到 ubuntu，`/home` 才有了使用者。见 §6「agent 以非 root 用户工作」。

### `/home`、设备节点、签名（第七个会话）

- **App 从宿主侧读不到 `/home` 里的文件**：`GuestFiles` 按系统容器的 rootfs 找文件，`/home` 现在只挂在 monitor 和容器的命名空间里。`/var/lib/docker` 等一直如此。
- **`/home` 里有复制不了的文件时，首次搬迁一直失败**（设备节点；属主映射不到、其他人又不可读的文件）：monitor.log 里有 `cp` 的原话，要从宿主处理掉那个文件，或者删容器。属主映射不到但可读的文件，复制后属主变成 nobody。
- **没测的**：`trusted.*` 的 xattr（应该是静默丢掉）；断电和崩溃窗口（只做了分析，历史 §11.4）；`make_nodes()` 的失败路径；这一版上的在线扩容；rclone mount。
- **`make_nodes()` 会跟随 `state/dev` 上的软链接**：`state` 属于 App，而 App 本来就有 root，不构成新的越权；容器看不到 `state`。
- **`systemLaunchFailed` 记的是 runtime 版本，不含 launcher**：某个 dshns 版本启动失败后，换一个修好的 dshns（runtime 没变）不会自动清掉失败记录，要手动重新打开开关。
- **`system.json` 的 `launcher.sha256` 和 APK 里 `libdshns.so` 的字节不同**（gradle 打包时又 strip 了一次）：签名用的是前者，不影响用途，只是不能拿它核对设备上的文件。
- **启动失败后 `state/storage.dev` 会留下**，内容过时但没人用它判断状态。
- **引擎页「容器存储」的用量不实时刷新**，要等下一次状态事件。
- **第七个会话之后没有在新对话里真调过一次 `mcp__phone__*`**（只看了无障碍绑定和 relay 的 TCP 连接）。

### Docker / Podman

- **接管关着时的 Docker 没测过**：App 不写 `daemon.json`，默认网桥是否可用取决于文件当时的内容。开关只在下次启动引擎时生效。
- **Podman 容器的 1 号进程不理 SIGTERM 时，停容器会超时被强杀**：systemd 最多等 90 秒，超过 dshns 的 60 秒。收尾照常，只是慢。
- **`--device` 只支持同名映射**（§3）；绕法是同名 `--device` 或 `-v /dev/a:/dev/b`。
- **Podman 默认网络没有 IPv6**：`-p` 的端口从宿主经 `[fd10:213::2]` 连不上，经 `10.213.0.2` 可以；Docker 两个都行。
- `podman build` 会有几行 `can't raise ambient capability` 警告，不影响构建。
- **`/sys` 没重挂**：Docker 启动时警告缺 `docker0/bridge/stp_state` 等，不影响默认网桥；容器里看得到宿主全部网卡和 wlan0 的 MAC。

### 网络、后台

- **VPN 开关切换、熄屏、Doze、后台、低内存查杀都没实测**；v1 的 memory/cpu/cpuset 还挂在启动它的 App 所在组。
- **手机操控的 TCP 回退绑在 `10.213.0.1`**：只对容器网段开放，有 32 字节 token；跨 netns 的连接拿不到 uid，校验放行，这是接受的弱化。
- **引擎只绑 127.0.0.1**（`dsh web` 拒绝 `0.0.0.0`），所以用中继而不是 DNAT。
- **nf_tables 的安全代价**：容器 root 能直接碰到，内核源码停在 6.12.58，要跟安全补丁。

### 其他

- 没在真机上触发过「系统容器启动失败 → 自动回退」。
- 容器里执行 `reboot`：monitor 当作退出，**不会自动重启**。容器自己停掉时，引擎页的状态要等下一次状态事件才刷新。
- 存储授权变化、runtime 升级、dshns 变化时，下次启动引擎会自动重启容器，日志里有一行说明。
- `dsh-exec` cgroup 在 systemd 的 unit 体系之外。系统容器模式下日志里会出现 "starting proot with incomplete process observation"（原有措辞，不准确）。
- ping 的 file capability 没保留。x86_64 没钉镜像，不显示系统容器功能。
- LXC 镜像只剩 `runtime/.cache/lxc/` 里这一份，换 serial 时要同时更新 PIN。
- App 的 `files/` 下还有一个 `ns-launch.sh`（命名空间启动留下的），与系统容器无关。
- `node --test runtime/tests/*.test.mjs` 在本机有 10 个失败，原因是没设 `DSH_PREFIX`/`DSH_TEST_PREFIX`（要打好补丁的 DSH 安装）；这几轮都没改 bridge/patches，没处理。
- 第一份交接里未验证的项（su 强杀兜底、`WRITE_SECURE_SETTINGS`）仍未验证。

## 6. 下一步和还没定的方向

1. **新对话里让 agent 真用一次 `su - ubuntu -c`**（比如装 PostgreSQL 跑 `initdb`），看提示词那一条够不够用；顺便真调一次 `mcp__phone__*`。
2. **测 VPN 开关、熄屏/Doze、后台**：容器和引擎是否跟着 App 的路由表，netd 清规则后 2 秒内是否补回。
3. 视需要支持容器内 `reboot`（monitor 重新启动容器）和容器状态的定期刷新。
4. **App 里 agent 提过、核实后可以做、用户还没定的**：重挂 `/sys`、时区跟随手机（容器是 UTC）、日志上限（`daemon.json` 的 `log-opts`、journald 的 `SystemMaxUse`）、`/sdcard` 只读档。它提的"宿主写 cgroup 就能限内存""把宿主 `/dev/loop*` 绑进容器""`lazy_itable_init=0`"是错的或不该做。
5. **给容器整体设内存上限**（没定）：`docker --memory`、`--cpus` 做不到（控制器在 v1）；可行的是 dshns 在 v1 memcg 里给整个容器设一个总上限，或者容器里装 earlyoom。
6. **agent 以非 root 用户工作**（2026-11-10 和用户讨论过，用户原以为已经是非 root；**定下的做法是引擎保持 root，提示词里告诉 agent 某个程序拒绝 root 时用 `su - ubuntu -c '命令'`**，已加进 `GuestSetup.SYSTEM_EXTRA`。把引擎整体换成非 root 没有做，也没有定要做）。现状：引擎由 `dshns exec` 以容器 root 启动，`HOME=/root`，工作区 `/root/workspace`。同日在平板上以 ubuntu 实测：`/root` 是 `root:root 700`，进不去；`/sdcard` 报 "Bad address"；`su - ubuntu` 自带 `XDG_RUNTIME_DIR=/run/user/1000`；sudo 免密。整体换成 ubuntu 会碰到：
   - **根源是 uid 映射**（§4）：App 和容器共享的东西都是按 App uid 做的，只有容器 root 拿得到。
   - **`/root`**：是 proot rootfs 的目录绑进来的，属主是 App uid。chown 给 ubuntu，兼容启动和 App 就读不了；搬去 `/home/ubuntu`，对话和工作区进了 ext4 镜像，兼容启动看不到，「启动失败自动回退」之后是空的。和「共用 `/root`」的决定冲突。
   - **手机文件**：ubuntu 访问 `/sdcard` 报 "Bad address"（§3）。
   - **App 打开、预览 agent 写的文件**：`GuestFiles` 以 App 身份从宿主侧读，ubuntu 的文件读不了，`/home` 里的根本看不到。
   - **手机操控**：`/var/lib/dsh-app/phone/bridge.json`（token）是 0600、属容器 root。
   - **不是安全边界**：ubuntu 有免密 sudo（镜像自带 `/etc/sudoers.d/90-incus`），用 Docker 还得进 docker 组。去掉 sudo 才算边界，但那样装不了包。真正的边界一直是用户命名空间加 KernelSU 的 no-new-privs。
   - **会话环境**：`dshns exec` 不经过 PAM，没有 `XDG_RUNTIME_DIR` 和用户级 systemd（`su - ubuntu` 有）。
   - **后台联网没测**（推断）：容器里非 root 进程在宿主上是不属于任何 App 的 uid，只测过亮屏时能联网。
   - 所以现在的做法是：引擎保持 root，ubuntu 只是"某个工具拒绝 root 时用的普通用户"；`/home` 上存储和 rootless Podman 正好是它的落脚点。
7. **让容器里的 agent 开发调试 Android App**（没定）：adb 和 su 分不开（都是 shell uid 2000）；容器连 adbd 要让它在网络上监听；更合适的是在手机操控通道上加白名单式的开发工具，不要放进 dshns；arm64 上要另找 `aapt2`。细节在历史 §10「还没定的方向」。
