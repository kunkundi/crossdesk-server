# 运维与故障排查

[返回项目首页](../README.md) · [English](OPERATIONS_EN.md)

[日志与更新](#查看日志与更新) · [密码保护](#设备密码验证保护) · [重连凭据](#专用重连凭据) · [容量调优](#在线容量与重启恢复调优) · [留存](#数据保存与自动清理) · [备份](#备份与迁移) · [常见问题](#常见问题)

<a id="operations"></a>

## 维护与排查

### 查看日志与更新

在配置目录执行：

```bash
sudo docker compose ps
# 默认日志目录；自定义 CROSSDESK_LOG_DIR 时替换路径。
sudo sh -c 'tail -n 100 /var/log/crossdesk/crossdesk-server_$(date +%F).log'
sudo sh -c 'tail -n 100 /var/log/crossdesk/coturn/turn_$(date +%F).log'
```

更新时先备份，再修改 `.env` 中的固定镜像 tag，并使用与该版本匹配的部署文件：

```bash
sudo docker compose config -q
sudo docker compose pull
sudo docker compose up -d --no-build
```

`docker compose restart` 用于重启现有容器，不会应用修改后的环境变量或镜像。信令日志按服务进程的本地日期追加到 `${CROSSDESK_LOG_DIR}/crossdesk-server_YYYY-MM-DD.log`；Coturn 日志写入同目录下的 `coturn/turn_YYYY-MM-DD.log`，默认容器时区为 UTC，查看时请选择实际存在的日期文件。新版 Compose 关闭 Docker 日志副本，避免只按容量轮转的副本绕过保存期限；`docker compose logs` 不再提供这两个服务的日志。启动失败时，可用 `sudo docker compose run --rm --service-ports crossdesk-server` 在前台查看启动输出，运行前先停止原信令容器以避免端口冲突。

信令服务的连接诊断日志可用于排查“进程仍在运行，但客户端无法连接”：

| 日志 | 关键字段 |
| --- | --- |
| `Connection capacity` | 启动时配置/实际连接上限、`fd_soft_limit`、预留 FD；因 nofile 降低容量时另有告警 |
| `Connection admission paused / resumed` | `blocked_by_*` 区分连接总量、未打开连接额度、FD 额度或 FD 采样失败；记录暂停时长和重试检查次数 |
| `Connection diagnostics` | 启动约 5 秒后首次输出，随后约每 60 秒及停服时汇总连接、FD、积压和累计错误 |
| `Signal event loop delayed` | 事件循环定时检查延迟至少 5 秒时记录延迟量 |

接入暂停、accept/资源错误、握手前失败及事件循环延迟的同类告警最多每 30 秒输出一次；恢复日志只与已输出的暂停告警配对。`*_total` 是进程启动以来的累计计数，限频不影响计数；`*_limit_checks_total` 统计触发限制的检查次数，并非被拒客户端数。`unopened` 包括尚未打开的 WebSocket、普通 HTTPS 请求及其清理阶段。`fd_open` 是日志生成时实测值，`fd_estimated` 是接入检查使用的估计值，`fd_sample_age_ms` 表示其采样年龄；`-1` 表示不可用或无限制。`expired_handles` 和 `oldest_unopened_ms` 可帮助发现残留或长时间未完成的连接。

### 设备密码验证保护

设备密码保持 **6 位大小写字母与数字**，改用 OpenSSL `RAND_priv_bytes()` 和拒绝采样生成；盐使用 `RAND_bytes()` 生成独立的 16 字节随机值。随机源或 Argon2id 不可用时拒绝启动／操作，不回退到伪随机数或 SHA-256。

新注册与改密使用 **Argon2id v19，19 MiB 内存、2 次迭代、并行度 1、32 字节摘要**。现有 `password_hash` 列保存 `argon2id$v=19$m=19456,t=2,p=1$` 前缀和十六进制摘要，`password_salt` 保存十六进制盐。旧版加盐 SHA-256 记录在密码验证成功后原子升级，设备 ID 和六位密码不变；长期未验证的旧记录继续按设备保留期限清理。哈希升级不能使旧密码或已泄漏备份失效，需要时应通过客户端更换密码。旧版服务端无法验证升级后的记录，回滚必须使用升级前备份。

密码计算默认由两个独立工作线程执行，使用按来源 IP 轮转的有界等待队列。默认最多接纳 4096 个请求（包含执行中和等待提交的结果），同一 IP 最多占用 256 个；等待请求不占用计算线程。不会在计算时持有数据库或失败限流锁。每个连接保持消息顺序，完成后复查连接身份及凭据快照，过期结果不能覆盖并发改密或恢复已删除身份。未知设备、旧格式错误密码和损坏的记录也执行一次 Argon2id，减少耗时差异。启动时验证本机 OpenSSL provider 支持上述算法和参数。

注册、登录、查询、加入会话及新改密计算共用来源额度：默认每个来源 IP 每分钟最多启动 60 次，成功操作同样计数；不再设置整个进程每分钟 300 次的配额。仅在获得空闲计算线程后扣除额度，排队、断线取消和队列满拒绝都不提前扣费。密码计算来源额度用尽时在服务器内等待下一窗口，不阻塞其他来源。队列满或排队到期仍返回原协议的 `Credential service busy` 和 `retry_after`，失败冷却规则保持不变。

已接纳的认证请求默认最多等待／执行 600 秒，排队期间不触发原来的 15 秒未登录断开；从未提交有效认证请求的连接仍按 15 秒规则清理。旧客户端无需识别新协议或实现 `retry_after` 重试，即可收到排队后的原登录响应。600 秒是最长保留期限，并非固定等待时间；断线会取消尚未执行的请求。重启时恢复中的远控记录保留窗口随认证期限延长，避免客户端还在排队时记录就被清理。

六位密码的强度仍有限，来源计算额度与下述失败冷却共同限制在线猜测；慢哈希主要提高数据库泄漏后的离线猜测成本。

同一 WebSocket 连接只允许一次成功登录。后续 `login` 请求返回 `Already authenticated`，不会再注册设备、验证密码或更新在线状态；如需重新登录，请建立新连接。首次登录失败后仍可在原连接重试。

`query_user_id_list` 和 `join_transmission` 要求连接先完成登录，加入会话时还校验请求者身份与连接绑定。设备密码错误和设备 ID 不存在使用相同回复，不会返回设备列表或连接凭据。登录和查询保留 `Authentication failed`；Join 为兼容已发布桌面端，统一使用 `reason: "Incorrect password"` 和 `error_code: "AUTHENTICATION_FAILED"`，不恢复可区分 ID 是否存在的报错。

旧桌面端的空密码 Join（`ID@`）返回 `Incorrect password` / `PASSWORD_REQUIRED` 以打开密码框；不查询设备是否存在、不进行密码验证，也不消耗密码计算或失败额度。此提示不授予任何会话权限，不清除或延长已有冷却。新客户端应先收集密码再 Join。

Join 的其他拒绝（冷却、认证服务繁忙、未登录、远端不可用）统一保留旧客户端可识别的 `reason: "Remote unavailable"`，使其结束等待；旧版界面会显示设备离线。新客户端优先读取 `error_code`：`AUTHENTICATION_THROTTLED`、`CREDENTIAL_SERVICE_BUSY`、`CREDENTIAL_RATE_LIMITED`、`NOT_AUTHENTICATED` 或 `REMOTE_UNAVAILABLE`，并在存在 `retry_after` 时显示剩余秒数、禁止提前重试。认证诊断按错误码计数，空密码提示不计为鉴权失败。

设备登录、查询和加入会话共用失败计数：15 分钟内，同一来源 IP 失败 20 次，或同一目标设备失败 5 次，将从达到阈值时起冷却 15 分钟。冷却期间跳过密码验证（包括正确密码），返回 `Too many authentication attempts`（Join 使用上述兼容文案）及 `error_code: "AUTHENTICATION_THROTTLED"` 和以秒计的 `retry_after`。重连、更换控制端 ID、切换接口或成功验证其他设备都不会清除失败计数；被限流的请求不延长冷却时间。已有远控会话不受影响。

来源使用 TCP 对端 IP（不含端口），不信任请求字段或 `X-Forwarded-For`。同一 NAT 或反向代理后的连接共用来源额度。计数保存在单个服务进程内，重启后清空；多实例部署需要在可信网关或共享存储中补充统一限流。来源和目标各最多保留 16,384 条记录，容量耗尽时拒绝新增验证键，直到过期记录释放空间。

注册需显式使用空设备 ID（原生客户端）或 `web`（Web 客户端）。携带不存在或已过期的 ID 登录会返回统一认证失败，不再自动分配新 ID；客户端需通过重置本地身份重新发起注册。

### 专用重连凭据

原生客户端在注册或密码登录时携带 `reconnect_version: 1`，成功响应将返回 `reconnect_token`（32 字节安全随机数的 64 位小写十六进制编码）和 `reconnect_expires_at`（Unix 秒）。每个设备最多保存一个有效凭据，绝对有效期为 30 天；数据库只保存 SHA-256 摘要，服务端重启后仍可验证。旧客户端不携带此字段时保持原有协议。

后续断线重连发送以下请求，`user_id` 只包含设备 ID，不包含密码；已有的会话恢复与 ICE 能力字段仍可同时携带：

```json
{"type":"login","user_id":"123456789","reconnect_version":1,"reconnect_token":"<64 位小写十六进制凭据>","session_resume_version":1,"ice_config_version":1}
```

验证成功返回 `status: "success"`、`reconnected: true`、`reconnect_version: 1` 和原到期时间，并正常更新在线状态、恢复会话及重新开始心跳计时。该路径不进入 Argon2id 队列、不消耗密码计算的来源额度，也不读写密码失败计数；仍受连接总量和消息队列限制。重连不轮换凭据、不延长有效期，响应丢失后可以安全重试。

凭据仅用于设备自身登录，不能代替访问其他设备所需的密码或远控会话票据。空身份、`web`、`web-*` 和 `C-*` 临时身份不签发凭据。再次执行支持新协议的密码登录会替换旧凭据；修改设备密码、删除设备或到期后旧凭据失效。已登录设备还可发送 `{"type":"revoke_reconnect_credential"}` 撤销自己的凭据，成功响应同名 `type` 和 `status: "success"`，不关闭现有连接。

无效凭据统一返回 `Authentication failed` 和 `reconnect_rejected: true`，服务端不会自动转入昂贵的密码计算。MiniRTC 客户端收到失败后清除凭据，在同一连接回退一次密码登录；再次失败进入原有错误流程。凭据按客户端连接实例保存在内存中，断网或服务端重启时复用；退出应用、重建实例、切换服务器或更换身份后重新进行密码认证，不新增本地明文凭据文件。新客户端连接旧服务端时继续使用密码协议。

`Connection diagnostics` 新增 `reconnect_success_total` 和 `reconnect_failed_total`；成功重连同时计入 `login_success_total`，不会增加 `Credential diagnostics` 的密码计算次数。

### 在线容量与重启恢复调优

在 `.env` 配置下列选项，并重新创建信令容器：

| 配置 | 默认值 | 作用与范围 |
| --- | --- | --- |
| `CROSSDESK_MAX_CONNECTIONS` | `8192` | 信令连接总上限，1–65536；设备、`C-*` 控制端及未完成连接都占用名额 |
| `CROSSDESK_NOFILE` | `32768` | Compose 容器文件描述符上限；实际连接容量还会扣除已用与预留 FD |
| `CROSSDESK_AUTH_WORKERS` | `2` | 同时执行密码计算的线程数，1–32；影响 CPU、内存和认证吞吐量 |
| `CROSSDESK_AUTH_QUEUE_CAPACITY` | `4096` | 总未完成认证请求数，1–16384；扩大它能接住突发，不提高计算速度 |
| `CROSSDESK_AUTH_SOURCE_CAPACITY` | `256` | 同一来源 IP 的未完成请求数，1–4096；防止单一来源占满队列 |
| `CROSSDESK_AUTH_SOURCE_PER_MINUTE` | `60` | 单 IP 每分钟启动认证数，1–6000；同一 NAT／反向代理后的客户端共享 |
| `CROSSDESK_AUTH_TIMEOUT_SECONDS` | `600` | 接纳后的最大认证期限，15–900 秒 |

2000 台设备保持在线不需要 2000 个认证线程；只有登录、加入会话等需要校验密码的操作使用线程。先保留 2 个线程，在目标机器测量认证吞吐量再增加，避免挤占 TLS、心跳和数据库的 CPU。每个正在计算的 Argon2id 任务需要约 19 MiB 算法内存，等待队列没有这项开销。使用密码集中恢复时，耗时约为请求数除以实测完成速率，另受单 IP 配额约束；队列期限要覆盖预计恢复时间。大量设备共用出口时，应同时评估来源队列、来源额度和恢复期限。

现有 `.env` 的值会覆盖新默认值，例如保留 `CROSSDESK_MAX_CONNECTIONS=2048` 不会自动变成 8192。服务端请求的 TCP listen backlog 为 4096，实际值受操作系统上限约束；同时处理的未打开连接仍限制为 128。上述参数用于信令与重启恢复，不能代替目标机器上的持续在线、并发远控及 TURN 带宽压测。

`Credential capacity` 记录实际生效的认证配置；`Credential diagnostics` 每分钟记录 `active`、`waiting`、`queue_full_total`、`source_full_total`、`expired_total`、`source_rate_waits_total` 及计算耗时。`Connection diagnostics` 同时记录登录成功、认证失败、繁忙响应、失败冷却响应和认证超时的累计次数。队列持续增长表示处理速度跟不上请求；`source_rate_waits_total` 增长表示来源额度等待，单独增加线程无法解除它。

### 数据保存与自动清理

| 数据 | 默认规则 |
| --- | --- |
| 信令与 Coturn 的运行／连接日志 | 已关闭文件最后写入满 **185 天**后删除；按日切分，不按容量提前覆盖 |
| 设备在线状态、版本／平台 | 设备及其控制端分身均离线超过 **180 天**后一起清理；任一身份在线、最近 180 天内使用过或仍被活动／恢复中会话引用时跳过 |
| 服务端历史设备关联 | 随过期设备清理；无有效设备状态的旧关联按更新时间满 **180 天**后清理；旧库无时间戳的关联从首次升级开始计时 |
| 设备 ID、密码盐和密码摘要 | 随过期设备在同一事务中删除；旧版本只留下身份、缺少离线时间的记录从首次升级开始计时 |
| 控制端日活去重记录 | 按北京时间仅保留今日、昨日；设备历史／身份清理时同步删除，普通断开连接不删除 |
| 管理员数据操作审计 | 按 `CROSSDESK_LOG_RETENTION_DAYS`（默认 **185 天**）从记录生成时间计算，分批删除到期记录 |
| 订阅关系、管理员会话 | 按原有登出／连接断开／会话过期规则清理；设备到期删除时也移除其内存订阅 |

通过 `.env` 的 `CROSSDESK_LOG_RETENTION_DAYS=185`、`CROSSDESK_OFFLINE_RETENTION_DAYS=180`、`CROSSDESK_RETENTION_INTERVAL_SECONDS=3600` 调整。天数范围为 1–3650，间隔范围为 60–86400 秒；空值使用默认值，非法值阻止启动，不会被解释为立即删除或永久保留。混合日志包含网络安全信息，缩短期限前须核对适用的法定留存要求；默认值不是对完整法律合规的保证。

服务开始运行后约 5 秒执行第一轮，此后默认每小时执行。数据库每批最多选取 200 个过期设备（含对应控制端分身）和 200 条其他过期关联，有积压时约 5 秒后继续，事务失败回滚并重试。离线期限从最后离线时间计算，恰好达到期限时不删除；没有状态记录的新登记身份从登记时计时，管理员仅清理历史并保留身份时从该操作重新计时。实际完成时间受积压和运行状态影响。按整份日志文件清理，因此文件首条记录可能比配置期限多保留约一天，再加一次检查间隔。服务停止期间不执行清理，重新启动后继续。

日志清理只处理指定日志目录中的已知日志文件，包括旧版 `crossdesk-server-YYYYMMDD-HHMMSS[.N].log`，以及 `coturn/turn_YYYY-MM-DD.log`；不递归扫描其他目录、不跟随符号链接，也不删除当前信令日志。确保该目录仅供这一组实例使用，并监控磁盘空间和 `Retention cleanup`／清理失败日志。文件日志无法初始化时服务启动失败。设备到期删除后，原设备 ID 和密码不再有效；再次上线需重置本地身份，以空设备 ID 发起新设备登记。

SQLite 启用 `secure_delete`，删除后尝试非阻塞 WAL 检查点；这不等于对文件系统快照、备份或底层介质的安全擦除。备份、外部日志平台、旧容器及其日志副本由部署方单独设置到期删除；本程序不会扫描或删除它们。升级时应重新创建容器应用日志配置，并按实际留存义务处置旧副本。隐私政策中的期限需在生产环境部署生效后同步更新。

### 备份与迁移

备份 `.env`、`compose.yaml` 及 `${CROSSDESK_DATA_DIR}` 下的 `certs/` 和 `db/`；备份包含密钥与设备数据库。以下示例使用默认数据目录，在停止服务后打包，期间远程连接会中断：

```bash
sudo docker compose stop
backup_file="crossdesk-backup-$(date +%Y%m%d-%H%M%S).tar.gz"
sudo tar -czf "$backup_file" .env compose.yaml -C /var/lib/crossdesk certs db
sudo docker compose up -d --no-build
```

迁移时在目标服务器恢复配套文件，核对挂载目录和权限，再启动。公网 IP / 域名改变时，还需更新证书与客户端地址。旧部署使用自定义数据目录时，把 `.env` 的 `CROSSDESK_DATA_DIR` 和 `CROSSDESK_LOG_DIR` 指向原有目录。

### 常见问题

| 现象 | 检查方向 |
| --- | --- |
| 配置校验失败 | 必填 IP 和两个密钥是否为空，配置文件是否来自同一 Release |
| 证书生成 / 数据库写入失败 | 查看信令日志，确认挂载路径可写；默认镜像以 root 启动，只有修改容器用户等情况下才按实际 UID/GID 调整权限 |
| Coturn 无法绑定地址 | `INTERNAL_IP` 是否属于宿主机网卡，端口是否被其他进程占用 |
| 客户端 TLS 错误 | 系统根证书信任、证书有效期及 IP / 域名匹配；修改 IP 不会更新已有证书 |
| 已连服务器但对端离线 | 两端是否使用同一服务，被控端是否运行，是否复制了当前 ID |
| P2P 失败且无法中继 | 客户端中继选项、客户端版本、共享密钥一致性、TURN 与媒体端口是否放通 |
| 后台未启用 / 登录失败 | 两个管理环境变量是否均已传入，是否重新创建容器，是否使用 HTTPS |
