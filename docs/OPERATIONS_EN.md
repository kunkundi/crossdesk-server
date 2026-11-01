# Operations and troubleshooting

[Project overview](../README_EN.md) · [中文](OPERATIONS.md)

[Logs and upgrades](#logs-and-upgrades) · [Passwords](#device-password-protection) · [Reconnect credentials](#dedicated-reconnect-credentials) · [Capacity](#online-capacity-and-restart-recovery) · [Retention](#retention-and-automatic-cleanup) · [Backups](#backups-and-migration) · [Troubleshooting](#common-issues)

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

Warnings for admission pauses, accept/resource errors, pre-open failures, and event loop delays are limited to one per category per 30 seconds. Recovery records are paired only with reported pauses. `*_total` counters accumulate since process startup even when warnings are suppressed; `*_limit_checks_total` counts checks hitting a limit, not rejected clients. `unopened` includes WebSocket handshakes, ordinary HTTPS requests, and their cleanup. `fd_open` is measured when logging; `fd_estimated` is the admission estimate, with its sample age in `fd_sample_age_ms`. A value of `-1` means unavailable or unlimited. `expired_handles` and `oldest_unopened_ms` help identify retained or stalled connections.

### Device password protection

Device passwords remain **6 alphanumeric characters**, generated using OpenSSL `RAND_priv_bytes()` with rejection sampling. Each credential gets an independent 16-byte salt from `RAND_bytes()`. Unavailable randomness or Argon2id fails startup/the operation; there is no fallback to a weak PRNG or SHA-256.

Registration and password changes use **Argon2id v19 with 19 MiB memory, 2 iterations, parallelism 1 and a 32-byte digest**. The existing `password_hash` column contains the `argon2id$v=19$m=19456,t=2,p=1$` prefix and a hex digest; `password_salt` contains a hex salt. Legacy salted SHA-256 records migrate atomically after successful verification without changing the ID or six-character password. Unused legacy records remain subject to device retention. Rehashing does not invalidate old passwords or leaked backups; rotate passwords through the client when necessary. Older servers cannot verify upgraded records; rollback requires a pre-upgrade backup.

Two dedicated workers perform password calculations by default, with a bounded queue scheduled round-robin by source IP. The defaults allow 4096 outstanding jobs, including running jobs and results awaiting commit, and 256 per source. Waiting jobs occupy no hashing worker. Hashing holds neither the database nor the failure-limiter lock. Messages stay ordered per connection; completion rechecks the connection identity and credential snapshot, so stale work cannot overwrite concurrent password changes or restore deleted identities. Missing devices, wrong legacy passwords and malformed records also run one Argon2id calculation to reduce timing differences. Startup checks the actual OpenSSL provider and parameters.

Registration, login, queries, joins and new password-change calculations share a source budget: by default, 60 starts per IP per minute, including successful operations. There is no longer a process-wide 300-per-minute quota. A token is consumed only after a worker becomes available; enqueueing, canceling disconnected waiters and rejecting a full queue consume no token. Rate-limited sources wait on the server without blocking unrelated sources. Queue overflow or expiry returns the existing `Credential service busy` response with `retry_after`. Failure cooldowns remain unchanged.

Accepted authentication requests have a default 600-second lifetime and are exempt from the ordinary 15-second unauthenticated-connection timeout while pending. Connections that never submit an accepted request still use the 15-second rule. Legacy clients receive the original login response after waiting, without a new protocol or client-side retry. The 600 seconds is a maximum, not a fixed delay. Disconnected waiters are canceled. The restart recovery window for remote-session records also extends with the authentication lifetime so records survive queued reconnects.

Six-character passwords still have limited strength: source budgets and the failure cooldown below constrain online guessing, while slow hashing raises the cost of offline guessing after a database leak.

Each WebSocket connection allows one successful login. Subsequent `login` requests return `Already authenticated` without registering devices, verifying passwords or updating presence. Open a new connection to log in again; an initial failed login can still be retried on the same connection.

`query_user_id_list` and `join_transmission` require a logged-in connection; joining also checks that the requester's identity matches its connection binding. Wrong passwords and unknown device IDs return the same failure, without a device list or connection credentials. Login and queries use `Authentication failed`; joins use the compatibility reasons below.

Password login (including the first public-key binding for an existing ID), queries and joins share failure counts. Within a 15-minute window, 20 failures from one source IP or 5 failures against one target device start a **1-minute** cooldown when the threshold failure completes. Each actual failed verification after cooldown doubles that dimension's delay: **1, 2, 4, 8, 16, 32, 64, 128, 240 minutes**, capped at **4 hours**. Cooldown expiry does not grant another batch of guesses. Source and target counts are independent; the longer remaining delay applies.

During cooldown, password verification is skipped even for correct passwords; responses include `Too many authentication attempts` (joins use the compatibility reason below), `error_code: "AUTHENTICATION_THROTTLED"` and `retry_after` in seconds. Blocked requests neither escalate nor extend the delay. After cooldown, each source/target permits only one in-flight password verification, preventing parallel retries from obtaining extra guesses. Below-threshold counts expire with the original 15-minute window. Once backoff starts, counts and escalation persist until **30 minutes after the latest cooldown ends**, provided no further verification fails. Successful verification neither clears failure history nor extends its retention; reconnecting, changing controller IDs or switching endpoints cannot reset backoff.

Requests received during an existing cooldown return the throttling response immediately, without entering the credential queue or spending a password-computation source token. Already queued requests recheck cooldown immediately before password computation, so failures occurring while they wait cannot be bypassed.

Use the admin [risk controls](ADMIN_EN.md#device-risk-controls) to add expiring device-ID/IP password restrictions and inspect or clear automatic limits. Manual rules persist; signature login remains available. Clearing a record never bypasses password verification and does not prevent new failures from triggering limits again.

Existing remote-control sessions and public-key-bound signature logins are unaffected. Attackers can still force unbound legacy IDs into longer waits, including delaying their first key binding. Key binding protects only the device's own login: remote-control Join and queries still share password failure counts, so even a key-bound device can remain online while an attacker temporarily prevents new password-authenticated connections. Source-IP cooldowns can also affect other devices behind the same NAT. Administrator login uses an independent limiter and retains its fixed 15-minute cooldown.

Join failures preserve reasons recognized by released desktops: wrong passwords and unknown IDs both return `Incorrect password` with `error_code: "AUTHENTICATION_FAILED"`. An empty-password Join (`ID@`) returns `Incorrect password` / `PASSWORD_REQUIRED` without looking up the target, verifying a password or consuming credential/failure budgets. It grants no access and never clears or extends a cooldown. New clients should collect a password before joining.

Other Join rejections retain the legacy `Remote unavailable` reason so old clients stop waiting (their UI displays offline). New clients must prefer `error_code`: `AUTHENTICATION_THROTTLED`, `CREDENTIAL_SERVICE_BUSY`, `CREDENTIAL_RATE_LIMITED`, `NOT_AUTHENTICATED`, `REMOTE_UNAVAILABLE`, displaying and honoring `retry_after` when present. Authentication diagnostics count error codes; empty-password prompts are not authentication failures. Login and query failures retain their generic reason.

The source is the TCP peer IP without its port; request fields and `X-Forwarded-For` are not trusted. Connections behind one NAT or reverse proxy share a source quota. Counters are local to one server process and reset on restart; multiple instances need coordinated limits in a trusted gateway or shared store. Each dimension holds at most 16,384 records and rejects new verification keys at capacity until expired records free space.

Registration must explicitly use an empty device ID (native clients), `web` (Web clients), or `cli` (CLI controllers). Logging in with an unknown or expired ID returns the generic authentication failure instead of assigning a replacement ID. Clients must reset their local identity to register again.

Each CLI startup/new connection sends `{"type":"login","user_id":"cli"}`. A successful reply is `{"type":"login","status":"success","user_id":"cli-xxxxxxxxx"}`, with nine random digits and no local password or persistent reconnect credential. Join uses that ID on the same connection. Web and CLI temporary identities can only control other devices: they cannot host transmissions, change a local password, or log in again using an assigned ID. WebSocket disconnect removes their temporary registration; presence, subscriptions and control sessions follow the Web cleanup path. Offline statistics retain the existing retention policy.

Upgrade the server before using the new CLI. The CLI does not fall back to registering a native device on older servers.

### Dedicated reconnect credentials

Native clients opt in with `reconnect_version: 1` during registration or password login. A successful response includes `reconnect_token` (32 cryptographically random bytes encoded as 64 lowercase hexadecimal characters) and `reconnect_expires_at` (Unix seconds). Each device has at most one credential with an absolute 30-day lifetime. SQLite stores only its SHA-256 digest, allowing validation after server restarts. Clients that omit the opt-in retain the existing protocol.

Subsequent reconnects send only the device ID, without its password. Existing session recovery and ICE capability fields can be included:

```json
{"type":"login","user_id":"123456789","reconnect_version":1,"reconnect_token":"<64 lowercase hexadecimal characters>","session_resume_version":1,"ice_config_version":1}
```

Success returns `status: "success"`, `reconnected: true`, `reconnect_version: 1` and the original expiry, followed by normal presence updates, session recovery and a fresh heartbeat interval. This path bypasses the Argon2id queue and password source budget without reading or changing password failure counters. Connection and message queue limits still apply. Reconnects neither rotate the credential nor extend its lifetime, so a lost response can be retried.

Credentials authenticate only the device itself; access to another device still requires the existing password/session authorization. Empty, `web`, `cli`, `web-*`, `cli-*` and `C-*` temporary identities do not receive credentials. Another opted-in password login replaces the credential; changing the device password, deleting the device or reaching expiry invalidates it. An authenticated device can revoke its own credential with `{"type":"revoke_reconnect_credential"}`; success returns the same `type` and `status: "success"` without closing active connections.

Invalid credentials receive `Authentication failed` with `reconnect_rejected: true`, without implicit password work. MiniRTC clears the rejected credential and retries password login once on the same connection; a second failure follows existing error handling. Credentials stay in memory for the lifetime of the client connection instance and survive network outages/server restarts. Exiting the app, recreating the instance, switching servers or changing identity requires password authentication; no new plaintext credential file is written. New clients keep using password authentication with older servers.

`Connection diagnostics` adds `reconnect_success_total` and `reconnect_failed_total`. Successful reconnects also count toward `login_success_total` without increasing password work in `Credential diagnostics`.

### Online capacity and restart recovery

Configure these options in `.env` and recreate the signaling container:

| Setting | Default | Effect and range |
| --- | --- | --- |
| `CROSSDESK_MAX_CONNECTIONS` | `8192` | Total signaling connections, 1–65536; includes devices, `C-*` controllers and unopened connections |
| `CROSSDESK_NOFILE` | `32768` | Compose container file-descriptor limit; used and reserved FDs reduce effective capacity |
| `CROSSDESK_AUTH_WORKERS` | `2` | Concurrent password workers, 1–32; controls CPU, memory and authentication throughput |
| `CROSSDESK_AUTH_QUEUE_CAPACITY` | `4096` | Total outstanding requests, 1–16384; absorbs bursts without increasing calculation speed |
| `CROSSDESK_AUTH_SOURCE_CAPACITY` | `256` | Outstanding requests per source IP, 1–4096; prevents one source from filling the queue |
| `CROSSDESK_AUTH_SOURCE_PER_MINUTE` | `60` | Starts per IP per minute, 1–6000; shared behind NAT or a reverse proxy |
| `CROSSDESK_AUTH_TIMEOUT_SECONDS` | `600` | Maximum accepted authentication lifetime, 15–900 seconds |

Keeping 2000 devices online does not require 2000 authentication threads. Only operations that verify passwords use these workers. Start with two, measure throughput on the deployment machine, and increase only with CPU headroom for TLS, heartbeats and database work. Each active Argon2id calculation needs approximately 19 MiB of algorithm memory; waiting requests do not allocate it. Password-based burst recovery time is approximately the request count divided by measured completion throughput, also constrained by source quotas. Choose a deadline covering that recovery time. Shared-egress deployments need source capacity, source rate and deadlines considered together.

Existing `.env` settings override new defaults: an existing `CROSSDESK_MAX_CONNECTIONS=2048` remains 2048. The requested TCP listen backlog is 4096, subject to the OS limit, while concurrent unopened connections remain capped at 128. These settings cover signaling and reconnect recovery; sustained online operation, concurrent remote sessions and TURN bandwidth still need tests on the deployment machine.

`Credential capacity` logs effective authentication settings. `Credential diagnostics` reports `active`, `waiting`, `queue_full_total`, `source_full_total`, `expired_total`, `source_rate_waits_total` and calculation timing every minute. `Connection diagnostics` also counts successful logins, authentication failures, busy responses, failure-cooldown responses and authentication timeouts. Persistent queue growth means arrivals exceed processing throughput; increasing workers alone cannot resolve source-rate waits.

### Retention and automatic cleanup

| Data | Default policy |
| --- | --- |
| Signaling and Coturn operational/connection logs | Delete closed files **185 days** after their last write; daily files are not overwritten by a size cap |
| Device presence and version/platform | Delete the device and its controller clone together after both have been offline for more than **180 days**; skip identities that are online, were used within the last 180 days or are referenced by active/recoverable sessions |
| Historical server-side device associations | Remove with expired devices; other stale associations without current owner presence expire after **180 days**; legacy rows without timestamps start aging at first upgrade |
| Device IDs, password salts and hashes | Delete with expired devices in the same transaction; legacy credentials without presence timestamps start aging at first upgrade |
| Daily active controller records | Retain only today and yesterday in UTC+8; history/identity cleanup removes matching contributions, while ordinary disconnection does not |
| Admin data-operation audits | Expire from creation time according to `CROSSDESK_LOG_RETENTION_DAYS` (**185 days** by default), in batches |
| Subscriptions and admin sessions | Existing logout/disconnect/session-expiry rules continue to apply; device expiry also clears its in-memory subscriptions |

Configure `.env` with `CROSSDESK_LOG_RETENTION_DAYS=185`, `CROSSDESK_OFFLINE_RETENTION_DAYS=180` and `CROSSDESK_RETENTION_INTERVAL_SECONDS=3600`. Days must be 1–3650 and the interval 60–86400 seconds. Empty values use defaults; invalid values prevent startup rather than enabling immediate deletion or indefinite retention. Check applicable minimum retention obligations before shortening mixed operational/security logs. These defaults do not certify legal compliance.

The first pass runs about five seconds after the server starts, then hourly by default. Each transaction selects at most 200 expired devices (including their controller clones) and 200 other stale associations. A backlog triggers another batch about five seconds later; failures roll back and retry. Offline age starts at the last disconnect; records exactly at the cutoff are retained. New credentials without presence start aging at registration; an admin history-only cleanup starts a fresh retention period for retained credentials. Completion depends on load and uptime. Whole-file expiry can retain the earliest entries for roughly one additional day plus the check interval. Cleanup pauses while the service is stopped and resumes after restart.

Log cleanup only handles recognized files in the configured log directory, including legacy `crossdesk-server-YYYYMMDD-HHMMSS[.N].log` files and `coturn/turn_YYYY-MM-DD.log`. Cleanup does not recurse into unrelated directories, follow symlinks or delete the current signaling log. Use a dedicated directory for each deployment and monitor free disk space and retention completion/failure logs. Failure to initialize file logging prevents server startup. After device expiry, the old device ID and password are no longer valid; returning devices must reset their local identity and register with an empty device ID.

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
