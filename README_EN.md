# CrossDesk Server

Device registration, presence, remote-session signaling, and temporary TURN credentials for [CrossDesk](https://github.com/kunkundi/crossdesk), with a built-in Web admin dashboard. Signaling uses WSS; device records, presence, and active sessions are persisted in SQLite.

[中文](README.md) · [Release files](https://github.com/kunkundi/crossdesk-server/releases) · [Docker images](https://hub.docker.com/r/crossdesk/crossdesk-server/tags) · [Desktop client](https://github.com/kunkundi/crossdesk) · [Web client](https://github.com/kunkundi/crossdesk-web-client)

[![Build](https://github.com/kunkundi/crossdesk-server/actions/workflows/build.yml/badge.svg)](https://github.com/kunkundi/crossdesk-server/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/kunkundi/crossdesk-server)](https://github.com/kunkundi/crossdesk-server/releases)
[![Docker Pulls](https://img.shields.io/docker/pulls/crossdesk/crossdesk-server)](https://hub.docker.com/r/crossdesk/crossdesk-server)
[![License: LGPL v3](https://img.shields.io/badge/license-LGPL--3.0-blue)](LICENSE)

[Deployment](#run-services) · [Client setup](#clients) · [Admin dashboard](#admin) · [Status API](#stats) · [Maintenance](#operations) · [Build](#build)

## Components and capabilities

| Component | Responsibility |
| --- | --- |
| CrossDesk Server | WSS signaling, device identities and password verification, presence, session coordination, and temporary TURN credentials |
| Coturn | Relays media and data when clients cannot connect directly; runs in a separate container |
| SQLite | Stores device records, presence, and active session information |
| Web admin dashboard | Metrics, client/session lists, search, filters, sorting, session disconnection |

Clients try P2P connections and use TURN when needed. The current [Compose deployment](compose.yaml) and [CI](.github/workflows/build.yml) target **Linux amd64 / arm64**. Release images use Ubuntu 22.04 as their runtime environment.

## Run services

### 1. Prepare the deployment files

Use a Linux server with **Docker Engine and the Compose plugin** installed, and confirm that `docker compose version` works. Compose uses host networking; `INTERNAL_IP` must be an address the host can bind.

Download **`compose.yaml`** and **`env.example`** from the same [GitHub Release](https://github.com/kunkundi/crossdesk-server/releases) into a dedicated deployment directory. Run there:

```bash
cp env.example .env
```

The release's `env.example` pins the corresponding image tag. The source repository instead provides [`.env.example`](.env.example), which uses `latest`; choose a fixed release tag when using that file. Run all subsequent Compose commands from the configuration directory.

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

Optional `COTURN_PUBLIC_HOST` advertises a separate TURN hostname/IP; an empty value uses `EXTERNAL_IP`. `COTURN_CREDENTIAL_TTL_SECONDS` defaults to `3600` seconds, with a range of `60`–`86400`. `COTURN_LOG_LEVEL` defaults to `warning`, and `COTURN_MEMORY_LIMIT` to `512m`. See the [environment example](.env.example) for all options.

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

<a id="clients"></a>

## Certificates and client setup

### Trust your server's certificate

Default files are stored under `${CROSSDESK_DATA_DIR}/certs/`:

| File | Purpose |
| --- | --- |
| `api.crossdesk.cn_root.crt` | Root certificate distributed to clients |
| `api.crossdesk.cn_bundle.crt` | Server certificate chain |
| `api.crossdesk.cn.key` | Server private key, retained on the server |

These are **filenames expected by the program**; your server does not need the `api.crossdesk.cn` domain. The [certificate generator](docker/generate_certs.sh) includes only `EXTERNAL_IP` as an IP subject alternative name. Use that IP in clients. For a custom domain, supply a valid certificate chain covering that domain and its private key at the `bundle.crt` and `.key` paths listed above.

The [startup script](docker/start.sh) reuses existing certificates. Editing the IP in `.env` does not reissue them. Regenerating self-signed certificates replaces the root certificate, which must then be trusted again on every client. A certificate issued by a system-trusted CA usually needs no separate root import.

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

1. Open **☰ → Settings → Self-Hosted Config** in the desktop client.
2. Enter the server IP/hostname, signaling port `9099`, and relay port `3478`, then confirm. The address field takes no URL scheme, path, or port.
3. Enable the **Self-Hosted Config** checkbox and click **OK** in the parent settings window. Configure both clients to use the same server.
4. After trusting the certificate, reopen the clients and wait for a server connection. Connect using the host's currently displayed ID and password.

The current desktop client verifies certificates through the **system trust store**; there is no certificate-file picker in the self-hosting dialog. Recheck the device ID after switching servers.

- **Web:** configure `signalingUrl` (for example, `wss://203.0.113.10:9099`) and the STUN address (for example, `stun:203.0.113.10:3478`) in the [Web client's](https://github.com/kunkundi/crossdesk-web-client) `web_client.js`. Replace the example IP, trust the certificate in the browser, and keep SRTP enabled on the host.
- **Native iOS:** enter the signaling host, signaling port, and STUN/TURN port under Settings → Server and apply. Install and trust the root certificate on the device for a self-signed deployment.

<a id="admin"></a>

## Admin dashboard

Set both `ADMIN_USERNAME` and `ADMIN_PASSWORD` in `.env`, then run `sudo docker compose up -d --no-build` again. Setting only one does not enable the dashboard. Open **`https://server-address:9099/admin`**, trust the certificate, and log in with that account.

| Area / control | Action |
| --- | --- |
| Top metrics | View online devices, Web clients, and active connections |
| Client Presence | Defaults to online PCs; switch PC/Web, filter Online/Controlled/Offline/All, search IDs, sort, and paginate; Controlled counts and lists only devices currently being controlled, once per device |
| Details | Expand platform/version, current online/control durations, connection IP and remote peers |
| Active Sessions → Disconnect | Confirm to disconnect the selected session; devices stay online |
| Refresh lists / Logout | Refresh manually / sign out |

While visible, the page refreshes data every 5 seconds and updates displayed durations every second. Admin sessions are held in memory, expire after 8 hours by default, and require a new login after a server restart. Dashboard login cookies require HTTPS.

Connection IPs are held in memory while devices are online and cleared on logout. Connection logs can still contain IPs. The server no longer queries external IP geolocation services or displays regional distributions.

Frontend source lives in [src/admin/web](src/admin/web), installed at `/crossdesk-server/admin` in the container. A custom `CROSSDESK_ADMIN_WEB_DIR` must also be explicitly passed through Compose's `environment` and its directory mounted. Adding it to `.env` alone does not pass it into the container. Restart the service after frontend changes to refresh its asset cache.

Upgrading requires deploying the new server binary/image and matching admin assets. Remove obsolete `CROSSDESK_GEOIP_*` variables and the unused API key from your deployment environment. Existing database files remain compatible; legacy location columns, if present, are no longer read or written.

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
| `online_device_count` | Online devices, excluding `web-*` and `C-*` |
| `online_web_client_count` | Online `web-*` clients |
| `active_connection_count` | Active remote-control connections, counted separately for each host–guest pair |
| `online_duration_seconds` | Sum of the current online periods of online devices |

Cumulative online, controlling, and controlled durations are no longer collected or returned. Legacy database columns remain unused for compatibility; new databases omit them. Current online and remote-session durations remain available.

Durations are in seconds. Detailed admin APIs use `/api/admin/*` and require admin login; the public status API does not return device details.

<a id="operations"></a>

## Maintenance and troubleshooting

### Logs and upgrades

Run from the configuration directory:

```bash
sudo docker compose ps
# Default log directory; substitute your CROSSDESK_LOG_DIR when customized.
sudo sh -c 'tail -n 100 /var/log/crossdesk/crossdesk-server_$(date +%F).log'
sudo sh -c 'tail -n 100 /var/log/crossdesk/coturn/turn_$(date +%F).log'
```

Back up before upgrading. Change the fixed image tag in `.env` and use deployment files matching that release:

```bash
sudo docker compose config -q
sudo docker compose pull
sudo docker compose up -d --no-build
```

`docker compose restart` does not apply changed environment variables or images. Signaling logs append to `${CROSSDESK_LOG_DIR}/crossdesk-server_YYYY-MM-DD.log`; Coturn uses `coturn/turn_YYYY-MM-DD.log` in that directory. Filenames follow the process local date (UTC in the default containers); select an existing date file when viewing logs. The new Compose configuration disables Docker log copies so size-only rotation cannot bypass retention. `docker compose logs` no longer serves these logs. To diagnose startup failures, stop the original signaling container to avoid a port conflict, then use `sudo docker compose run --rm --service-ports crossdesk-server` to see attached startup output.

Connection diagnostics help investigate a running process that cannot reliably accept clients:

| Log | Key fields |
| --- | --- |
| `Connection capacity` | Configured/effective connection limits, `fd_soft_limit`, and reserved FDs at startup; a separate warning reports capacity reduced by nofile |
| `Connection admission paused / resumed` | `blocked_by_*` distinguishes total connections, unopened connections, FD capacity, and FD sampling failure; includes pause duration and retry checks |
| `Connection diagnostics` | First emitted after approximately 5 seconds, then every 60 seconds and on shutdown; summarizes connections, FDs, backlogs, and cumulative errors |
| `Signal event loop delayed` | Reports maintenance timer delays of at least 5 seconds |

Warnings for admission pauses, accept/resource errors, peer address failures, pre-open failures, and event loop delays are limited to one per category per 30 seconds. Recovery records are paired only with reported pauses. `*_total` counters accumulate since process startup even when warnings are suppressed; `*_limit_checks_total` counts checks hitting a limit, not rejected clients. `unopened` includes WebSocket handshakes, ordinary HTTPS requests, and their cleanup. `fd_open` is measured when logging; `fd_estimated` is the admission estimate, with its sample age in `fd_sample_age_ms`. A value of `-1` means unavailable or unlimited. `expired_handles` and `oldest_unopened_ms` help identify retained or stalled connections.

### Retention and automatic cleanup

| Data | Default policy |
| --- | --- |
| Signaling and Coturn operational/connection logs | Delete closed files **185 days** after their last write; daily files are not overwritten by a size cap |
| Device presence and version/platform | Delete the device and its controller clone together after both have been offline for more than **180 days**; skip identities that are online, were used within the last 180 days or are referenced by active/recoverable sessions |
| Historical server-side device associations | Remove with expired devices; other stale associations without current owner presence expire after **180 days**; legacy rows without timestamps start aging at first upgrade |
| Device IDs, password salts and hashes | Delete with expired devices in the same transaction; legacy credentials without presence timestamps start aging at first upgrade |
| Current IPs, subscriptions, admin sessions | Existing logout/disconnect/session-expiry rules continue to apply; device expiry also clears its in-memory subscriptions and caches; connection logs may contain IPs |

Configure `.env` with `CROSSDESK_LOG_RETENTION_DAYS=185`, `CROSSDESK_OFFLINE_RETENTION_DAYS=180` and `CROSSDESK_RETENTION_INTERVAL_SECONDS=3600`. Days must be 1–3650 and the interval 60–86400 seconds. Empty values use defaults; invalid values prevent startup rather than enabling immediate deletion or indefinite retention. Check applicable minimum retention obligations before shortening mixed operational/security logs. These defaults do not certify legal compliance.

The first pass runs about five seconds after the server starts, then hourly by default. Each transaction selects at most 200 expired devices (including their controller clones) and 200 other stale associations. A backlog triggers another batch about five seconds later; failures roll back and retry. Offline age starts at the last disconnect; records exactly at the cutoff are retained. New credentials without presence start aging at registration; an admin history-only cleanup starts a fresh retention period for retained credentials. Completion depends on load and uptime. Whole-file expiry can retain the earliest entries for roughly one additional day plus the check interval. Cleanup pauses while the service is stopped and resumes after restart.

Log cleanup only handles recognized files in the configured log directory, including legacy `crossdesk-server-YYYYMMDD-HHMMSS[.N].log` files and `coturn/turn_YYYY-MM-DD.log`. Cleanup does not recurse into unrelated directories, follow symlinks or delete the current signaling log. Use a dedicated directory for each deployment and monitor free disk space and retention completion/failure logs. Failure to initialize file logging prevents server startup. After device expiry, the old device ID and password are no longer valid; returning devices follow the new-device registration flow.

SQLite uses `secure_delete` and attempts a nonblocking WAL checkpoint after deletion. This does not securely erase snapshots, backups or underlying storage. Operators must separately expire backups, external log stores and old container log copies; the server never scans or deletes them. Recreate containers when upgrading to apply the new logging configuration and handle old copies according to applicable retention obligations. Update the public privacy policy only once the production deployment actually applies these rules.

### Backups and migration

Back up `.env`, `compose.yaml`, and `certs/` plus `db/` under `${CROSSDESK_DATA_DIR}`. These backups include keys and the device database. This example uses the default data directory and stops services for a consistent archive; remote connections are interrupted during the backup:

```bash
sudo docker compose stop
backup_file="crossdesk-backup-$(date +%Y%m%d-%H%M%S).tar.gz"
sudo tar -czf "$backup_file" .env compose.yaml -C /var/lib/crossdesk certs db
sudo docker compose up -d --no-build
```

For migration, restore the matching files on the destination host, check mount paths and permissions, then start. A changed public IP/hostname also requires updating certificates and client addresses. For older deployments with custom paths, point `CROSSDESK_DATA_DIR` and `CROSSDESK_LOG_DIR` at the existing directories.

### Common issues

| Symptom | Check |
| --- | --- |
| Configuration validation fails | Required IPs and both secrets are set, and files belong to the same release |
| Certificate generation / database writes fail | Signaling logs and writable mount paths; the default image starts as root, so adjust ownership for the actual UID/GID only when changing container users or similar settings |
| Coturn cannot bind | `INTERNAL_IP` belongs to a host interface and the ports are available |
| Client TLS errors | System root trust, validity dates, and matching IP/hostname; editing the IP does not update existing certificates |
| Server connected, remote device offline | Both clients use the same service, the host is running, and its current ID was copied |
| P2P and relay both fail | Client relay setting/version, matching TURN shared secrets, and relay/media firewall ports |
| Dashboard disabled / login fails | Both admin variables reached the container, the container was recreated, and the browser uses HTTPS |

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

Ensure `xmake --version` works before the first build. See [xmake.lua](xmake.lua) for other platform configurations; the current release workflow provides only Linux binaries and images. Select Debug with `xmake f -m debug`; `xmake r -d crossdesk_server` runs through a debugger.

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

Direct binary execution requires the certificate, database, and log paths defined in [main.cpp](src/main.cpp), including `/var/lib/crossdesk/certs`. `CROSSDESK_DATA_DIR` / `CROSSDESK_LOG_DIR` configure Compose host mounts; they do not override paths in the binary. The binary does not generate certificates.

### CI test image

Successful default-branch builds publish the multi-architecture `crossdesk/crossdesk-server:test` tag, which changes with subsequent builds. Test deployments can set `CROSSDESK_IMAGE=crossdesk/crossdesk-server:test` in `.env` and use the pull/start commands above. Use fixed release tags for production. See the [CI workflow](.github/workflows/build.yml) for artifacts and image publishing.

## Feedback and license

[Report issues](https://github.com/kunkundi/crossdesk-server/issues) with the server tag, both client versions, deployment method, and sanitized logs. CrossDesk Server uses [LGPL-3.0](LICENSE).
