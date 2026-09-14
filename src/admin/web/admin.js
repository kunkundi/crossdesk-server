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
      'User distribution in China': '中国用户分布',
      'Total users': '总用户',
      'China user distribution map': '中国用户分布地图',
      'CrossDesk uses IP2Location.io': 'CrossDesk 使用 IP2Location.io',
      'IP geolocation': 'IP 地理位置查询',
      'web service.': '服务。',
      'Users in China': '国内用户',
      'Users outside China': '国外用户',
      'In China': '国内用户',
      'Outside China': '国外用户',
      'Unresolved': '未解析',
      'User regions': '用户地域范围',
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
      'Location status': '位置状态',
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
      'Location': '位置',
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
      'Region': '地区',
      'Country': '国家或地区',
      'Last online': '最近在线',
      'Active session': '活动会话',
      'Controlling': '正在控制',
      'Controlled by': '控制方',
      'Disconnect': '断开',
      'Invalid username or password': '用户名或密码错误',
      'Connection error': '连接失败，请重试',
      'Failed to disconnect session': '断开会话失败',
      'Failed to log out': '退出登录失败，请重试',
      'Map data could not be loaded': '地图数据加载失败',
      'Fewest users first': '按人数从少到多',
      'Most users first': '按人数从多到少',
      '{scope}, {order}; click to switch to {nextOrder}': '{scope}当前{order}，点击切换为{nextOrder}',
      '{count} users': '{count} 位用户',
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
      hideGeoTooltip();
      renderGeoDistribution(state.geo.distribution);
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
      sessions: {limit: 10, offset: 0, total: 0, search: ''},
      geo: {
        list: 'domestic',
        provinceOrder: 'desc',
        countryOrder: 'desc',
        distribution: null
      }
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
    const MAP_NS = 'http://www.w3.org/2000/svg';
    const CHINA_MAP_URL = '/admin/assets/china-provinces.json';
    const MAP_WIDTH = 760;
    const MAP_HEIGHT = 560;
    const MAP_PADDING = 24;
    const GEO_COLORS = ['#eef4f7', '#d7edf2', '#a9dce7', '#6fc1d3', '#2f95bd', '#1264a3'];
    const PROVINCE_LABEL_OFFSETS = {
      beijing: [0, -9],
      tianjin: [20, 12],
      shanghai: [20, 4],
      chongqing: [-12, 12],
      hongkong: [22, 16],
      macau: [-22, 18],
      hainan: [10, 16]
    };
    let geoMapReady = false;
    let geoMapDataPromise = null;
    let geoFeatures = [];
    let geoFeatureByKey = new Map();
    let geoProvinceCounts = new Map();
    let geoTotalUsers = 0;

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

    function formatPercent(count, total) {
      if (!total) return '0%';
      return `${((Number(count) || 0) * 100 / total).toFixed(1)}%`;
    }

    function geoColor(count, maxCount) {
      const value = Number(count) || 0;
      if (value <= 0 || maxCount <= 0) return GEO_COLORS[0];
      const index = Math.max(1, Math.ceil((value / maxCount) * (GEO_COLORS.length - 1)));
      return GEO_COLORS[Math.min(index, GEO_COLORS.length - 1)];
    }

    function provinceKey(feature) {
      return feature && feature.properties ? feature.properties.key : '';
    }

    function provinceName(feature) {
      if (!feature || !feature.properties) return '';
      if (language === 'zh') return feature.properties.name;
      const key = provinceKey(feature);
      const names = {hongkong: 'Hong Kong', macau: 'Macau'};
      return names[key] || key.replace(/_/g, ' ').replace(/\b\w/g, letter => letter.toUpperCase());
    }

    function provinceTooltipText(feature) {
      const count = geoProvinceCounts.get(provinceKey(feature)) || 0;
      return `${provinceName(feature)}: ${count} (${formatPercent(count, geoTotalUsers)})`;
    }

    function provinceDisplayName(key) {
      const province = geoFeatureByKey.get(key);
      return province ? provinceName(province) : key;
    }

    function currentGeoListMode() {
      return state.geo.list === 'foreign' ? 'foreign' : 'domestic';
    }

    function currentGeoListOrder() {
      return currentGeoListMode() === 'foreign' ? state.geo.countryOrder : state.geo.provinceOrder;
    }

    function setCurrentGeoListOrder(order) {
      if (currentGeoListMode() === 'foreign') state.geo.countryOrder = order;
      else state.geo.provinceOrder = order;
    }

    function syncGeoListControls() {
      const mode = currentGeoListMode();
      document.querySelectorAll('[data-geo-list]').forEach(button => {
        const active = button.dataset.geoList === mode;
        button.classList.toggle('active', active);
        button.setAttribute('aria-selected', active ? 'true' : 'false');
      });

      const button = document.getElementById('geo-list-order');
      if (!button) return;
      const isAsc = currentGeoListOrder() === 'asc';
      const label = t(mode === 'foreign' ? 'Users outside China' : 'Users in China');
      button.textContent = '';
      button.classList.toggle('ascending', isAsc);
      button.title = t(isAsc ? 'Fewest users first' : 'Most users first');
      button.setAttribute('aria-label', t('{scope}, {order}; click to switch to {nextOrder}', {
        scope: label, order: t(isAsc ? 'Ascending' : 'Descending'),
        nextOrder: t(isAsc ? 'Descending' : 'Ascending')
      }));
    }

    function renderGeoUserList(data) {
      const list = document.getElementById('geo-user-list') || document.getElementById('geo-top-provinces');
      if (!list) return;
      syncGeoListControls();
      list.textContent = '';
      const isForeign = currentGeoListMode() === 'foreign';
      const direction = currentGeoListOrder() === 'asc' ? 1 : -1;
      let geoItems = (Array.isArray(isForeign ? data.countries : data.provinces)
          ? (isForeign ? data.countries : data.provinces) : [])
        .map(item => ({
          key: typeof (isForeign ? item.country : item.province) === 'string'
            ? (isForeign ? item.country : item.province).trim() : '',
          count: Number(item.count) || 0
        }))
        .filter(item => item.key && item.count > 0)
        .sort((lhs, rhs) => {
          if (lhs.count !== rhs.count) return (lhs.count - rhs.count) * direction;
          const lhsName = isForeign ? lhs.key : provinceDisplayName(lhs.key);
          const rhsName = isForeign ? rhs.key : provinceDisplayName(rhs.key);
          const nameCompare = lhsName.localeCompare(rhsName, locale());
          return currentGeoListOrder() === 'asc' ? nameCompare : -nameCompare;
        });
      if (isForeign && !geoItems.length) {
        const foreignCount = Number(data.foreign_count) || 0;
        if (foreignCount > 0) geoItems = [{key: t('Users outside China'), count: foreignCount}];
      }
      if (!geoItems.length) {
        const item = document.createElement('li');
        item.textContent = '-';
        list.appendChild(item);
        return;
      }
      geoItems.forEach(item => {
        const li = document.createElement('li');
        const label = isForeign ? item.key : provinceDisplayName(item.key);
        li.textContent = `${label}: ${item.count} (${formatPercent(item.count, geoTotalUsers)})`;
        list.appendChild(li);
      });
    }

    function moveGeoTooltip(event) {
      const wrap = document.querySelector('.china-map-wrap');
      const tooltip = document.getElementById('geo-tooltip');
      if (!wrap || !tooltip) return;
      const rect = wrap.getBoundingClientRect();
      if (typeof event.clientX !== 'number' || typeof event.clientY !== 'number') {
        tooltip.style.left = '10px';
        tooltip.style.top = '10px';
        return;
      }
      const left = Math.max(0, Math.min(event.clientX - rect.left, rect.width - 220));
      const top = Math.max(0, Math.min(event.clientY - rect.top, rect.height - 54));
      tooltip.style.left = `${left}px`;
      tooltip.style.top = `${top}px`;
    }

    function showGeoTooltip(event, feature) {
      const tooltip = document.getElementById('geo-tooltip');
      const count = geoProvinceCounts.get(provinceKey(feature)) || 0;
      tooltip.replaceChildren();
      appendText(tooltip, 'strong', provinceName(feature));
      appendText(tooltip, 'span', t('{count} users', {count}));
      appendText(tooltip, 'small', formatPercent(count, geoTotalUsers));
      tooltip.classList.add('visible');
      const label = document.getElementById(`geo-label-${provinceKey(feature)}`);
      if (label) label.classList.add('visible');
      moveGeoTooltip(event);
    }

    function hideGeoTooltip() {
      document.getElementById('geo-tooltip').classList.remove('visible');
      document.querySelectorAll('.china-map .map-label.visible').forEach(label => {
        label.classList.remove('visible');
      });
    }

    function walkGeoCoordinates(value, visitor) {
      if (!Array.isArray(value)) return;
      if (typeof value[0] === 'number' && typeof value[1] === 'number') {
        visitor(value);
        return;
      }
      value.forEach(item => walkGeoCoordinates(item, visitor));
    }

    function mercatorPoint(coord) {
      const lon = Number(coord[0]);
      const lat = Math.max(-85, Math.min(85, Number(coord[1])));
      const x = lon * Math.PI / 180;
      const y = Math.log(Math.tan(Math.PI / 4 + lat * Math.PI / 360));
      return [x, y];
    }

    function buildMapProjection(features) {
      let minX = Infinity;
      let minY = Infinity;
      let maxX = -Infinity;
      let maxY = -Infinity;
      features.forEach(feature => {
        walkGeoCoordinates(feature.geometry && feature.geometry.coordinates, coord => {
          const point = mercatorPoint(coord);
          minX = Math.min(minX, point[0]);
          minY = Math.min(minY, point[1]);
          maxX = Math.max(maxX, point[0]);
          maxY = Math.max(maxY, point[1]);
        });
      });
      const width = Math.max(maxX - minX, 0.0001);
      const height = Math.max(maxY - minY, 0.0001);
      const scale = Math.min(
        (MAP_WIDTH - MAP_PADDING * 2) / width,
        (MAP_HEIGHT - MAP_PADDING * 2) / height);
      const offsetX = (MAP_WIDTH - width * scale) / 2;
      const offsetY = (MAP_HEIGHT - height * scale) / 2;
      return coord => {
        const point = mercatorPoint(coord);
        return [
          offsetX + (point[0] - minX) * scale,
          offsetY + (maxY - point[1]) * scale
        ];
      };
    }

    function formatMapPoint(point) {
      return `${point[0].toFixed(1)} ${point[1].toFixed(1)}`;
    }

    function ringPath(ring, project) {
      if (!Array.isArray(ring) || !ring.length) return '';
      return ring.map((coord, index) => {
        const command = index === 0 ? 'M' : 'L';
        return `${command}${formatMapPoint(project(coord))}`;
      }).join(' ') + ' Z';
    }

    function geometryPath(geometry, project) {
      if (!geometry || !Array.isArray(geometry.coordinates)) return '';
      const paths = [];
      if (geometry.type === 'Polygon') {
        geometry.coordinates.forEach(ring => paths.push(ringPath(ring, project)));
      } else if (geometry.type === 'MultiPolygon') {
        geometry.coordinates.forEach(polygon => {
          polygon.forEach(ring => paths.push(ringPath(ring, project)));
        });
      }
      return paths.filter(Boolean).join(' ');
    }

    function geometryCenter(geometry) {
      let minLon = Infinity;
      let minLat = Infinity;
      let maxLon = -Infinity;
      let maxLat = -Infinity;
      walkGeoCoordinates(geometry && geometry.coordinates, coord => {
        const lon = Number(coord[0]);
        const lat = Number(coord[1]);
        if (!Number.isFinite(lon) || !Number.isFinite(lat)) return;
        minLon = Math.min(minLon, lon);
        minLat = Math.min(minLat, lat);
        maxLon = Math.max(maxLon, lon);
        maxLat = Math.max(maxLat, lat);
      });
      if (!Number.isFinite(minLon) || !Number.isFinite(minLat)) return null;
      return [(minLon + maxLon) / 2, (minLat + maxLat) / 2];
    }

    function featureCenter(feature) {
      if (feature && feature.properties && Array.isArray(feature.properties.cp)) {
        return feature.properties.cp;
      }
      return geometryCenter(feature && feature.geometry);
    }

    async function loadChinaMapData() {
      if (geoFeatures.length) return true;
      if (!geoMapDataPromise) {
        geoMapDataPromise = fetch(CHINA_MAP_URL, {credentials: 'same-origin'})
          .then(response => {
            if (!response.ok) throw new Error('map data unavailable');
            return response.json();
          })
          .then(data => {
            geoFeatures = (Array.isArray(data.features) ? data.features : [])
              .filter(feature => provinceKey(feature) &&
                feature.geometry && Array.isArray(feature.geometry.coordinates));
            geoFeatureByKey = new Map(
              geoFeatures.map(feature => [provinceKey(feature), feature]));
            return geoFeatures.length > 0;
          })
          .catch(() => {
            geoMapDataPromise = null;
            return false;
          });
      }
      return geoMapDataPromise;
    }

    function renderMapStatus(message) {
      const svg = document.getElementById('china-map');
      svg.setAttribute('viewBox', `0 0 ${MAP_WIDTH} ${MAP_HEIGHT}`);
      const text = document.createElementNS(MAP_NS, 'text');
      text.classList.add('map-status');
      text.setAttribute('x', MAP_WIDTH / 2);
      text.setAttribute('y', MAP_HEIGHT / 2);
      text.textContent = message;
      svg.replaceChildren(text);
    }

    async function ensureChinaMap() {
      if (geoMapReady) return true;
      if (!await loadChinaMapData()) {
        renderMapStatus(t('Map data could not be loaded'));
        return false;
      }
      const svg = document.getElementById('china-map');
      svg.setAttribute('viewBox', `0 0 ${MAP_WIDTH} ${MAP_HEIGHT}`);
      const project = buildMapProjection(geoFeatures);
      const provinceLayer = document.createElementNS(MAP_NS, 'g');
      provinceLayer.classList.add('province-layer');
      const labelLayer = document.createElementNS(MAP_NS, 'g');
      labelLayer.classList.add('label-layer');

      geoFeatures.forEach(feature => {
        const key = provinceKey(feature);
        const path = document.createElementNS(MAP_NS, 'path');
        path.id = `geo-province-${key}`;
        path.classList.add('province');
        path.setAttribute('d', geometryPath(feature.geometry, project));
        path.setAttribute('fill', GEO_COLORS[0]);
        path.setAttribute('fill-rule', 'evenodd');
        path.setAttribute('tabindex', '0');
        path.setAttribute('role', 'img');
        path.setAttribute('aria-label', provinceTooltipText(feature));
        path.addEventListener('mouseenter', event => showGeoTooltip(event, feature));
        path.addEventListener('mousemove', moveGeoTooltip);
        path.addEventListener('mouseleave', hideGeoTooltip);
        path.addEventListener('focus', event => showGeoTooltip(event, feature));
        path.addEventListener('blur', hideGeoTooltip);
        provinceLayer.appendChild(path);

        const label = document.createElementNS(MAP_NS, 'text');
        const centerCoord = featureCenter(feature);
        if (!centerCoord) return;
        const center = project(centerCoord);
        const offset = PROVINCE_LABEL_OFFSETS[key] || [0, 0];
        label.id = `geo-label-${key}`;
        label.classList.add('map-label');
        if (['beijing', 'tianjin', 'shanghai', 'hongkong', 'macau'].includes(key)) {
          label.classList.add('small');
        }
        label.setAttribute('x', (center[0] + offset[0]).toFixed(1));
        label.setAttribute('y', (center[1] + offset[1]).toFixed(1));
        label.textContent = provinceName(feature);
        labelLayer.appendChild(label);
      });
      svg.replaceChildren(provinceLayer, labelLayer);
      geoMapReady = true;
      return true;
    }

    function applyGeoMapColors() {
      let maxProvinceCount = 0;
      geoProvinceCounts.forEach(count => {
        if (count > maxProvinceCount) maxProvinceCount = count;
      });
      geoFeatures.forEach(feature => {
        const key = provinceKey(feature);
        const count = geoProvinceCounts.get(key) || 0;
        const path = document.getElementById(`geo-province-${key}`);
        if (!path) return;
        const label = document.getElementById(`geo-label-${key}`);
        if (label) label.textContent = provinceName(feature);
        path.setAttribute('fill', geoColor(count, maxProvinceCount));
        path.setAttribute('aria-label', provinceTooltipText(feature));
      });
    }

    function renderGeoDistribution(distribution) {
      const data = distribution || {
        total_count: 0,
        domestic_count: 0,
        foreign_count: 0,
        unknown_count: 0,
        provinces: [],
        countries: []
      };
      state.geo.distribution = data;
      geoTotalUsers = Number(data.total_count) || 0;
      geoProvinceCounts = new Map();
      (Array.isArray(data.provinces) ? data.provinces : []).forEach(item => {
        const count = Number(item.count) || 0;
        geoProvinceCounts.set(item.province, count);
      });

      const foreignCount = Number(data.foreign_count) || 0;
      const unknownCount = Number(data.unknown_count) || 0;
      const rawDomesticCount = Number(data.domestic_count);
      const domesticCount = data.domestic_count === undefined
        ? Math.max(0, geoTotalUsers - foreignCount - unknownCount)
        : (Number.isFinite(rawDomesticCount) ? rawDomesticCount : 0);
      document.getElementById('geo-total').textContent = geoTotalUsers;
      document.getElementById('geo-domestic-count').textContent = domesticCount;
      document.getElementById('geo-domestic-percent').textContent = formatPercent(domesticCount, geoTotalUsers);
      document.getElementById('geo-foreign-count').textContent = foreignCount;
      document.getElementById('geo-foreign-percent').textContent = formatPercent(foreignCount, geoTotalUsers);
      document.getElementById('geo-unknown-count').textContent = unknownCount;
      document.getElementById('geo-unknown-percent').textContent = formatPercent(unknownCount, geoTotalUsers);

      renderGeoUserList(data);

      if (!geoMapReady) {
        ensureChinaMap().then(ok => {
          if (ok) {
            renderGeoDistribution(state.geo.distribution);
          }
        });
        return;
      }
      applyGeoMapColors();
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

    function locationLabel(device) {
      if (device.geo_location) return device.geo_location;
      if (device.client_ip) return t('Unknown');
      return '-';
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
        appendEmptyRow(fragment, 5);
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

        const locationCell = document.createElement('td');
        labelCell(locationCell, 'Location');
        appendText(locationCell, 'div', locationLabel(device));
        row.appendChild(locationCell);

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
          detailsCell.colSpan = 5;
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
          appendDetailItem(details, 'Location', locationLabel(device));
          appendDetailItem(details, 'Client IP', device.client_ip || '-');
          appendDetailItem(details, 'Region', device.geo_region || '-');
          appendDetailItem(details, 'Country', device.geo_country || '-');
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
      renderGeoDistribution(data.geo_distribution);
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
    document.querySelectorAll('[data-geo-list]').forEach(button => {
      button.addEventListener('click', () => {
        state.geo.list = button.dataset.geoList === 'foreign' ? 'foreign' : 'domestic';
        renderGeoUserList(state.geo.distribution || {
          total_count: 0,
          domestic_count: 0,
          foreign_count: 0,
          unknown_count: 0,
          provinces: [],
          countries: []
        });
        const list = document.getElementById('geo-user-list');
        if (list) list.scrollTop = 0;
      });
    });
    document.getElementById('geo-list-order').addEventListener('click', () => {
      setCurrentGeoListOrder(currentGeoListOrder() === 'asc' ? 'desc' : 'asc');
      renderGeoUserList(state.geo.distribution || {
        total_count: 0,
        domestic_count: 0,
        foreign_count: 0,
        unknown_count: 0,
        provinces: [],
        countries: []
      });
      const list = document.getElementById('geo-user-list');
      if (list) list.scrollTop = 0;
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
    renderGeoDistribution();
    refreshStats().then((ok) => {
      if (ok) showDashboard();
      else showLogin('');
    }).catch(() => showLogin(''));
