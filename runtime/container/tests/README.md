# 系统容器的真机测试脚本

系统容器（`../dshns.c`）没有能在桌面上跑的自动化测试，回归靠平板。这些是第七个会话（2026-11-10）实际用过的脚本，从交接文档里原样挪过来，**不进构建，也不被 `node --test runtime/tests` 或 `build-runtime.sh` 引用**。

写死的东西：`~/android-sdk/platform-tools/adb`、包名 `io.github.ccch1mneyyy.dsh_app`、App uid `10346`、容器名 `dsh`。设备操作的约定（`adb shell su <<'EOF'`、不要用 `adb push`）见仓库根的 `HANDOFF-system-container.md` §2。

| 脚本 | 在哪跑 | 做什么 |
|---|---|---|
| `cx.sh SCRIPT [TIMEOUT]` | 本机 | 把本地脚本放进用户的系统容器，以容器 root 跑。用 fifo 撑住 `dshns exec` 的 stdin（stdin 一关命令就被结束）。脚本跑完，它起的后台进程也一起没了。 |
| `ui.py dump \| tap \| tapx \| has` | 本机 | `uiautomator dump` 后按文字找节点、点击。**用 `tapx`（整段文字精确匹配）**：`tap` 的子串匹配会命中整个区块、点在区块中心。**agent 在用手机操控时不要跑它**（见「注意」）。 |
| `trace.sh [N [CUT]]`（带 `trace.py`） | 本机 | 从会话记录读出引擎里最新（N=2 是上一个）对话里 agent 做了什么：用户消息、工具调用和结果（截到 CUT 字）、回复、每一轮怎么结束的（`turn ... ended`）。不碰屏幕，agent 跑着的时候也能用。 |
| `sw.py LABEL [on\|off]` | 本机 | 读或切换一个开关（按 checkable 节点）。 |
| `stop-engine.sh`、`restart-engine.sh` | 本机 | 从引擎页停止、重启引擎（界面是中文时）。 |
| `prep.sh` | 容器（经 `cx.sh`） | 装测试工具，在 `/home/ubuntu/mig` 放一组测试文件（setuid、xattr、ACL、硬链接、稀疏文件等），记下 `/root/mig-before*.txt`。 |
| `check1.sh`、`check2.sh` | 容器（经 `cx.sh`） | 和 `prep.sh` 的记录比对属主、权限、xattr、ACL；看 `/dev/fuse`、`/dev/net/tun` 能不能打开；测存储上的 setuid。 |
| `speed.sh` | 容器（经 `cx.sh`） | rootfs → `/home` 的复制速度（会写 2 GiB 再删）。 |
| `systest.sh` | 设备（root） | `/home` 搬迁的失败路径 T1–T7，用单独的测试容器 `dshtest`（`dshns run`），不碰用户的容器。 |
| `runfail.sh [DSHNS]` | 设备（root） | `dshns run` 在挂存储、建容器根这两处失败后，不该留下自己的 cgroup。用空的 rootfs，不用解镜像；可以传一个还没装进 APK 的 dshns。用户的容器在跑时只能看到 `dsh-go/dshtest` 这一层；用户的容器停着时还能看到 `dsh-go`、根组 `+pids` 和 `pids.root` 记录是否收回（旧版会留下根组的 `+pids`，而记录随测试目录删掉了，要手动 `echo -pids > /sys/fs/cgroup/cgroup.subtree_control`）。 |
| `sigpipe.sh [DSHNS]` | 设备（root） | `dshns start` 的调用方在 monitor 回答之前被杀掉时，容器仍然要有 monitor。会真的启动、停止一个测试容器 `dshtest`（要先按下面的办法解好镜像）；网桥和规则的名字是全局的，**用户的容器必须先停**。 |

## `systest.sh`、`sigpipe.sh` 的前提

先把 APK 里的镜像以 App uid 解到 `files/systest/rootfs`（设备上没有 `xzcat`，在本机解压后传；`su <uid> -c 'a && b'` 只有第一条以那个身份执行，所以包成脚本）：

```sh
A=/data/user/0/io.github.ccch1mneyyy.dsh_app/files
adb shell su <<EOF
mkdir -p $A/systest/rootfs $A/systest/state; chown -R 10346:10346 $A/systest; chmod 700 $A/systest
EOF
printf 'cd %s/systest/rootfs || exit 9\ntar -x\n' $A | adb exec-in "su 10346 -c 'cat > $A/systest/x.sh'"
unzip -p app-arm64-release.apk assets/runtime/system-arm64-v8a.tar.xz | xz -dc | adb exec-in "su 10346 -c 'sh $A/systest/x.sh'"
adb shell su -c "chmod 755 $A/systest/rootfs"   # App 解完镜像也会这样做
```

然后：

```sh
adb exec-in "su -c 'cat > /data/local/tmp/systest.sh'" < systest.sh
adb shell su <<'EOF'
sh /data/local/tmp/systest.sh
EOF
```

测完 `rm -rf $A/systest` 和 `/data/local/tmp/systest.sh`。各步的预期结果见 `handoff/system-container-history.md` §10「本轮实现与验证」第 5 条。

## 注意

- `prep.sh` 会 `apt-get install` 一批包（acl、attr、sshfs、squashfuse、squashfs-tools、openssh-server、libfuse2t64），测完按 apt 历史 purge。
- 不要用 toybox 的 `mount -o loop` 去看存储镜像：路径超过 64 字节时挂载失败，却会留下没有 autoclear 的 loop 设备（要 `losetup -d`）。
- **`uiautomator dump`（`ui.py` 的每个子命令都会先 dump）会把 App 的无障碍服务断开约 1 秒**：它注册的 UiAutomation 不带 `FLAG_DONT_SUPPRESS_ACCESSIBILITY_SERVICES`（logcat：`Registering UiTestAutomationService ... flags=0x0`，紧跟着 `DshA11y: accessibility service gone`）。App 把这当成停止请求，**正在用手机操控的那一轮会被中止**（会话记录里 `turn/end` 的 reason 是 `aborted`/`user`）。发完消息后不要用 `ui.py` 轮询，用 `trace.sh` 看进度，这一轮结束后再 dump。agent 不用手机操控时 dump 不影响它。
- 拆分后重新上机跑过的有 `cx.sh`、`ui.py dump|tapx|has`、`runfail.sh`、`sigpipe.sh`、`trace.sh`（2026-11-10，APK 11102027）；其余只做过语法检查（`bash -n`、`py_compile`）。
- 给 App 里的 agent 发消息：`ui.py tapx 新对话` → 点输入框（横屏时约在 `2100 2284`）→ `adb shell "input text '...'"`（空格写成 `%s`，只能是 ASCII；搜狗输入法不会把它当拼音）→ `ui.py has` 核对全文 → `ui.py tapx 发送`。中文发不了（设备上没有 `cmd clipboard`，也没装 ADBKeyBoard），用英文写、末尾加 "Please reply in Chinese."。
- adb 传上去的可执行文件要先 `cp` 一份再执行，否则可能报 "Text file busy"。cgroup 文件的大小永远是 0，判断有没有内容要读出来，不能用 `[ -s ]`。
