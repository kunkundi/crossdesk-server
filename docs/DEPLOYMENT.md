# 部署与构建指南

[返回项目首页](../README.md) · [English](DEPLOYMENT_EN.md)

[部署服务](#运行服务) · [ICE 节点](#ice) · [证书与客户端](#clients) · [状态接口](#stats) · [源码构建](#build)

## 运行服务

### 1. 准备部署配置

在已安装 **Docker Engine 和 Compose 插件**的 Linux 服务器上操作，先确认 `docker compose version` 可用。Compose 使用主机网络；`INTERNAL_IP` 必须是宿主机实际可绑定的地址。

从同一个 [GitHub Release](https://github.com/kunkundi/crossdesk-server/releases) 下载 **`compose.yaml`** 和 **`env.example`**，放在单独的部署目录中。在该目录执行：

```bash
cp env.example .env
```

Release 附件中的 `env.example` 已固定对应镜像 tag。源码仓库中的文件名是 [`.env.example`](../.env.example)，其镜像值为 `latest`；使用它时请自行选择固定发布 tag。后续 Compose 命令都在配置目录执行。

### 2. 编辑 `.env`

用文本编辑器填写以下配置，替换所有示例地址。云服务器存在公网 NAT 时，公网和内网 IP 通常不同；公网 IP 直接配置在网卡上时，两项可以相同。

| 配置 | 用途 / 默认值 |
| --- | --- |
| `CROSSDESK_IMAGE` | 使用与部署文件配套的固定发布 tag |
| `EXTERNAL_IP` | 公网 IP，用于默认 TURN 地址与自动证书生成 |
| `INTERNAL_IP` | Coturn 监听和中继使用的本机网卡 IP |
| `CROSSDESK_SERVER_PORT` | WSS / HTTPS 共用端口，Compose 默认 `9099` |
| `COTURN_PORT` | STUN / TURN 端口，默认 `3478` |
| `MIN_PORT` / `MAX_PORT` | TURN 媒体端口范围，默认 `50000`–`60000` |
| `COTURN_AUTH_SECRET` | 信令服务与 Coturn 共享的签名密钥，不填入客户端 |
| `COTURN_STATELESS_NONCE_SECRET` | 独立的 nonce 密钥，生成后持久保存 |
| `CROSSDESK_DATA_DIR` | 数据与证书的宿主机目录，默认 `/var/lib/crossdesk` |
| `CROSSDESK_LOG_DIR` | 业务日志的宿主机目录，默认 `/var/log/crossdesk` |

分别运行两次下面的命令，将两个不同的输出填入上述两个密钥字段：

```bash
openssl rand -hex 32
openssl rand -hex 32
```

可选项 `COTURN_PUBLIC_HOST` 用于向客户端公布独立的 TURN 域名 / IP，留空时使用 `EXTERNAL_IP`。`COTURN_CREDENTIAL_TTL_SECONDS` 默认 `3600` 秒，范围为 `60`–`86400`；`COTURN_LOG_LEVEL` 默认 `warning`，`COTURN_MEMORY_LIMIT` 默认 `512m`。完整参数见 [环境变量示例](../.env.example)。

客户端登录和会话协商时由信令服务签发临时 TURN 用户名、密码；不要在客户端或 Web 页面中配置服务器共享密钥。更新服务端时同时核对客户端的版本兼容性。

### 3. 放通端口并启动

防火墙及云安全组需要允许：

| 端口 | 协议 | 用途 |
| --- | --- | --- |
| `CROSSDESK_SERVER_PORT`，默认 `9099` | TCP | WSS、状态接口与管理后台 |
| `COTURN_PORT`，默认 `3478` | TCP / UDP | STUN / TURN |
| `MIN_PORT`–`MAX_PORT`，默认 `50000`–`60000` | UDP | TURN 中继媒体 |

保存 `.env` 后执行：

```bash
sudo docker compose config -q
sudo docker compose pull
sudo docker compose up -d --no-build
sudo docker compose ps
```

`crossdesk_server` 会首次生成证书并创建数据库；`crossdesk_coturn` 等待证书文件就绪后启动。Compose 的健康检查只确认密钥和证书文件存在，还需通过下文的状态接口及实际客户端连接验证服务。

本文连接示例均使用 Compose 默认端口 **9099**；如果修改了端口，后续 URL 和客户端配置也要一起修改。直接运行二进制且不传端口参数时，程序默认使用 **9090**。

<a id="ice"></a>

### 会话 ICE 节点

默认根据 `COTURN_PUBLIC_HOST`（为空时用 `EXTERNAL_IP`）和 `COTURN_PORT` 下发 STUN、UDP / TCP TURN 节点。需要自定义完整列表时，在 `.env` 设置 `CROSSDESK_ICE_SERVERS`；它会**替代默认列表**，不是追加。示例：

```dotenv
CROSSDESK_ICE_SERVERS='[{"urls":["stun:203.0.113.10:3478"]},{"urls":["turn:203.0.113.10:3478?transport=udp","turn:203.0.113.10:3478?transport=tcp"]}]'
```

将示例 IP 换成实际地址。TURN 条目未提供 `shared_secret` 时使用 `COTURN_AUTH_SECRET`；单独指定的密钥必须与对应 TURN 服务一致，只保存在服务端。变量未设置或留空时使用默认列表；显式 `[]` 表示不下发 STUN / TURN，仅允许本地候选。

当前解析器支持 `stun:` 和 `turn:`，TURN 可选择 UDP / TCP，STUN URL 不接受 `transport` 参数；原生 ICE 路径暂不支持 `turns:`。配置最多 16 个条目、合计 32 个 URL，STUN / TURN 各最多 8 个不同节点；非法配置会阻止启动。校验规则见 [配置签发实现](../src/ice_server_config_issuer.cpp)。

上报 `ice_config_version: 1` 的客户端接收带有效期的完整会话配置，替代本地 ICE 列表；服务端同时保留旧临时 TURN 凭据以兼容相应旧客户端。备用 STUN / TURN 也应在这里配置，让原生端和 Web 使用相同节点。修改 `.env` 后重新创建信令容器；改地址不会迁移已建立的媒体路径。

<a id="clients"></a>

## 证书与客户端连接

### 信任自己的服务器证书

默认文件位于 `${CROSSDESK_DATA_DIR}/certs/`：

| 文件 | 用途 |
| --- | --- |
| `api.crossdesk.cn_root.crt` | 分发到客户端的根证书 |
| `api.crossdesk.cn_bundle.crt` | 服务端证书链 |
| `api.crossdesk.cn.key` | 服务端私钥，保留在服务器 |

这些是程序约定的**文件名**，不要求服务器使用 `api.crossdesk.cn` 域名。[自动生成脚本](../docker/generate_certs.sh) 只将 `EXTERNAL_IP` 写入证书的 IP 地址范围；客户端应使用该 IP。使用自定义域名时，需提供包含该域名的有效证书链和私钥，分别放到 `bundle.crt` 和 `.key` 对应的上述路径。

[启动脚本](../docker/start.sh) 会复用已有证书，修改 `.env` 中的 IP 不会重新签发证书。重新生成自签证书会更换根证书，所有客户端都需重新信任。使用受系统信任的 CA 证书时通常无需额外导入根证书。

将自己部署的 `api.crossdesk.cn_root.crt` 复制到客户端后，按平台导入系统信任库：

```powershell
# Windows：管理员 PowerShell，替换为实际文件路径
certutil -addstore "Root" "C:\path\to\api.crossdesk.cn_root.crt"
```

```bash
# Ubuntu / Debian
sudo cp /path/to/api.crossdesk.cn_root.crt /usr/local/share/ca-certificates/
sudo update-ca-certificates
```

```bash
# macOS
sudo security add-trusted-cert -d -r trustRoot \
  -k /Library/Keychains/System.keychain /path/to/api.crossdesk.cn_root.crt
```

### 配置控制端与被控端

1. 在桌面端打开 **设置 → 常规 → 自建服务配置**。
2. 填写服务器 IP / 域名及信令端口 `9099`，确认后返回。地址不带协议、路径或端口；当前版本由服务端下发 TURN 地址，无需单独填写 TURN 端口。
3. 勾选 **自建服务配置**，点击设置窗口的 **保存**。控制端与被控端使用同一套服务。
4. 信任证书后重新连接客户端，等待连接服务器成功，使用被控端当前显示的 ID 和密码连接。

桌面端使用**系统信任库**，此弹窗没有证书文件选择器。切换服务器后重新核对设备 ID；当前桌面端自动启用 SRTP，可与浏览器互通。

- **Web：** 在主脚本加载前设置 `window.CROSSDESK_CONFIG.signalingUrl`，详见 [Web 部署指南](https://github.com/kunkundi/crossdesk-web-client/blob/HEAD/docs/DEPLOYMENT.md#configuration)。浏览器同样需要信任 WSS 证书，会话 ICE 节点由服务端下发。
- **原生移动端：** 在应用的服务器设置中填写同一信令主机与端口。证书处理受平台影响，建议使用系统受信任 CA 签发的证书，尤其是 Android；具体见 [iOS](https://github.com/kunkundi/crossdesk/tree/HEAD/apps/ios) 和 [Android](https://github.com/kunkundi/crossdesk/tree/HEAD/apps/android) 应用说明。

<a id="stats"></a>

## 服务状态接口

`GET /stats` 和 `GET /api/stats` 与 WSS 共用 HTTPS 端口，**无需管理登录**，返回汇总统计并允许跨域读取。可在服务器上使用以下命令检查；替换示例 IP，若调整了数据目录也需修改根证书路径：

```bash
curl --fail --cacert /var/lib/crossdesk/certs/api.crossdesk.cn_root.crt \
  https://203.0.113.10:9099/stats
```

使用受信任 CA 的域名证书时：

```bash
curl --fail https://your-domain.example.com:9099/stats
```

| 字段 | 含义 |
| --- | --- |
| `online_device_count` | 当前在线原生设备总数（电脑 + 移动端），排除 `web-*` 与 `C-*` |
| `online_pc_client_count` | 当前在线电脑客户端数，未上报平台的原生设备也计入此项 |
| `online_mobile_client_count` | 当前在线 iOS / Android 客户端数，按已上报平台识别，排除 `web-*` 与 `C-*` |
| `online_web_client_count` | 当前在线 `web-*` 客户端数 |
| `active_connection_count` | 活动远控连接数，按 host 与各 guest 的连接分别计数 |
| `online_duration_seconds` | 当前在线设备的本次在线时长合计 |

服务端不再统计或返回累计在线、累计控制、累计被控时长。旧数据库中的对应列保留但停止读写，新数据库不创建这些列；本次在线与远控时长继续提供。

时长单位为秒。管理后台的明细接口使用 `/api/admin/*`，需要管理登录；公共状态接口不会返回设备明细。

<a id="build"></a>

## 从源码构建

CI 在 **Ubuntu 22.04、amd64 / arm64** 上构建。安装 Git、C++17 编译工具链和 [Xmake](https://xmake.io/guide/quick-start.html)；第三方 C++ 依赖由 Xmake 下载。下面的 Linux 依赖命令与 CI 基线一致：

```bash
sudo apt-get update
sudo apt-get install -y ca-certificates git curl unzip build-essential

git clone https://github.com/kunkundi/crossdesk-server.git
cd crossdesk-server
xmake f -c -m release -y
xmake b -vy crossdesk_server
```

首次构建前需确保 `xmake --version` 可用。其他系统的源码配置见 [xmake.lua](../xmake.lua)，当前发布流程只提供 Linux 二进制与容器镜像。切换 Debug 使用 `xmake f -m debug`；`xmake r -d crossdesk_server` 表示使用调试器运行。

认证队列回归与旧协议 WSS 集成测试（需要 Python 3、`websockets>=15` 和 `openssl` 命令）：

```bash
xmake b credential_queue_test credential_server_fixture
xmake r credential_queue_test
# 按当前平台、架构和构建模式替换 fixture 路径。
python3 tests/credential_server_integration.py \
  --fixture build/linux/x86_64/release/credential_server_fixture --clients 2100
```

测试在临时目录创建独立证书与数据库，验证来源等待超过 15 秒、断线释放名额、队列到期，以及集中登录、停服和重启后的再次登录。所有登录都执行真实 Argon2id 校验，模拟客户端不重试登录；TCP/TLS 握手并发限制为 64，避免本机 listen 队列先溢出。集中登录用例提高回环 IP 的来源额度，来源限额另有独立用例；结果不代表默认配置允许同一 IP 下 2100 台设备同时接纳，也不代表生产机器性能。

### 构建自己的运行镜像

Dockerfile **只打包已编译的 Linux 二进制**。先在与目标镜像架构相同的 Ubuntu 22.04 环境完成 Release 编译，再在仓库根目录执行：

```bash
# amd64；arm64 将 x86_64 替换为 arm64
install -Dm755 build/linux/x86_64/release/crossdesk_server dist/crossdesk_server
sudo docker build -f docker/dockerfile -t crossdesk-server:local .
```

在仓库根目录将 `.env.example` 复制为 `.env`，按部署章节填写配置，并将 `CROSSDESK_IMAGE` 设为 `crossdesk-server:local`。然后执行：

```bash
sudo docker compose config -q
sudo docker compose pull coturn
sudo docker compose up -d --no-build
```

直接运行二进制时，需要先准备 [main.cpp](../src/main.cpp) 规定的 `/var/lib/crossdesk/certs`、数据库和日志路径；`CROSSDESK_DATA_DIR` / `CROSSDESK_LOG_DIR` 是 Compose 的宿主机挂载设置，不是二进制的路径覆盖参数。二进制不会自动生成证书。

### CI 测试镜像

默认分支成功构建后发布多架构 `crossdesk/crossdesk-server:test`，该 tag 随后续构建变化。测试环境可在 `.env` 中设置 `CROSSDESK_IMAGE=crossdesk/crossdesk-server:test`，再执行部署章节的拉取与启动命令。正式部署使用固定发布 tag。发布产物与镜像流程见 [CI 工作流](../.github/workflows/build.yml)。
