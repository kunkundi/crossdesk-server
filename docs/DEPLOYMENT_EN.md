# Deployment and build guide

[Project overview](../README_EN.md) · [中文](DEPLOYMENT.md)

[Run services](#run-services) · [ICE endpoints](#ice) · [Certificates and clients](#clients) · [Status API](#stats) · [Build](#build)

## Run services

### 1. Prepare the deployment files

Use a Linux server with **Docker Engine and the Compose plugin** installed, and confirm that `docker compose version` works. Compose uses host networking; `INTERNAL_IP` must be an address the host can bind.

Download **`compose.yaml`** and **`env.example`** from the same [GitHub Release](https://github.com/kunkundi/crossdesk-server/releases) into a dedicated deployment directory. Run there:

```bash
cp env.example .env
```

The release's `env.example` pins the corresponding image tag. The source repository instead provides [`.env.example`](../.env.example), which uses `latest`; choose a fixed release tag when using that file. Run all subsequent Compose commands from the configuration directory.

### 2. Edit `.env`

Complete these values in a text editor and replace every example address. Public and private IPs usually differ on cloud hosts behind NAT. They can be identical when the public IP is assigned directly to the host's interface.

| Variable | Purpose / default |
| --- | --- |
| `CROSSDESK_IMAGE` | Fixed release tag matching the deployment files |
| `EXTERNAL_IP` | Public IP used for the default TURN endpoint and generated certificate |
| `INTERNAL_IP` | Local interface IP used by Coturn for listening and relaying |
| `CROSSDESK_SERVER_PORT` | Shared WSS / HTTPS port; Compose defaults to `9099` |
| `COTURN_PORT` | STUN / TURN port; defaults to `3478` |
| `MIN_PORT` / `MAX_PORT` | TURN media port range; defaults to `50000`–`60000` |
| `COTURN_AUTH_SECRET` | Signing secret shared by signaling and Coturn; never enter it in clients |
| `COTURN_STATELESS_NONCE_SECRET` | Separately generated, persistent nonce secret |
| `CROSSDESK_DATA_DIR` | Host data/certificate directory; defaults to `/var/lib/crossdesk` |
| `CROSSDESK_LOG_DIR` | Host application-log directory; defaults to `/var/log/crossdesk` |

Run the following twice and use the two different outputs for the two secret fields above:

```bash
openssl rand -hex 32
openssl rand -hex 32
```

Optional `COTURN_PUBLIC_HOST` advertises a separate TURN hostname/IP; an empty value uses `EXTERNAL_IP`. `COTURN_CREDENTIAL_TTL_SECONDS` defaults to `3600` seconds, with a range of `60`–`86400`. `COTURN_LOG_LEVEL` defaults to `warning`, and `COTURN_MEMORY_LIMIT` to `512m`. See the [environment example](../.env.example) for all options.

The signaling server issues temporary TURN usernames/passwords during client login and session negotiation. Keep server shared secrets out of clients and Web pages. Check client compatibility when upgrading the server.

### 3. Open ports and start

Allow these ports through the host firewall and cloud security group:

| Port | Protocol | Purpose |
| --- | --- | --- |
| `CROSSDESK_SERVER_PORT`, default `9099` | TCP | WSS, status API, and admin dashboard |
| `COTURN_PORT`, default `3478` | TCP / UDP | STUN / TURN |
| `MIN_PORT`–`MAX_PORT`, default `50000`–`60000` | UDP | TURN media relay |

After saving `.env`, run:

```bash
sudo docker compose config -q
sudo docker compose pull
sudo docker compose up -d --no-build
sudo docker compose ps
```

`crossdesk_server` creates its certificate and database on first startup; `crossdesk_coturn` waits for the certificate files. The Compose health check only verifies that the key/certificate files exist. Also verify the status API below and an actual client connection.

All connection examples below use the Compose default, **9099**. If you change it, update subsequent URLs and client settings as well. Running the binary directly without a port argument defaults to **9090**.

<a id="ice"></a>

### Session ICE endpoints

By default, signaling advertises STUN and UDP / TCP TURN endpoints using `COTURN_PUBLIC_HOST` (or `EXTERNAL_IP`) and `COTURN_PORT`. To supply a complete custom list, set `CROSSDESK_ICE_SERVERS` in `.env`. This **replaces**, rather than appends to, the default list. For example:

```dotenv
CROSSDESK_ICE_SERVERS='[{"urls":["stun:203.0.113.10:3478"]},{"urls":["turn:203.0.113.10:3478?transport=udp","turn:203.0.113.10:3478?transport=tcp"]}]'
```

Replace the documentation IP with your own endpoint. A TURN entry uses `COTURN_AUTH_SECRET` unless it supplies its own `shared_secret`, which must match that TURN service. Keep those secrets on the server. An unset or blank variable selects the default list; `[]` explicitly advertises no STUN / TURN servers and permits host candidates only.

The current parser accepts `stun:` and `turn:` URLs. TURN supports UDP / TCP; STUN URLs do not accept a `transport` parameter. `turns:` is not supported by the current native ICE path. It allows up to 16 entries, 32 URLs overall, and 8 distinct endpoints of each STUN / TURN kind. Invalid configuration prevents startup. See [the configuration issuer](../src/ice_server_config_issuer.cpp) for validation rules.

Clients advertising `ice_config_version: 1` receive a complete, expiring session configuration; its endpoints replace their local ICE list. The server also provides legacy temporary TURN credentials for compatible older clients. Configure backup STUN / TURN servers here so native and Web clients receive the same list. Recreate the signaling container after changing `.env`; changing endpoints does not migrate an already established media path.

<a id="clients"></a>

## Certificates and client setup

### Trust your server's certificate

Default files are stored under `${CROSSDESK_DATA_DIR}/certs/`:

| File | Purpose |
| --- | --- |
| `api.crossdesk.cn_root.crt` | Root certificate distributed to clients |
| `api.crossdesk.cn_bundle.crt` | Server certificate chain |
| `api.crossdesk.cn.key` | Server private key, retained on the server |

These are **filenames expected by the program**; your server does not need the `api.crossdesk.cn` domain. The [certificate generator](../docker/generate_certs.sh) includes only `EXTERNAL_IP` as an IP subject alternative name. Use that IP in clients. For a custom domain, supply a valid certificate chain covering that domain and its private key at the `bundle.crt` and `.key` paths listed above.

The [startup script](../docker/start.sh) reuses existing certificates. Editing the IP in `.env` does not reissue them. Regenerating self-signed certificates replaces the root certificate, which must then be trusted again on every client. A certificate issued by a system-trusted CA usually needs no separate root import.

Copy your deployment's `api.crossdesk.cn_root.crt` to each client and import it into the system trust store:

```powershell
# Windows: administrator PowerShell; substitute the actual file path
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

### Configure the controller and host

1. In the desktop client, open **Settings → General → Self-Hosted Config**.
2. Enter the server IP/hostname and signaling port `9099`, then confirm. The address field takes no URL scheme, path, or port. Current clients receive TURN endpoints from the server; there is no separate TURN-port field.
3. Enable **Self-Hosted Config** and click **Save** in the parent settings window. Configure both clients to use the same server.
4. After trusting the certificate, reconnect the clients and wait for a server connection. Use the host's currently displayed ID and password.

The desktop client uses the **system trust store** and has no certificate-file picker in this dialog. Recheck the device ID after switching servers. Current desktop builds enable SRTP automatically for browser interoperability.

- **Web:** set `window.CROSSDESK_CONFIG.signalingUrl` before loading the main script. Follow the [Web deployment guide](https://github.com/kunkundi/crossdesk-web-client/blob/HEAD/docs/DEPLOYMENT_EN.md#configuration); the browser must also trust the WSS certificate. Session ICE endpoints come from the server.
- **Native mobile clients:** configure the same signaling host and port in the app's server settings. Certificate handling depends on the platform; a system-trusted CA certificate is recommended, particularly on Android. See the [iOS](https://github.com/kunkundi/crossdesk/tree/HEAD/apps/ios) and [Android](https://github.com/kunkundi/crossdesk/tree/HEAD/apps/android) app guides for platform-specific setup.

<a id="stats"></a>

## Status API

`GET /stats` and `GET /api/stats` share the WSS HTTPS port, return aggregate statistics, allow cross-origin reads, and **do not require admin login**. Check from the server with the command below. Replace the example IP and adjust the root-certificate path if you changed the data directory:

```bash
curl --fail --cacert /var/lib/crossdesk/certs/api.crossdesk.cn_root.crt \
  https://203.0.113.10:9099/stats
```

For a domain with a trusted CA certificate:

```bash
curl --fail https://your-domain.example.com:9099/stats
```

| Field | Meaning |
| --- | --- |
| `online_device_count` | Total online native devices (PC + mobile), excluding `web-*`, `cli-*` and `C-*` |
| `online_pc_client_count` | Online PC clients, including native devices that have not reported a platform |
| `online_mobile_client_count` | Online iOS / Android clients, identified by their reported platform and excluding `web-*`, `cli-*` and `C-*` |
| `online_web_client_count` | Online `web-*` clients |
| `online_cli_client_count` | Online `cli-*` controllers |
| `active_connection_count` | Active remote-control connections, counted separately for each host–guest pair |
| `online_duration_seconds` | Sum of the current online periods of online devices |

Cumulative online, controlling, and controlled durations are no longer collected or returned. Legacy database columns remain unused for compatibility; new databases omit them. Current online and remote-session durations remain available.

Durations are in seconds. Detailed admin APIs use `/api/admin/*` and require admin login; the public status API does not return device details.

<a id="build"></a>

## Build from source

CI builds on **Ubuntu 22.04, amd64 / arm64**. Install Git, a C++17 toolchain, and [Xmake](https://xmake.io/guide/quick-start.html). Xmake downloads the C++ dependencies. These Linux dependency commands match the CI baseline:

```bash
sudo apt-get update
sudo apt-get install -y ca-certificates git curl unzip build-essential

git clone https://github.com/kunkundi/crossdesk-server.git
cd crossdesk-server
xmake f -c -m release -y
xmake b -vy crossdesk_server
```

Ensure `xmake --version` works before the first build. See [xmake.lua](../xmake.lua) for other platform configurations; the current release workflow provides only Linux binaries and images. Select Debug with `xmake f -m debug`; `xmake r -d crossdesk_server` runs through a debugger.

Credential queue regression and legacy WSS integration tests require Python 3, `websockets>=15` and the `openssl` command:

```bash
xmake b credential_queue_test credential_server_fixture
xmake r credential_queue_test
# Substitute the fixture path for your platform, architecture and build mode.
python3 tests/credential_server_integration.py \
  --fixture build/linux/x86_64/release/credential_server_fixture --clients 2100
```

The test creates isolated certificates and databases in a temporary directory. It checks source waits beyond 15 seconds, disconnect cancellation, expiry, burst login and another burst after restart. Every login performs real Argon2id verification with no client login retry. TCP/TLS handshake concurrency is capped at 64 to avoid overflowing the local listen queue first. The burst case raises the loopback source quota; separate cases enforce source limits. Results do not imply default admission of 2100 devices behind one IP or production-machine performance.

### Build a local runtime image

The Dockerfile **packages an already compiled Linux binary**. Complete a Release build on Ubuntu 22.04 with the same architecture as the target image, then run from the repository root:

```bash
# amd64; replace x86_64 with arm64 for an arm64 build
install -Dm755 build/linux/x86_64/release/crossdesk_server dist/crossdesk_server
sudo docker build -f docker/dockerfile -t crossdesk-server:local .
```

Copy `.env.example` to `.env` in the repository root, complete the deployment settings above, and set `CROSSDESK_IMAGE=crossdesk-server:local`. Then run:

```bash
sudo docker compose config -q
sudo docker compose pull coturn
sudo docker compose up -d --no-build
```

Direct binary execution requires the certificate, database, and log paths defined in [main.cpp](../src/main.cpp), including `/var/lib/crossdesk/certs`. `CROSSDESK_DATA_DIR` / `CROSSDESK_LOG_DIR` configure Compose host mounts; they do not override paths in the binary. The binary does not generate certificates.

### CI test image

Successful default-branch builds publish the multi-architecture `crossdesk/crossdesk-server:test` tag, which changes with subsequent builds. Test deployments can set `CROSSDESK_IMAGE=crossdesk/crossdesk-server:test` in `.env` and use the pull/start commands above. Use fixed release tags for production. See the [CI workflow](../.github/workflows/build.yml) for artifacts and image publishing.
