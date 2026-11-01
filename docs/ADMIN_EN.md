# Admin dashboard guide

[Project overview](../README_EN.md) · [中文](ADMIN.md)

[Enable the dashboard](#admin) · [Risk controls](#device-risk-controls) · [Daily activity](#daily-active-controllers) · [Authentication](#refresh-and-authentication) · [Device data requests](#device-data-requests)

<a id="admin"></a>

## Admin dashboard

Set both `ADMIN_USERNAME` and `ADMIN_PASSWORD` in `.env`, then run `sudo docker compose up -d --no-build` again. Setting only one does not enable the dashboard. Open **`https://server-address:9099/admin`**, trust the certificate, and log in with that account.

| Area / control | Action |
| --- | --- |
| Top metrics | View separate online PC, mobile (iOS / Android), and Web client counts, plus today’s and yesterday’s daily active controllers; active connections are counted separately |
| Client Presence | Defaults to online PCs; switch PC/Mobile/Web; mobile clients are identified by their reported `ios` / `android` platform; combine platform, version and Online/Controlled/Offline/All filters, search IDs, sort, and paginate; platform/version options cover all reported values in the current category, with counts and pagination following the filters; Controlled counts and lists only devices currently being controlled, once per device |
| Details | Expand platform/version, current online/control durations and remote peers |
| Active Sessions → Disconnect | Confirm to disconnect the selected session; devices stay online |
| Refresh lists / Logout | Refresh manually / sign out |

The top navigation includes **Overview** (`/admin/overview`) for metrics, clients and active sessions; **Data management** (`/admin/data`) for querying, exporting and cleaning device records; **Risk controls** (`/admin/risk`) for password restrictions; and **About** (`/admin/about`) for the server version, project description, license and project links. Each page supports direct links, reloads and browser back/forward navigation. If authentication is required, signing in opens the originally requested page. `/admin` continues to show the overview by default.

### Device risk controls

Open **Risk controls** (`/admin/risk`) to inspect device-ID and source-IP password failure counts, thresholds, remaining waits and manual reasons. The page refreshes every 5 seconds and supports search, type/source filters and pagination; summary counts follow the filters.

- **Add restriction**: choose a device ID or source IP, duration and optional reason. Adding the same target updates its expiry and reason. Durations range from 1 minute to 30 days, with at most 4,096 active manual rules. IPv4 and IPv6 addresses are supported; network ranges are not.
- **Clear restriction / record**: remove that target's manual rule and reset its automatic failures, cooldown and backoff. In-flight checks retain their concurrency reservations, but checks already running at clear time cannot restore old failure counts. Device and IP state are independent: clearing a device does not clear restrictions on its source IP.
- **Scope**: password authentication only, including legacy password login, initial key binding for an existing ID, and remote-control queries/joins. A device rule targets the device whose password is being checked, not a claimed controller identity; use the original ID without `C-`. IP rules use the TCP peer address, not a client-supplied address.

Bound-device signature login, valid legacy reconnect credentials, existing sessions and administrator login remain unaffected. Manual rules persist in SQLite across restarts and stop applying at expiry. Automatic counts and backoff are process-local, reset on restart, and are not shared across instances.

The authenticated API exposes `GET /api/admin/risk` with `search`, `scope=all|device|ip`, `kind=all|manual|auto` and `offset` (50 rows per page). Manual and automatic state for the same target are merged and classified as manual when a manual rule exists. `POST /api/admin/risk` requires JSON and `X-CrossDesk-CSRF` from `/api/admin/data-session`:

```json
{"action":"restrict","scope":"device","target":"123456789","duration_seconds":3600,"reason":"Repeated password attempts"}
```

Clear with `{"action":"clear","scope":"device","target":"123456789"}`; use `scope: "ip"` for an IP rule. Successful actions log the administrator, action, scope and target, without passwords or reason text. Rules are checked before queue admission, before password computation and before authentication completes, so previously queued requests cannot bypass newly added restrictions.

This page supports incident response and recovery. New failures can trigger limits again after a clear; it does not automatically stop sustained attacks against legacy IDs. IP restrictions can affect other devices behind the same NAT. Use it alongside public-key device login and source-level response.

### Daily active controllers

Days follow **UTC+8**, starting at midnight regardless of the host timezone. Successful remote-session joins count toward the metric. Native controllers are deduplicated by device ID, with `C-` identities merged into the original device; Web controllers use temporary `web-*` IDs, so obtaining a new ID can count again. Login, presence, and failed authentication do not count. This measures clients, not unique people or confirmed media transmission.

Ending a session preserves that day's count. A maintenance task runs about every 5 seconds and counts confirmed sessions that span midnight into the new day. Counts survive restarts. Recovering sessions contribute to a new day only after rejoining or confirmation by both peers. Legacy recovery uses saved relationships and both peers logging in within the recovery window; this neither proves media is flowing nor grants new control permissions. Credential-aware sessions require both peers to report that the connection is alive. Unconfirmed sessions remain recovering, excluded from active connections and new daily counts until recovery or expiry.

Collection starts when the feature is enabled: earlier dates display `—`, and the first day is marked partial. Only today and yesterday are retained. Admin `/api/admin/stats` and `/api/admin/overview` expose counts and coverage in `stats.daily_active_controllers`; the public stats API keeps its existing fields.

### Refresh and authentication

On wider screens, device and session lists appear side by side with search and pagination kept visible. Mobile layouts keep search and client-type selection available, group platform/version/sort controls under filters, and show compact records or cards with touch-friendly actions.

While visible, the overview refreshes data every 5 seconds and updates displayed durations every second. Data management and About do not poll device or session lists. Admin sessions are held in memory, expire after 8 hours by default, and require a new login after a server restart. Dashboard login cookies require HTTPS.

Admin login starts a 15-minute cooldown after 5 failures from one source IP or 20 failures across all sources within 15 minutes. Wrong usernames and passwords share these budgets. Changing usernames, cookies or connections does not reset failures, and successful logins do not clear unexpired failures. During cooldown, credentials are not verified and no session cookie is issued; the API returns `429 too_many_login_attempts`, a `Retry-After` header and JSON `retry_after` in seconds. The login page displays the wait time. Rejected requests do not extend cooldown, and existing admin sessions remain valid.

The source is the TCP peer IP; `X-Forwarded-For` and source fields in request bodies are not trusted. Users behind one NAT or reverse proxy share a source quota. Admin and device-password limits are independent. Counters are local to the server process and reset on restart; multiple instances need coordinated limits in a trusted gateway or shared store.

Transport peer IPs are used in memory for connection handling and authentication rate limits. They are not returned through the admin API, displayed in the dashboard or recorded in new signaling connection logs. The server does not query external IP geolocation services or display regional distributions. Historical logs and logs from the separate Coturn service retain their respective retention policies.

Frontend source lives in [src/admin/web](../src/admin/web), installed at `/crossdesk-server/admin` in the container. A custom `CROSSDESK_ADMIN_WEB_DIR` must also be explicitly passed through Compose's `environment` and its directory mounted. Adding it to `.env` alone does not pass it into the container. Restart the service after frontend changes to refresh its asset cache.

Upgrading requires deploying the new server binary/image and matching admin assets. Remove obsolete `CROSSDESK_GEOIP_*` variables and the unused API key from your deployment environment. Existing database files remain compatible; legacy location columns, if present, are no longer read or written.

### Device data requests

Open **Data management → Device data requests** and enter the original device ID without its `C-` prefix, plus a request reference containing no personal information, such as `REQ-2026-001`. References allow ASCII letters, digits, `-`, and `_`, up to 64 characters. An exact-ID query covers the device and its `C-` identity: presence, platform/version, association counts, today/yesterday activity, and active/recovering session counts.

Queries and JSON exports omit passwords, salts, hashes, and other device IDs. An export is a current database summary, not a full connection history or database backup: sessions include only this device's role and start time, while associations are represented by counts.

Export and cleanup require the **admin password again** and confirmation that the requester and scope have been checked. This checkbox records a manual check; the system does not prove device ownership. A public device ID or shared remote-access password alone does not establish the right to request data.

Query a preview before cleanup, select a scope, and re-enter the device ID:

- **History only:** remove presence/platform/version, historical associations involving the device or its controller identity, and its daily activity contributions. Preserve the device ID and password digest so existing access credentials remain valid.
- **History and identity:** also remove the registration, salt, and hash. The old identity becomes invalid; subsequent registration may assign a new ID. Continued use creates new records.

Both identities must be offline with no active or recovering session. Disconnecting a session does not log the device out; exit its client or close its signaling connection first. Otherwise the API returns `409 device_busy`. Cleanup is serialized with signaling writes and rechecks state and the preview version inside a transaction. Changes since the preview return `409 stale_preview`; query again. Associated in-memory subscriptions, network information, and password-change result caches are also removed without changing other devices' identities or counts.

`GET /api/admin/data-session` obtains the session CSRF token. `POST /api/admin/device-data/query|export|cleanup` requires JSON, a valid admin session, and `X-CrossDesk-CSRF`; export/cleanup also verify the admin password. Five failed password checks pause verification for one minute across all admin sessions. Parameterized SQL, transactions, and preview checks protect cleanup. Operations reject more than 10,000 related associations or sessions rather than truncating results or partially deleting. A one-second SQL execution budget causes a failed operation to roll back; query state again before retrying.

Successful queries, exports, and cleanup commit with an `admin_data_audit` entry in the same transaction. If audit writing fails, data is not returned and cleanup is not committed. Audits include time, administrator, request reference, action, scope, and deletion counts, excluding device IDs, IPs, credentials, and data snapshots. They expire according to `CROSSDESK_LOG_RETENTION_DAYS` (185 days by default). Operators manage verification materials separately using the request reference.

**Scope:** exports exclude legacy location fields, runtime subscriptions, logs, and backups. Cleanup removes matching database rows, including any legacy location fields within them, but does not scan logs, backups, snapshots, external log copies, or client-local records. Handle those copies separately when required; a successful cleanup does not mean every copy is erased. Exports download to the administrator's browser without a separate server-side export file. Verify the recipient before delivery and store downloads appropriately.
