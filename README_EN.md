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
| Web admin dashboard | Online/daily activity metrics, client/session management, device-data queries, export, and cleanup |

Clients try P2P connections and use TURN when needed. The current [Compose deployment](compose.yaml) and [CI](.github/workflows/build.yml) target **Linux amd64 / arm64**. Release images use Ubuntu 22.04 as their runtime environment.

<a id="run-services"></a>

## Quick deployment

Use a **Linux amd64 / arm64** server with Docker Engine and the Compose plugin. The deployment uses host networking. Download `compose.yaml` and `env.example` from the **same [Release](https://github.com/kunkundi/crossdesk-server/releases)** into a dedicated directory, then run:

```sh
cp env.example .env
openssl rand -hex 32
openssl rand -hex 32
```

Edit `.env` before starting. Keep the fixed release image tag, set `EXTERNAL_IP` to the public IP and `INTERNAL_IP` to a local address Coturn can bind. Put the two different generated secrets in `COTURN_AUTH_SECRET` and `COTURN_STATELESS_NONCE_SECRET`. The source repository uses [`.env.example`](.env.example) instead of the release asset's `env.example`; its image defaults to `latest`, so select a fixed tag for deployment.

Open the configured ports in both the firewall and cloud security group:

| Default port | Protocol | Service |
| --- | --- | --- |
| `9099` | TCP | WSS signaling, HTTPS stats, and admin |
| `3478` | TCP / UDP | STUN / TURN |
| `50000`–`60000` | UDP | TURN relay |

From the configuration directory:

```sh
sudo docker compose config -q
sudo docker compose pull
sudo docker compose up -d --no-build
sudo docker compose ps
```

The first start creates certificates and a database. Then complete **certificate trust and client setup** below. Compose's health check only checks certificate files; verify HTTPS and an actual remote session too. The Compose port defaults to `9099`; the standalone binary defaults to `9090`.

[Full deployment guide](docs/DEPLOYMENT_EN.md) · [Custom ICE / TURN endpoints](docs/DEPLOYMENT_EN.md#ice)

<a id="clients"></a>

## Certificates and clients

The generated certificate covers only `EXTERNAL_IP`. Import your deployment's root certificate from `${CROSSDESK_DATA_DIR}/certs/api.crossdesk.cn_root.crt` into each client's trust store. For a custom domain, supply a matching certificate chain and key. Changing the IP in `.env` does not regenerate existing certificates.

In the desktop client, open **Settings → General → Self-Hosted Config**, enter the host and signaling port, confirm, enable the checkbox, and **Save**. Both peers must use the same server. TURN endpoints are supplied by the server. For Web, configure the WSS endpoint and browser certificate trust using the [Web guide](https://github.com/kunkundi/crossdesk-web-client/blob/HEAD/docs/DEPLOYMENT_EN.md#configuration).

[Certificate files, trust commands, and platform setup](docs/DEPLOYMENT_EN.md#clients)

<a id="admin"></a>

## Admin dashboard

Set **both** `ADMIN_USERNAME` and `ADMIN_PASSWORD` in `.env`, recreate the container with `sudo docker compose up -d --no-build`, then open `https://your-server:9099/admin`.

| Page | Purpose |
| --- | --- |
| Overview | Online PC/mobile/Web clients, daily active controllers, active sessions, filtering, and session disconnection |
| Data management | Exact-device queries, JSON summary export, and cleanup with password confirmation |
| About | Server version, license, and project links |

See the [admin guide](docs/ADMIN_EN.md) for metric definitions, recovery behavior, authentication limits, and data-operation scope. Disconnecting a session leaves its devices online.

<a id="stats"></a>

## Check service health

Replace the example IP and use your deployment's root certificate (adjust the path if needed):

```sh
curl --fail --cacert /var/lib/crossdesk/certs/api.crossdesk.cn_root.crt \
  https://203.0.113.10:9099/stats
```

`/stats` and `/api/stats` expose aggregate counts without admin authentication; they do not return device details. See [field definitions](docs/DEPLOYMENT_EN.md#stats). Test a client session to verify media and relay connectivity.

<a id="operations"></a>

## Maintenance

- Persist the data and log directories. Defaults are `/var/lib/crossdesk` and `/var/log/crossdesk`; application logs are written to files, with Compose container logging disabled.
- Before upgrades, back up `.env`, `compose.yaml`, certificates, and the database. Password hashes are upgraded after successful verification; an older server cannot read upgraded hashes, so rollback requires a pre-upgrade backup.
- Review retention and capacity for your deployment. Defaults include 185-day logs, 180-day offline-device cleanup, and 8,192 signaling connections; these are configuration defaults, not a throughput guarantee.

[Logs and upgrades](docs/OPERATIONS_EN.md) · [Authentication, recovery, capacity, retention, and backups](docs/OPERATIONS_EN.md#operations)

<a id="build"></a>

## Build from source

Use Git, a C++17 compiler, and Xmake. The release baseline is Ubuntu 22.04 on amd64 / arm64. The [build guide](docs/DEPLOYMENT_EN.md#build) includes dependency setup, existing regression tests, and image packaging; the Dockerfile packages an already compiled binary.

## Feedback and license

[Report issues](https://github.com/kunkundi/crossdesk-server/issues) with the server tag, both client versions, deployment method, and sanitized logs. CrossDesk Server uses [LGPL-3.0](LICENSE).
