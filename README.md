# hdcfwd — hdc 局域网/云端透明端口转发器

让局域网或云端的其他机器**像使用本机设备一样**直接使用 OpenHarmony `hdc`：
不需要 `-s ip:port`、不需要改任何使用习惯，`hdc shell` / `hdc list targets` /
`hdc file send` / DevEco Studio 等一切基于 hdc 的工具**全部透明可用**，且与
设备直连机器上的本地使用**互不冲突**。云端（无公网入站、只能出站 SSH 的
远程 Ubuntu）场景配合 SSH 端口转发使用，见[场景二](#场景二跨公网云端-ubuntu配套-ssh-端口转发)。

```
┌─────────────────────┐         LAN (TCP 9710)        ┌──────────────────────┐
│  设备直连机器 (win)   │ ◄──────────────────────────► │  局域网其他机器 (linux) │
│                     │                               │                      │
│  hdc server ──8710──┤◄─┐                       ┌──►├──8710── 本机 hdc 客户端 │
│  (只监听 127.0.0.1)  │  │                       │   │  (hdcfwd client 冒充   │
│         ▲           │  └── hdcfwd server ──────┘   │   本地 hdc server)     │
│         └── USB 设备 │      监听 0.0.0.0:9710        │                      │
└─────────────────────┘      自动保活/拉起 server      └──────────────────────┘
```

现状痛点：hdc server 只监听 `127.0.0.1:8710`，局域网共享设备目前只能
`hdc kill && hdc -s 0.0.0.0:8710 -m` + 每条命令带 `-s ip:8710`，侵入性大。
本工具一次性解决。

---

## 一、设计思路（源自 adbportforward 的移植与增强）

架构思想来自 Android 的 `adbportforward`（chabernac/portforward）项目：

**adbportforward 的做法**
- adb server 只监听 `127.0.0.1:5037`，局域网无法访问；
- **server 模式**（设备机）：在 `0.0.0.0:6037` 上监听新端口，转发到本机
  `127.0.0.1:5037`，并周期性执行 `adb start-server` 保活；
- **client 模式**（远端机）：先 `adb kill-server` 腾出端口，然后接管
  `127.0.0.1:5037`，转发到设备机 `6037`；
- 效果：远端机上 `adb devices` 无需任何参数，体验与本机完全一致。

**移植到 hdc 的三个关键适配**

1. **协议完全透明**：hdc 客户端-服务端协议是纯 TCP 字节流（握手 banner
   `OHOS HDC` + channelId + 可选版本串，之后是会话多路复用流量），协议中
   没有任何依赖 IP 地址的字段，因此**哑的 TCP 双向字节泵即可完整转发**，
   无需解析协议、无需修改 hdc。

2. **抑制客户端的“自动拉起服务端”**（adb 没有的坑）：hdc 客户端在连接前会
   先通过 `Base::ProgramMutex()` 判断本机是否已有 hdc server（Windows 查全局
   命名互斥体 `Global\HDCServer`；Linux 对 `<tmpdir>/.HDCServer.pid` 做
   `fcntl F_SETLK` 写锁），如果没有就 fork 一个注定失败的本地 server
   （端口已被转发器占用），每条命令白白浪费约 1.3 秒。
   **hdcfwd client 模式持有与 hdc server 完全相同的互斥体/文件锁**
   （Windows 用相同的 permissive DACL 创建 `Global\HDCServer`；Linux 用
   `fcntl` 写锁并保持 fd 不关——注意不能用 `flock`，它与 `fcntl` 在
   Linux 上是两个独立的锁空间，互相不可见），让本机所有 hdc 客户端都认为
   “server 已在运行”，从而直接连上转发器。

3. **让 `hdc kill` 不会误杀转发器**：`hdc kill` 的实现是读取
   `.HDCServer.pid` 里的 pid 然后 SIGKILL。client 模式接管控锁后把 pid
   文件写成 `0`，使 `hdc kill` 优雅失败而不是杀死转发器（`hdc kill` 语义
   本就是“杀本机 server”，而本机并没有真正的 server）。同时在
   `.hdcfwd.marker` 里记录转发器真实 pid 用于自身防重复启动。

**server 模式的自愈能力**（对应 adbportforward 的周期 `adb start-server`）
- 启动时、每条新连接到来时（懒加载）、以及每 30 秒（keepalive，可用
  `--keepalive 0` 关闭）都会探测本地 `127.0.0.1:8710`；探测失败则自动以
  分离进程方式拉起 `hdc -m -s 127.0.0.1:8710`（server 仍只监听回环，
  对外暴露的始终只有转发器本身）。

---

## 二、编译

### 方式 A：直接编译（无任何第三方依赖）

```bash
# Linux
g++ -O2 -std=c++17 -o hdcfwd hdcfwd.cpp -lpthread

# Windows (MSVC)
cl /O2 /EHsc hdcfwd.cpp ws2_32.lib

# Windows (mingw 交叉编译)
x86_64-w64-mingw32-clang++ -O2 -std=c++17 -o hdcfwd.exe hdcfwd.cpp \
    -lws2_32 -static -lpthread
```

或直接运行 `./build.sh`（自动产出 Linux + Windows 双平台二进制，Windows
部分优先使用 OpenHarmony 源码树自带的 mingw 工具链，找不到则跳过）。

> Ubuntu 24.04 交叉编译提示：若 clang 报 `libtinfo.so.5` 缺失，从
> Ubuntu 22.04 归档下载 `libtinfo5_6.3-2_amd64.deb` 解出后设置
> `LD_LIBRARY_PATH` 指向它即可。

### 方式 B：在 OpenHarmony 源码树中编译（附：编译官方 hdc）

本工具完全独立，不需要 OpenHarmony 源码树。如需顺带编译官方 hdc
（例如给 client 机配一个新版本 hdc）：

```bash
# 第一次（切产品，需要完整 gn + ninja）
./build.sh --product-name ohos-sdk --build-target developtools/hdc:hdc

# Linux x86_64 宿主机版
./prebuilts/build-tools/linux-x86/bin/ninja -w dupbuild=warn \
    -C out/sdk clang_x64/developtools/hdc/hdc
# 产物: out/sdk/clang_x64/developtools/hdc/hdc
# 依赖: out/sdk/clang_x64/thirdparty/libusb/libusb_shared.so
#       （放同目录并 export LD_LIBRARY_PATH=. 即可）

# Windows x64 宿主机版
LD_LIBRARY_PATH=<libtinfo5目录> ./prebuilts/build-tools/linux-x86/bin/ninja \
    -w dupbuild=warn -C out/sdk mingw_x86_64/developtools/hdc/hdc.exe
# 产物: out/sdk/mingw_x86_64/developtools/hdc/hdc.exe
```

---

## 三、部署与使用

### 场景一：局域网直连（同一网段）

#### 1. 设备直连机器（例如 Windows）—— server 模式

```bat
:: 建议把 hdcfwd.exe 与 hdc.exe 放同一目录（转发器自动发现 hdc），或用 --hdc 指定
hdcfwd.exe server
:: 自定义端口：hdcfwd.exe server -t 9710 -p 8710
::   -t 隧道端口(默认 9710, 监听 0.0.0.0)
::   -p 本地 hdc server 端口(默认 8710, 或环境变量 OHOS_HDC_SERVER_PORT)
```

- 本机原有 hdc 使用方式**完全不受影响**（hdc server 照常只监听回环 8710）；
- hdc server 没启动也没关系，转发器会自动拉起并持续保活；
- 防火墙需放行 9710/tcp（若开启）。

**开机自启（Windows，无需管理员）**

```bat
schtasks /create /tn hdcfwd_server ^
  /tr "C:\path\to\start-hdcfwd.bat" /sc onlogon /f
schtasks /run /tn hdcfwd_server
```

`start-hdcfwd.bat` 内容：
```bat
@echo off
C:\Users\<you>\hdcfwd.exe server -v > C:\Users\<you>\hdcfwd.log 2>&1
```

**Linux 设备机自启**：systemd unit，或 `nohup hdcfwd server &`。

#### 2. 局域网其他机器（例如 Ubuntu）—— client 模式

```bash
hdcfwd client 192.168.200.1        # 参数是设备机的 IP
# 自定义: hdcfwd client <ip> -t 9710 -p 8710
```

启动过程（全自动）：
1. 若本机 8710 被本地 hdc server 占用 → 自动按 pid 文件杀掉（会校验进程
   镜像名，不会误杀无关进程）；
2. 接管 `127.0.0.1:8710` 并持有 hdc server 存在性锁（本机 hdc 客户端零感知）；
3. 之后 `hdc list targets`、`hdc shell`、`hdc file send/recv`、`hdc install`、
   DevEco Studio…… 一切照常，**不需要任何 `-s` 参数**。

**停止**：Ctrl+C 或 kill（自动释放锁与端口，之后本机 hdc 恢复独立运行）。

```bash
# 常驻（示例）
nohup hdcfwd client 192.168.200.1 > /tmp/hdcfwd.log 2>&1 &
```

### 场景二：跨公网/云端 Ubuntu（配套 SSH 端口转发）

适用条件：设备直连机器在**本地**（无公网 IP），使用者在**云端 Ubuntu**——
云端只开放 SSH(22) 端口，且只能本地连云端、云端不能反向连本地。此时局域网
直连不可用，改用 SSH **远程转发（`-R`）**从本地主动把 hdcfwd 隧道端口"背"进
云端：

```
┌──────────────────────┐     仅一条出站 SSH 连接      ┌─────────────────────────┐
│ 本地 Windows (设备机)  │ ──────────────────────────► │ 云端 Ubuntu              │
│  hdc server  127.0.0.1:8710                         │ sshd 监听 127.0.0.1:9710 │
│  hdcfwd server 0.0.0.0:9710 ◄─────────────────────── │   │（ssh -R 反向隧道）    │
│        ▲              转发回本机 127.0.0.1:9710      │   ▼                     │
│        └── USB 设备                                  │ hdcfwd client           │
│                                                     │  127.0.0.1:8710 → 9710  │
│  ssh -N -R 127.0.0.1:9710:127.0.0.1:9710            │        ▲                │
│      user@cloud                                     │        └── 本机 hdc 客户端│
└──────────────────────┘                              └─────────────────────────┘
```

与场景一相比只有两处不同：**本地设备机**多跑一条 ssh 隧道（出站方向，符合
"只能本地连云端"的约束）；**云端**的 hdcfwd client 目标写 `127.0.0.1` 而不是
设备机 IP。两端 hdcfwd 的其余用法完全不变。

> **为什么隧道端口用 9710 而不是 8710**：云端的 `127.0.0.1:8710` 要留给
> hdcfwd client 接管（本机 hdc 客户端直连它），ssh 隧道的监听器不能占用。

#### 1. 本地 Windows：hdcfwd server 先跑起来（同场景一）

见上文场景一第 1 步（`hdcfwd.exe server`，可注册为计划任务常驻）。

#### 2. 本地 Windows：配置免密 SSH（隧道需要长连+自动重连，不适合密码交互）

Windows 10 1809+ 自带 OpenSSH 客户端（`where ssh` 可确认），cmd 执行：

```bat
ssh-keygen -t ed25519
:: 一路回车，生成 %USERPROFILE%\.ssh\id_ed25519(.pub)

:: 把公钥装到云端（这一步需要输一次密码）：
type %USERPROFILE%\.ssh\id_ed25519.pub | ssh user@cloud-ip "mkdir -p ~/.ssh && cat >> ~/.ssh/authorized_keys && chmod 700 ~/.ssh && chmod 600 ~/.ssh/authorized_keys"

ssh user@cloud-ip echo OK        :: 免密验证，输出 OK 即成功
```

#### 3. 本地 Windows：建立隧道（先在前台验证）

```bat
ssh -N -R 127.0.0.1:9710:127.0.0.1:9710 -o ExitOnForwardFailure=yes -o ServerAliveInterval=30 -o ServerAliveCountMax=3 user@cloud-ip
```

- `-N`：只转发、不执行远程命令；`-R A:B:C`：在**云端**监听 A(`127.0.0.1:9710`)，
  连接经 SSH 通道回到**本机**的 B(`127.0.0.1:9710`，即 hdcfwd server)；
- `ExitOnForwardFailure=yes`：远端端口被占用时立即报错退出，而不是静默失效；
- `ServerAliveInterval=30`/`ServerAliveCountMax=3`：防 NAT 空闲断连。

前台跑着这条命令时，就可以在云端执行第 5 步验证全链路；验证通过后再做成常驻。

#### 4. 本地 Windows：隧道常驻（断线自动重连）

`start-tunnel.bat`：

```bat
@echo off
:loop
ssh -N -R 127.0.0.1:9710:127.0.0.1:9710 -o BatchMode=yes -o ExitOnForwardFailure=yes -o ServerAliveInterval=30 -o ServerAliveCountMax=3 -i "%USERPROFILE%\.ssh\id_ed25519" user@cloud-ip
timeout /t 5 /nobreak >nul
goto loop
```

注册为登录自启任务（无需管理员）：

```bat
schtasks /create /tn hdcfwd_tunnel /tr "C:\path\to\start-tunnel.bat" /sc onlogon /f
schtasks /run /tn hdcfwd_tunnel
```

> **多台云端共享同一台设备机**：每台云端建一个 bat/计划任务（隧道命令相同、
> `user@cloud-ip` 不同），各隧道互不干扰。

#### 5. 云端 Ubuntu：启动 hdcfwd client

```bash
ss -ltn | grep 9710        # 先确认 sshd 已在本机 9710 监听（隧道已建立）
hdcfwd client 127.0.0.1    # 注意目标是 127.0.0.1（走 ssh -R 隧道，不是设备机 IP）
hdc list targets           # 无需 -s，透明使用
```

常驻可用 systemd（`/etc/systemd/system/hdcfwd-client.service`）：

```ini
[Unit]
Description=hdcfwd client (hdc transparent forwarder)
After=network-online.target

[Service]
ExecStart=/usr/local/bin/hdcfwd client 127.0.0.1
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload && sudo systemctl enable --now hdcfwd-client
```

#### 6. 顺序与自愈语义

| 事项 | 说明 |
|---|---|
| 启动顺序 | 隧道先于 hdcfwd client：client 启动时会探测 `127.0.0.1:9710`，不通则拒绝启动并明确提示 |
| 隧道闪断 | hdcfwd client **不会退出**；期间 hdc 命令报 `E002110` 连接失败，bat 在 5 秒内自动重连隧道，之后自动恢复（实测约 12 秒全链路恢复） |
| 云端 sshd 要求 | 默认配置即可（`AllowTcpForwarding yes` 为默认值；只监听回环无需 `GatewayPorts`） |
| 安全性 | 比场景一更好：全部流量走 SSH 加密通道，两端都不新增任何公网暴露端口 |

#### 7. 常见问题

- **`Warning: remote port forwarding failed for listen port 9710`**：云端 9710
  被占用（残留隧道或其他进程）。云端 `ss -ltnp | grep 9710` 排查；或换端口
  `-R 127.0.0.1:19710:127.0.0.1:9710`，client 改用
  `hdcfwd client 127.0.0.1 -t 19710`。
- **隧道命令卡住无输出**：确认 `ExitOnForwardFailure=yes` 是否生效、云端
  `/etc/ssh/sshd_config` 是否被管理方设置 `AllowTcpForwarding no`。
- **隧道正常但 hdc 报连接失败**：确认云端启动的是 `hdcfwd client 127.0.0.1`
  而不是 `hdcfwd client <设备机IP>`——后者在云端走不通（云端连不了本地）。

### 命令一览

```
hdcfwd server [options]               # 设备机上运行，对外暴露本地 hdc server
hdcfwd client <server-host> [options] # 局域网机器上运行，透明接管本地 hdc 端口
  -t, --tunnel-port PORT   隧道端口（默认 9710）
  -p, --hdc-port PORT      hdc server 端口（默认 8710 / $OHOS_HDC_SERVER_PORT）
      --hdc PATH           hdc 可执行文件路径（默认: 同目录 > PATH）
      --expose             client 模式监听 0.0.0.0（允许链式转发，注意安全）
      --keepalive SEC      server 模式保活间隔（默认 30 秒，0 关闭）
  -v, --verbose            详细日志
  -h, --help               帮助
```

---

## 四、语义与注意事项

| 行为 | 说明 |
|---|---|
| 设备机本地使用 | 完全不受影响，与隧道同时使用无冲突（已实测并发） |
| `hdc tconn` / `-t <key>` | 透传生效；多设备时照常通过 `-t` 选择 |
| `hdc fport` | 监听器创建在**设备机**上（与 `hdc -s ip fport` 行为一致） |
| client 机 `hdc kill` | 优雅失败（不会杀转发器，也不会杀远端 server） |
| 设备机 `hdc kill` | 杀掉本地 server；转发器在下次连接/保活时自动拉起（秒级恢复） |
| 版本差异 | 默认构建无版本强校验，客户端与服务端 hdc 小版本差异可互通（实测 3.2.0g ↔ 3.2.0b） |
| client 机本地设备 | client 模式会接管 8710（本地 server 被杀），需要本地设备时先停转发器 |
| OHOS_HDC_SERVER_PORT | 转发器与 hdc 读取同一环境变量，保持一致即可 |

**安全提示**：隧道端口(9710)在设备机上对所有网卡开放，等同于“局域网内任何
人可用你的设备”。仅限可信局域网使用；跨不可信网络请用[场景二](#场景二跨公网云端-ubuntu配套-ssh-端口转发)
的 SSH 隧道方式（全程加密，且不新增任何公网暴露端口）。

---

## 五、实测记录

验证环境：Windows 11 (192.168.200.1, DevEco hdc 3.2.0b, USB 真机
`5ccec022...`) ⟵ Hyper-V 虚拟交换机（Public profile 防火墙关闭）⟶
Ubuntu 24.04 (192.168.200.10, hdc 3.2.0g 自源码编译)。

| 用例 | 结果 |
|---|---|
| `hdc list targets`（无 -s） | ✅ 列出设备 |
| `hdc shell`（含 root 身份、交互命令） | ✅ |
| `hdc file send/recv`（含 20MB 大文件） | ✅ 34.4 MB/s |
| `hdc checkserver` | ✅（跨小版本互通） |
| 设备机本地 hdc 同时使用 | ✅ 无冲突 |
| client 机 `hdc kill` | ✅ 转发器存活，隧道恢复 |
| 设备机 server 被杀后自愈 | ✅ 0.56s 恢复 |
| client 机 hdc 误拉起抑制 | ✅（存在性锁生效） |
| 转发器重复启动保护 | ✅ |
| 存在真实 server 时 client 启动拒绝 | ✅（提示先 `hdc kill`） |

**场景二（SSH 隧道 + 云端 Ubuntu）实测**：Windows 主动 `ssh -R` 到远端
Ubuntu（模拟云端：client 只连 `127.0.0.1:9710`，全程不直连设备机）——

| 用例 | 结果 |
|---|---|
| ssh -R 隧道 + `hdcfwd client 127.0.0.1` 全链路 | ✅ list targets / shell / file send |
| 强杀隧道 ssh 进程模拟断线 | ✅ hdcfwd client 存活，bat 5 秒重连，约 12 秒全链路恢复 |
| 数据路径确认 | ✅ 全部流量经 sshd:22 加密通道（`ss -tn` 验证） |

## 六、文件说明

```
hdcfwd.cpp   单文件实现（C++17，Windows/POSIX 双平台，无第三方依赖）
build.sh     一键构建 Linux + Windows 双平台二进制
LICENSE      Apache License 2.0
```
