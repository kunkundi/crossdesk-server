    const chinese = {
      'Language': '语言',
      'Logout': '退出登录',
      'Admin Login': '管理员登录',
      'Username': '用户名',
      'Password': '密码',
      'Login': '登录',
      'Last refresh:': '最近刷新：',
      'never': '尚未刷新',
      'Refresh lists': '刷新列表',
      'Online devices': '在线设备',
      'Web clients': '网页客户端',
      'Active sessions': '活动会话',
      'Online time': '累计在线时长',
      'Control time': '累计控制时长',
      'Controlled time': '累计被控时长',
      'Client Presence': '设备在线状态',
      'Search device ID': '搜索设备 ID',
      'Client category': '客户端类型',
      'PC': '电脑',
      'Web': '网页端',
      'Device sort': '设备排序',
      'Status': '状态',
      'Last seen': '最近活动',
      'Online since': '上线时间',
      'Current online': '本次在线',
      'Total online': '累计在线',
      'Total control': '累计控制',
      'Total controlled': '累计被控',
      'Device ID': '设备 ID',
      'Toggle sort order': '切换排序方向',
      'ASC': '升序',
      'DESC': '降序',
      'Ascending': '升序',
      'Descending': '降序',
      'Devices per page': '每页设备数',
      'Sessions per page': '每页会话数',
      '10 / page': '10 条 / 页',
      '50 / page': '50 条 / 页',
      '100 / page': '100 条 / 页',
      '200 / page': '200 条 / 页',
      'Device filters': '设备筛选',
      'Online': '在线',
      'Controlled': '被控中',
      'Devices currently being controlled; each device is counted once': '正在被控制的设备，每台设备只计一次',
      'Offline': '离线',
      'All': '全部',
      'Client': '客户端',
      'State': '状态',
      'Detail': '详情',
      'Previous': '上一页',
      'Next': '下一页',
      'Search session or user': '搜索会话或用户',
      'Transmission': '会话',
      'Participants': '参与者',
      'Action': '操作',
      'No records': '暂无记录',
      'Unknown': '未知',
      'Web client': '网页客户端',
      'PC client': '电脑客户端',
      'Remote': '远控中',
      'Hide': '收起',
      'Details': '详情',
      'Current control': '本次控制',
      'Current controlled': '本次被控',
      'Client IP': '客户端 IP',
      'Last online': '最近在线',
      'Active session': '活动会话',
      'Controlling': '正在控制',
      'Controlled by': '控制方',
      'Disconnect': '断开',
      'Invalid username or password': '用户名或密码错误',
      'Connection error': '连接失败，请重试',
      'Failed to disconnect session': '断开会话失败',
      'Failed to log out': '退出登录失败，请重试',
      'controlling {count}': '正在控制 {count} 台设备',
      'controlled by {count}': '有 {count} 个控制方',
      'host {id}': '被控端 {id}',
      '{start}-{end} of {total}': '第 {start}-{end} 条，共 {total} 条',
      'Disconnect session {id} for host {host}? Devices stay online.': '确定断开被控端 {host} 的会话 {id}？设备将保持在线。',
      '{days}d {hours}h': '{days}天 {hours}小时',
      '{hours}h {minutes}m': '{hours}小时 {minutes}分',
      '{minutes}m {seconds}s': '{minutes}分 {seconds}秒',
      '{seconds}s': '{seconds}秒'
    };
    const languageStorageKey = 'crossdesk-admin-language';
    let language = (navigator.language || '').toLowerCase().startsWith('zh') ? 'zh' : 'en';
    try {
      const saved = localStorage.getItem(languageStorageKey);
      if (saved === 'zh' || saved === 'en') language = saved;
    } catch (_) {}

    function locale() {
      return language === 'zh' ? 'zh-CN' : 'en-US';
    }

    function t(key, values = {}) {
      const text = language === 'zh' ? (chinese[key] || key) : key;
      return text.replace(/\{(\w+)\}/g, (match, name) => values[name] ?? match);
    }

    function translatePage() {
      document.documentElement.lang = locale();
      document.getElementById('language').value = language;
      for (const attribute of ['text', 'placeholder', 'title', 'aria-label']) {
        const dataAttribute = attribute === 'text' ? 'data-i18n' : `data-i18n-${attribute}`;
        document.querySelectorAll(`[${dataAttribute}]`).forEach(element => {
          const value = t(element.getAttribute(dataAttribute));
          if (attribute === 'text') element.textContent = value;
          else element.setAttribute(attribute, value);
        });
      }
    }

    function setMessage(id, key) {
      const element = document.getElementById(id);
      element.dataset.i18n = key;
      element.textContent = t(key);
    }

    function setLanguage(value) {
      language = value === 'zh' ? 'zh' : 'en';
      try { localStorage.setItem(languageStorageKey, language); } catch (_) {}
      translatePage();
      updateRefreshTime();
      applyDeviceKindCounts();
      renderDevices(currentDevices);
      renderSessions(currentSessions);
      updatePager('devices');
      updatePager('sessions');
      updateLiveDurations();
    }

    function updateRefreshTime() {
      document.getElementById('last-refresh').textContent = lastRefreshAt
        ? new Date(lastRefreshAt).toLocaleTimeString(locale()) : t('never');
    }

    const loginView = document.getElementById('login-view');
    const dashboardView = document.getElementById('dashboard-view');
    const logoutButton = document.getElementById('logout');
    const state = {
      devices: {
        limit: 10,
        offset: 0,
        total: 0,
        search: '',
        filter: 'online',
        kind: 'pc',
        sort: 'status',
        order: 'desc'
      },
      sessions: {limit: 10, offset: 0, total: 0, search: ''}
    };
    const searchTimers = {devices: null, sessions: null};
    const expandedDevices = new Set();
    let currentDevices = [];
    let devicesCapturedAt = 0;
    let currentSessions = [];
    let deviceKindCounts = null;
    let lastRefreshAt = 0;
    let listTimer = null;
    let durationTimer = null;
    let listRefreshSerial = 0;
    let listRefreshInFlight = false;
    let listRefreshPending = false;
    let statsSnapshot = {
      onlineDuration: 0,
      onlineCount: 0,
      controlDuration: 0,
      controlledDuration: 0,
      activeConnections: 0,
      capturedAt: 0
    };
    function showDashboard() {
      loginView.classList.add('hidden');
      dashboardView.classList.remove('hidden');
      logoutButton.classList.remove('hidden');
      refreshLists();
      if (!listTimer) listTimer = setInterval(refreshLists, 5000);
      if (!durationTimer) durationTimer = setInterval(updateLiveDurations, 1000);
    }

    function showLogin(message) {
      ++listRefreshSerial;
      listRefreshPending = false;
      dashboardView.classList.add('hidden');
      loginView.classList.remove('hidden');
      logoutButton.classList.add('hidden');
      if (listTimer) clearInterval(listTimer);
      listTimer = null;
      if (durationTimer) clearInterval(durationTimer);
      durationTimer = null;
      setMessage('login-error', message || '');
    }

    async function login(event) {
      event.preventDefault();
      const body = JSON.stringify({
        username: document.getElementById('username').value,
        password: document.getElementById('password').value
      });
      try {
        const response = await fetch('/api/admin/login', {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          credentials: 'same-origin',
          body
        });
        if (response.ok) showDashboard();
        else showLogin(response.status === 401 ? 'Invalid username or password' : 'Connection error');
      } catch (_) {
        showLogin('Connection error');
      }
    }

    async function logout() {
      try {
        const response = await fetch('/api/admin/logout', {method: 'POST', credentials: 'same-origin'});
        if (response.ok || response.status === 401) showLogin('');
        else setMessage('refresh-error', 'Failed to log out');
      } catch (_) {
        setMessage('refresh-error', 'Failed to log out');
      }
    }

    function formatTime(value) {
      if (!value) return '-';
      return new Date(value * 1000).toLocaleString(locale());
    }

    function formatDuration(value) {
      let seconds = Number(value) || 0;
      if (seconds < 0) seconds = 0;
      const days = Math.floor(seconds / 86400);
      seconds %= 86400;
      const hours = Math.floor(seconds / 3600);
      seconds %= 3600;
      const minutes = Math.floor(seconds / 60);
      seconds = Math.floor(seconds % 60);
      if (days > 0) return t('{days}d {hours}h', {days, hours});
      if (hours > 0) return t('{hours}h {minutes}m', {hours, minutes});
      if (minutes > 0) return t('{minutes}m {seconds}s', {minutes, seconds});
      return t('{seconds}s', {seconds});
    }

    function appendEmptyRow(body, colSpan) {
      const row = document.createElement('tr');
      const cell = document.createElement('td');
      cell.className = 'empty';
      cell.colSpan = colSpan;
      cell.textContent = t('No records');
      row.appendChild(cell);
      body.appendChild(row);
    }

    function appendText(parent, tag, value, className) {
      const element = document.createElement(tag);
      if (className) element.className = className;
      element.textContent = value;
      parent.appendChild(element);
      return element;
    }

    function labelCell(cell, label) {
      cell.dataset.label = t(label);
      return cell;
    }

    function appendBadge(parent, value, className) {
      return appendText(parent, 'span', value, `badge ${className}`);
    }

    function setDurationDataset(cell, kind, device, base, capturedAt, running, rate) {
      const isRunning = typeof running === 'boolean' ? running : device.online;
      cell.dataset.duration = kind;
      cell.dataset.online = device.online ? '1' : '0';
      cell.dataset.running = isRunning ? '1' : '0';
      cell.dataset.base = String(base || 0);
      cell.dataset.capturedAt = String(capturedAt);
      cell.dataset.rate = String(rate || 1);
    }

    function sessionSummary(device) {
      const targets = Array.isArray(device.active_control_targets) ? device.active_control_targets : [];
      const controlledBy = Array.isArray(device.active_controlled_by) ? device.active_controlled_by : [];
      const controlling = Number(device.active_control_count) || targets.length;
      const controlled = Number(device.active_controlled_count) || controlledBy.length;
      const parts = [];
      if (controlling > 0) parts.push(t('controlling {count}', {count: controlling}));
      if (controlled > 0) parts.push(t('controlled by {count}', {count: controlled}));
      return parts.join(', ') || '-';
    }

    function peerList(value) {
      return Array.isArray(value) && value.length ? value.join(', ') : '-';
    }

    function activeDuration(value, activeCount) {
      return Number(activeCount) > 0 ? formatDuration(value) : '-';
    }

    function platformLabel(platform) {
      if (platform === 'windows') return 'Windows';
      if (platform === 'macos') return 'macOS';
      if (platform === 'linux') return 'Linux';
      return platform || '';
    }

    function appendDetailItem(parent, label, value, className, dataset) {
      const item = document.createElement('div');
      appendText(item, 'span', t(label));
      const strong = appendText(item, 'strong', value, className);
      if (dataset) {
        Object.keys(dataset).forEach(key => {
          strong.dataset[key] = dataset[key];
        });
      }
      parent.appendChild(item);
      return strong;
    }

    function renderDevices(devices, capturedAt = devicesCapturedAt) {
      currentDevices = devices;
      devicesCapturedAt = capturedAt;
      const body = document.getElementById('devices');
      const fragment = document.createDocumentFragment();
      if (!devices.length) {
        appendEmptyRow(fragment, 4);
        body.replaceChildren(fragment);
        return;
      }
      devices.forEach(device => {
        const activeSessions = Number(device.active_session_count) || 0;
        const isExpanded = expandedDevices.has(device.id);
        const row = document.createElement('tr');
        row.className = isExpanded ? 'device-row expanded' : 'device-row';
        const clientCell = document.createElement('td');
        labelCell(clientCell, 'Client');
        appendText(clientCell, 'div', device.id, 'device-id');
        const clientMeta = document.createElement('div');
        clientMeta.className = 'client-meta';
        appendText(clientMeta, 'span', t(device.kind === 'web' ? 'Web client' : 'PC client'), 'subline');
        if (device.kind !== 'web' && device.client_platform) {
          appendBadge(clientMeta, platformLabel(device.client_platform), 'platform');
        }
        if (device.kind !== 'web' && device.client_version) {
          appendBadge(clientMeta, device.client_version, 'version');
        }
        clientCell.appendChild(clientMeta);
        row.appendChild(clientCell);

        const statusCell = document.createElement('td');
        labelCell(statusCell, 'State');
        appendBadge(statusCell, t(device.online ? 'Online' : 'Offline'),
          device.online ? 'online' : 'offline');
        if (activeSessions > 0) appendBadge(statusCell, t('Remote'), 'active');
        row.appendChild(statusCell);

        const currentCell = appendText(row, 'td', device.online ? formatDuration(device.online_duration_seconds) : '-');
        labelCell(currentCell, 'Current online');
        setDurationDataset(currentCell, 'current', device, device.online_duration_seconds, capturedAt);
        const detailCell = document.createElement('td');
        detailCell.className = 'detail-action';
        labelCell(detailCell, 'Detail');
        const detailButton = document.createElement('button');
        detailButton.type = 'button';
        detailButton.textContent = t(isExpanded ? 'Hide' : 'Details');
        detailButton.addEventListener('click', () => {
          if (expandedDevices.has(device.id)) expandedDevices.delete(device.id);
          else expandedDevices.add(device.id);
          renderDevices(currentDevices);
        });
        detailCell.appendChild(detailButton);
        row.appendChild(detailCell);
        fragment.appendChild(row);

        if (isExpanded) {
          const detailsRow = document.createElement('tr');
          detailsRow.className = 'details-row';
          const detailsCell = document.createElement('td');
          detailsCell.colSpan = 4;
          const details = document.createElement('div');
          details.className = 'detail-grid';
          const currentOnline = appendDetailItem(
            details, 'Current online',
            device.online ? formatDuration(device.online_duration_seconds) : '-');
          setDurationDataset(currentOnline, 'current-online', device,
            device.online_duration_seconds, capturedAt);
          const totalOnline = appendDetailItem(
            details, 'Total online', formatDuration(device.total_online_seconds));
          setDurationDataset(totalOnline, 'total', device, device.total_online_seconds, capturedAt);
          const activeControlCount = Number(device.active_control_count) || 0;
          const activeControlledCount = Number(device.active_controlled_count) || 0;
          const currentControl = appendDetailItem(
            details, 'Current control',
            activeDuration(device.current_control_seconds, activeControlCount));
          setDurationDataset(currentControl, 'current-control', device,
            device.current_control_seconds, capturedAt,
            activeControlCount > 0, activeControlCount);
          const totalControl = appendDetailItem(
            details, 'Total control', formatDuration(device.total_control_seconds));
          setDurationDataset(totalControl, 'total-control', device,
            device.total_control_seconds, capturedAt,
            activeControlCount > 0, activeControlCount);
          const currentControlled = appendDetailItem(
            details, 'Current controlled',
            activeDuration(device.current_controlled_seconds, activeControlledCount));
          setDurationDataset(currentControlled, 'current-controlled', device,
            device.current_controlled_seconds, capturedAt,
            activeControlledCount > 0, activeControlledCount);
          const totalControlled = appendDetailItem(
            details, 'Total controlled', formatDuration(device.total_controlled_seconds));
          setDurationDataset(totalControlled, 'total-controlled', device,
            device.total_controlled_seconds, capturedAt,
            activeControlledCount > 0, activeControlledCount);
          appendDetailItem(details, 'Client IP', device.client_ip || '-');
          appendDetailItem(details, 'Online since', formatTime(device.online_since));
          appendDetailItem(details, 'Last online', formatTime(device.online ? 0 : device.updated_at));
          appendDetailItem(details, 'Active session', sessionSummary(device));
          appendDetailItem(details, 'Controlling', peerList(device.active_control_targets), 'peer-list');
          appendDetailItem(details, 'Controlled by', peerList(device.active_controlled_by), 'peer-list');
          detailsCell.appendChild(details);
          detailsRow.appendChild(detailsCell);
          fragment.appendChild(detailsRow);
        }
      });
      body.replaceChildren(fragment);
      updateLiveDurations();
    }

    function renderSessions(sessions) {
      currentSessions = sessions;
      const body = document.getElementById('sessions');
      const fragment = document.createDocumentFragment();
      if (!sessions.length) {
        appendEmptyRow(fragment, 3);
        body.replaceChildren(fragment);
        return;
      }
      sessions.forEach(session => {
        const guests = session.guest_ids.join(', ') || '-';
        const row = document.createElement('tr');
        row.className = 'session-row';
        const transmissionCell = document.createElement('td');
        labelCell(transmissionCell, 'Transmission');
        appendText(transmissionCell, 'div', session.transmission_id);
        appendText(transmissionCell, 'span', t('host {id}', {id: session.host_id}), 'muted');
        row.appendChild(transmissionCell);

        const participantsCell = document.createElement('td');
        labelCell(participantsCell, 'Participants');
        appendText(participantsCell, 'div', session.participant_count);
        appendText(participantsCell, 'span', guests, 'muted');
        row.appendChild(participantsCell);

        const actionCell = document.createElement('td');
        labelCell(actionCell, 'Action');
        const button = document.createElement('button');
        button.className = 'danger';
        button.textContent = t('Disconnect');
        button.dataset.id = session.transmission_id;
        button.dataset.host = session.host_id;
        button.addEventListener('click', () => disconnectSession(button.dataset.id, button.dataset.host, button));
        actionCell.appendChild(button);
        row.appendChild(actionCell);
        fragment.appendChild(row);
      });
      body.replaceChildren(fragment);
    }

    function buildOverviewUrl() {
      const params = new URLSearchParams();
      params.set('device_limit', state.devices.limit);
      params.set('device_offset', state.devices.offset);
      params.set('device_filter', state.devices.filter);
      params.set('device_kind', state.devices.kind);
      params.set('device_sort', state.devices.sort);
      params.set('device_order', state.devices.order);
      params.set('session_limit', state.sessions.limit);
      params.set('session_offset', state.sessions.offset);
      if (state.devices.search) params.set('device_search', state.devices.search);
      if (state.sessions.search) params.set('session_search', state.sessions.search);
      return `/api/admin/overview?${params.toString()}`;
    }

    function syncPage(pageState, pageData) {
      if (!pageData) return false;
      pageState.limit = pageData.limit;
      pageState.offset = pageData.offset;
      pageState.total = pageData.total;
      if (pageState.total > 0 && pageState.offset >= pageState.total && pageState.limit > 0) {
        pageState.offset = Math.floor((pageState.total - 1) / pageState.limit) * pageState.limit;
        return true;
      }
      return false;
    }

    function updatePager(kind) {
      const page = state[kind];
      const prefix = kind === 'devices' ? 'device' : 'session';
      const start = page.total === 0 ? 0 : Math.min(page.offset + 1, page.total);
      const end = Math.min(page.offset + page.limit, page.total);
      document.getElementById(`${prefix}-page-info`).textContent = t('{start}-{end} of {total}', {start, end, total: page.total});
      document.getElementById(`${prefix}-prev`).disabled = page.offset === 0;
      document.getElementById(`${prefix}-next`).disabled = page.offset + page.limit >= page.total;
      document.getElementById(`${prefix}-limit`).value = String(page.limit);
      if (kind === 'devices') {
        document.getElementById('device-kind').value = page.kind;
        document.getElementById('device-sort').value = page.sort;
        document.getElementById('device-order').textContent = t(page.order === 'asc' ? 'ASC' : 'DESC');
        document.getElementById('device-order').title = t(page.order === 'asc' ? 'Ascending' : 'Descending');
      }
    }

    function applyDeviceCounts(counts) {
      if (counts) {
        ['all', 'online', 'offline', 'active'].forEach(filter => {
          const count = Number(counts[filter]) || 0;
          const countElement = document.getElementById(`device-count-${filter}`);
          if (countElement) countElement.textContent = count;
        });
      }
      document.querySelectorAll('[data-device-filter]').forEach(button => {
        button.classList.toggle('active', button.dataset.deviceFilter === state.devices.filter);
        button.setAttribute('aria-selected', button.dataset.deviceFilter === state.devices.filter ? 'true' : 'false');
      });
    }

    function applyDeviceKindCounts(counts) {
      const kindSelect = document.getElementById('device-kind');
      if (!kindSelect) return;
      if (counts) deviceKindCounts = counts;
      counts = deviceKindCounts;
      const labels = {pc: t('PC'), web: t('Web')};
      Array.from(kindSelect.options).forEach(option => {
        const value = option.value;
        const count = counts && Object.prototype.hasOwnProperty.call(counts, value)
          ? Number(counts[value]) || 0
          : null;
        option.textContent = count === null ? labels[value] : `${labels[value]} ${count}`;
      });
      kindSelect.value = state.devices.kind;
    }

    function applyStats(stats) {
      if (stats.server_version) document.getElementById('server-version').textContent = stats.server_version;
      document.getElementById('metric-devices').textContent = stats.online_device_count;
      document.getElementById('metric-web').textContent = stats.online_web_client_count;
      document.getElementById('metric-sessions').textContent = stats.active_connection_count;
      document.getElementById('metric-duration').textContent = formatDuration(stats.total_online_seconds);
      document.getElementById('metric-control').textContent = formatDuration(stats.total_control_seconds);
      document.getElementById('metric-controlled').textContent = formatDuration(stats.total_controlled_seconds);
      lastRefreshAt = Date.now();
      updateRefreshTime();
      statsSnapshot = {
        onlineDuration: Number(stats.total_online_seconds) || 0,
        onlineCount: Number(stats.online_device_count) || 0,
        controlDuration: Number(stats.total_control_seconds) || 0,
        controlledDuration: Number(stats.total_controlled_seconds) || 0,
        activeConnections: Number(stats.active_connection_count) || 0,
        capturedAt: Math.floor(Date.now() / 1000)
      };
    }

    function updateLiveDurations() {
      const now = Math.floor(Date.now() / 1000);
      document.querySelectorAll('[data-duration]').forEach(cell => {
        if (cell.dataset.running !== '1') return;
        const base = Number(cell.dataset.base) || 0;
        const capturedAt = Number(cell.dataset.capturedAt) || now;
        const rate = Number(cell.dataset.rate) || 1;
        cell.textContent = formatDuration(base + rate * (now - capturedAt));
      });
      if (statsSnapshot.capturedAt > 0) {
        const elapsed = now - statsSnapshot.capturedAt;
        document.getElementById('metric-duration').textContent =
          formatDuration(statsSnapshot.onlineDuration +
            statsSnapshot.onlineCount * elapsed);
        document.getElementById('metric-control').textContent =
          formatDuration(statsSnapshot.controlDuration +
            statsSnapshot.activeConnections * elapsed);
        document.getElementById('metric-controlled').textContent =
          formatDuration(statsSnapshot.controlledDuration +
            statsSnapshot.activeConnections * elapsed);
      }
    }

    async function refreshStats() {
      if (document.hidden) return true;
      let response;
      try {
        response = await fetch('/api/admin/stats', {credentials: 'same-origin'});
      } catch (_) {
        setMessage('refresh-error', 'Connection error');
        return false;
      }
      if (response.status === 401) {
        showLogin('');
        return false;
      }
      if (!response.ok) {
        setMessage('refresh-error', 'Connection error');
        return false;
      }
      const data = await response.json();
      setMessage('refresh-error', '');
      applyStats(data.stats);
      return true;
    }

    async function refreshLists() {
      if (document.hidden || dashboardView.classList.contains('hidden')) return;
      if (listRefreshInFlight) {
        listRefreshPending = true;
        return;
      }
      listRefreshInFlight = true;
      const serial = ++listRefreshSerial;
      const refreshButton = document.getElementById('list-refresh');
      refreshButton.disabled = true;
      try {
        await loadLists(serial);
      } catch (_) {
        if (serial === listRefreshSerial && !dashboardView.classList.contains('hidden')) {
          setMessage('refresh-error', 'Connection error');
        }
      } finally {
        listRefreshInFlight = false;
        refreshButton.disabled = false;
        if (listRefreshPending && !dashboardView.classList.contains('hidden')) {
          listRefreshPending = false;
          void refreshLists();
        }
      }
    }

    async function loadLists(serial) {
      const requestedUrl = buildOverviewUrl();
      const response = await fetch(requestedUrl, {
        credentials: 'same-origin',
        signal: AbortSignal.timeout(12000)
      });
      if (serial !== listRefreshSerial || dashboardView.classList.contains('hidden')) {
        return;
      }
      if (response.status === 401) {
        showLogin('');
        return;
      }
      if (!response.ok) {
        throw new Error('Failed to refresh lists');
      }
      const data = await response.json();
      if (serial !== listRefreshSerial || requestedUrl !== buildOverviewUrl() || dashboardView.classList.contains('hidden')) {
        return;
      }
      setMessage('refresh-error', '');
      applyStats(data.stats);
      if (data.devices_page && data.devices_page.kind) {
        state.devices.kind = data.devices_page.kind;
      }
      const reloadDevices = syncPage(state.devices, data.devices_page);
      const reloadSessions = syncPage(state.sessions, data.sessions_page);
      if (reloadDevices || reloadSessions) {
        listRefreshPending = true;
        return;
      }
      applyDeviceCounts(data.device_counts);
      applyDeviceKindCounts(data.device_kind_counts);
      renderDevices(data.devices || [], Math.floor(Date.now() / 1000));
      renderSessions(data.sessions || []);
      updatePager('devices');
      updatePager('sessions');
    }

    async function disconnectSession(id, host, button) {
      if (!confirm(t('Disconnect session {id} for host {host}? Devices stay online.', {id, host}))) return;
      button.disabled = true;
      try {
        const response = await fetch(`/api/admin/sessions/${encodeURIComponent(id)}/disconnect`, {
          method: 'POST',
          credentials: 'same-origin'
        });
        if (response.ok) refreshLists();
        else setMessage('refresh-error', 'Failed to disconnect session');
      } catch (_) {
        setMessage('refresh-error', 'Failed to disconnect session');
      } finally {
        button.disabled = false;
      }
    }

    document.getElementById('language').addEventListener('change', event => setLanguage(event.target.value));
    document.getElementById('login-form').addEventListener('submit', login);
    document.getElementById('logout').addEventListener('click', logout);
    document.getElementById('device-search').addEventListener('input', (event) => {
      state.devices.search = event.target.value.trim();
      state.devices.offset = 0;
      clearTimeout(searchTimers.devices);
      searchTimers.devices = setTimeout(refreshLists, 250);
    });
    document.querySelectorAll('[data-device-filter]').forEach(button => {
      button.addEventListener('click', () => {
        state.devices.filter = button.dataset.deviceFilter;
        state.devices.offset = 0;
        expandedDevices.clear();
        applyDeviceCounts();
        refreshLists();
      });
    });
    document.getElementById('device-kind').addEventListener('change', (event) => {
      state.devices.kind = event.target.value === 'web' ? 'web' : 'pc';
      state.devices.offset = 0;
      expandedDevices.clear();
      applyDeviceKindCounts();
      refreshLists();
    });
    document.getElementById('device-sort').addEventListener('change', (event) => {
      state.devices.sort = event.target.value;
      state.devices.offset = 0;
      refreshLists();
    });
    document.getElementById('device-order').addEventListener('click', () => {
      state.devices.order = state.devices.order === 'asc' ? 'desc' : 'asc';
      state.devices.offset = 0;
      refreshLists();
    });
    document.getElementById('session-search').addEventListener('input', (event) => {
      state.sessions.search = event.target.value.trim();
      state.sessions.offset = 0;
      clearTimeout(searchTimers.sessions);
      searchTimers.sessions = setTimeout(refreshLists, 250);
    });
    document.getElementById('device-limit').addEventListener('change', (event) => {
      state.devices.limit = Number(event.target.value);
      state.devices.offset = 0;
      refreshLists();
    });
    document.getElementById('session-limit').addEventListener('change', (event) => {
      state.sessions.limit = Number(event.target.value);
      state.sessions.offset = 0;
      refreshLists();
    });
    document.getElementById('device-prev').addEventListener('click', () => {
      state.devices.offset = Math.max(0, state.devices.offset - state.devices.limit);
      refreshLists();
    });
    document.getElementById('device-next').addEventListener('click', () => {
      if (state.devices.offset + state.devices.limit < state.devices.total) {
        state.devices.offset += state.devices.limit;
        refreshLists();
      }
    });
    document.getElementById('session-prev').addEventListener('click', () => {
      state.sessions.offset = Math.max(0, state.sessions.offset - state.sessions.limit);
      refreshLists();
    });
    document.getElementById('session-next').addEventListener('click', () => {
      if (state.sessions.offset + state.sessions.limit < state.sessions.total) {
        state.sessions.offset += state.sessions.limit;
        refreshLists();
      }
    });
    document.getElementById('list-refresh').addEventListener('click', refreshLists);
    document.addEventListener('visibilitychange', () => {
      if (!document.hidden && !dashboardView.classList.contains('hidden')) {
        refreshLists();
      }
    });
    translatePage();
    updateRefreshTime();
    applyDeviceCounts();
    applyDeviceKindCounts();
    refreshStats().then((ok) => {
      if (ok) showDashboard();
      else showLogin('');
    }).catch(() => showLogin(''));
