/* MiiCam Web UI - vanilla JS SPA, no framework. */

const API = '/api';

async function api(path, opts) {
  const res = await fetch(API + path, opts);
  const ctype = res.headers.get('content-type') || '';
  if (ctype.includes('application/json')) {
    return res.json();
  }
  return res.text().then((t) => {
    try { return JSON.parse(t); } catch (e) { return { status: 'error', message: t }; }
  });
}

let toastTimer = null;
function toast(msg, kind) {
  const el = document.getElementById('toast');
  el.textContent = msg;
  el.className = 'toast show ' + (kind || '');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => el.classList.remove('show'), 3000);
}

/* ---------------- Tab navigation ---------------- */
function showTab(name) {
  document.querySelectorAll('.topnav a').forEach((a) => {
    a.classList.toggle('active', a.dataset.tab === name);
  });
  document.querySelectorAll('.panel').forEach((p) => {
    p.classList.remove('active');
  });
  document.getElementById('tab-' + name).classList.add('active');
  if (tabLoaders[name]) tabLoaders[name]();
}
document.querySelectorAll('.topnav a').forEach((a) => {
  a.addEventListener('click', (e) => { e.preventDefault(); showTab(a.dataset.tab); });
});

/* ---------------- Home / live ---------------- */
let streamActive = false;
let streamTimer = null;

/* lighttpd mod_cgi buffers all PHP output until exit, so multipart MJPEG
 * streaming is not possible; poll /api/snapshot (rtspd snapshots ~1-2s each,
 * and the API coalesces triggers so rtspd is never overloaded). */
function startStream() {
  const img = document.getElementById('live-stream');
  const tick = () => {
    if (!streamActive) return;
    img.onerror = () => toast('Snapshot error - is rtspd running?', 'err');
    img.src = '/api/snapshot?' + Date.now(); // cache-buster forces a fresh request
  };
  streamActive = true;
  document.getElementById('btn-stream-start').disabled = true;
  document.getElementById('btn-stream-stop').disabled = false;
  tick();
  streamTimer = setInterval(tick, 3000);
}
function stopStream() {
  streamActive = false;
  if (streamTimer) { clearInterval(streamTimer); streamTimer = null; }
  const img = document.getElementById('live-stream');
  img.src = 'data:image/gif;base64,R0lGODlhAQABAAAAACH5BAEKAAEALAAAAAABAAEAAAICTAEAOw==';
  img.onload = null; img.onerror = null;
  document.getElementById('btn-stream-start').disabled = false;
  document.getElementById('btn-stream-stop').disabled = true;
}

/* ---------------- Form field builders ---------------- */
/* Each builder appends a .field into a .form-grid element.
 * `keys` is the parsed config.cfg map; `cfgKey` (optional) is the config
 * key to bind the input to for save-via-patch. */

function fieldNumber(parentId, label, cfgKey, keys, opts) {
  opts = opts || {};
  const div = document.createElement('div');
  div.className = 'field';
  const l = document.createElement('label'); l.textContent = label;
  const input = document.createElement('input');
  input.type = 'number';
  if (opts.min !== undefined) input.min = opts.min;
  if (opts.max !== undefined) input.max = opts.max;
  const v = keys[cfgKey];
  input.value = (v === undefined || v === null || v === '') ? '' : v;
  if (cfgKey) input.dataset.cfgKey = cfgKey;
  div.appendChild(l); div.appendChild(input);
  document.getElementById(parentId).appendChild(div);
  return input;
}

function fieldSelect(parentId, label, cfgKey, keys, options) {
  const div = document.createElement('div');
  div.className = 'field';
  const l = document.createElement('label'); l.textContent = label;
  const sel = document.createElement('select');
  const cur = keys[cfgKey];
  options.forEach(([val, txt]) => {
    const o = document.createElement('option');
    o.value = val; o.textContent = txt;
    if (String(cur) === String(val)) o.selected = true;
    sel.appendChild(o);
  });
  if (cfgKey) sel.dataset.cfgKey = cfgKey;
  div.appendChild(l); div.appendChild(sel);
  document.getElementById(parentId).appendChild(div);
  return sel;
}

function fieldText(parentId, label, cfgKey, keys) {
  const div = document.createElement('div');
  div.className = 'field';
  const l = document.createElement('label'); l.textContent = label;
  const input = document.createElement('input');
  input.type = 'text';
  const v = keys[cfgKey];
  input.value = (v === undefined || v === null || v === '') ? '' : v;
  if (cfgKey) input.dataset.cfgKey = cfgKey;
  div.appendChild(l); div.appendChild(input);
  document.getElementById(parentId).appendChild(div);
  return input;
}

function fieldInfo(parentId, label, value) {
  const div = document.createElement('div');
  div.className = 'field';
  const l = document.createElement('label'); l.textContent = label;
  const s = document.createElement('div');
  s.style.padding = '7px 8px';
  s.style.background = 'var(--panel)';
  s.style.border = '1px solid var(--border)';
  s.style.borderRadius = '4px';
  s.style.color = 'var(--muted)';
  s.textContent = value;
  div.appendChild(l); div.appendChild(s);
  document.getElementById(parentId).appendChild(div);
}

function toggleField(label, live, liveOn, liveOff) {
  /* `live` is the current 0/1 state from /api/status. */
  const div = document.createElement('div');
  div.className = 'field';
  const l = document.createElement('label'); l.textContent = label;
  const wrap = document.createElement('div');
  wrap.className = 'toggle';
  const on = document.createElement('button'); on.textContent = 'On';
  const off = document.createElement('button'); off.textContent = 'Off';
  const paint = () => {
    on.classList.toggle('on-state', live === 1);
    off.classList.toggle('off-state', live !== 1);
  };
  on.addEventListener('click', async () => { live = 1; paint(); await liveOn(); });
  off.addEventListener('click', async () => { live = 0; paint(); await liveOff(); });
  paint();
  wrap.appendChild(on); wrap.appendChild(off);
  div.appendChild(l); div.appendChild(wrap);
  return div;
}

/* Save all inputs inside a group (marked with data-cfgKey) via config patch. */
async function saveCfgGroup(groupPrefix, extraKeys) {
  const inputs = document.querySelectorAll('#settings-' + groupPrefix + ' [data-cfgKey]');
  const keys = {};
  inputs.forEach((el) => { keys[el.dataset.cfgKey] = el.value; });
  if (extraKeys) Object.assign(keys, extraKeys);
  const body = new URLSearchParams();
  body.append('patch', '1');
  for (const [k, v] of Object.entries(keys)) {
    body.append('keys[' + k + ']', v);
  }
  const r = await api('/config', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: body,
  });
  if (r.status === 'ok') toast('Saved to config.cfg');
  else toast('Save failed: ' + (r.message || ''), 'err');
  return r;
}

/* ---------------- Settings page (video customization) ---------------- */
async function loadSettings() {
  try {
    const cfgR = await api('/config');
    const keys = cfgR.keys || {};
    const st = await api('/status');
    const cam = st.camera || {};

    /* --- Format: codec (fixed H.264), size, bitrate, bitrate mode, fps,
     *     flip (live) --- */
    const f = document.getElementById('settings-format');
    f.innerHTML = '';
    fieldInfo('settings-format', 'Codec', 'H.264 (fixed)');
    const resSel = fieldSelect('settings-format', 'Resolution', null, keys, [
      ['1280x720', '1280x720'],
      ['640x360', '640x360'],
      ['320x180', '320x180'],
      ['custom', 'Custom (via config.cfg)'],
    ]);
    fieldNumber('settings-format', 'Width', 'RTSP_WIDTH', keys, { min: 160, max: 1280 });
    fieldNumber('settings-format', 'Height', 'RTSP_HEIGHT', keys, { min: 120, max: 720 });
    fieldNumber('settings-format', 'Bitrate (kbps)', 'RTSP_BITRATE', keys, { min: 100, max: 16000 });
    fieldNumber('settings-format', 'Bitrate mode (1-4)', 'RTSP_BITRATE_MODE', keys, { min: 1, max: 4 });
    fieldNumber('settings-format', 'FPS (1-15)', 'RTSP_FRAMERATE', keys, { min: 1, max: 15 });
    const flipEl = toggleField('Flip (live)', (cam.flipmode === undefined || cam.flipmode === null) ? 0 : cam.flipmode,
        async () => setMode('flipmode', 'on'),
        async () => setMode('flipmode', 'off'));
    f.appendChild(flipEl);

    /* --- Security: RTSP user/pass + port (fixed 554) --- */
    const s = document.getElementById('settings-security');
    s.innerHTML = '';
    fieldText('settings-security', 'RTSP username', 'RTSP_USER', keys);
    fieldText('settings-security', 'RTSP password', 'RTSP_PASS', keys);
    fieldInfo('settings-security', 'RTSP port', '554 (fixed)');
    fieldText('settings-security', 'Web API username', 'HTTP_API_USER', keys);
    fieldText('settings-security', 'Web API password', 'HTTP_API_PASS', keys);

    /* --- Day / Night: live toggles + auto night mode --- */
    const d = document.getElementById('settings-daynight');
    d.innerHTML = '';
    d.appendChild(toggleField('Night mode', (cam.nightmode === undefined || cam.nightmode === null) ? 0 : cam.nightmode,
      async () => setMode('nightmode', 'on'), async () => setMode('nightmode', 'off')));
    d.appendChild(toggleField('IR cut', (cam.ir_cut === undefined || cam.ir_cut === null) ? 0 : cam.ir_cut,
      async () => setMode('ir_cut', 'on'), async () => setMode('ir_cut', 'off')));
    fieldSelect('settings-daynight', 'Auto night mode', 'AUTO_NIGHT_MODE', keys, [
      ['0', 'Off'],
      ['1', 'On'],
    ]);
    fieldInfo('settings-daynight', 'Note', 'Mode/IR-cut apply live; auto night mode applies at next boot.');

    /* --- Audio: encode type + sample rate (+ volume is config-only) --- */
    const a = document.getElementById('settings-audio');
    a.innerHTML = '';
    fieldSelect('settings-audio', 'Encode type', 'RTSP_AUDIO_TYPE', keys, [
      ['', 'Off'],
      ['aac', 'AAC'],
      ['pcm', 'PCM'],
      ['g726', 'G.726 / ADPCM'],
      ['alaw', 'G.711 A-law'],
      ['ulaw', 'G.711 µ-law'],
    ]);
    fieldSelect('settings-audio', 'Sample rate', 'RTSP_AUDIO_RATE', keys, [
      ['', 'Default (16000)'],
      ['8000', '8000'],
      ['16000', '16000'],
      ['32000', '32000'],
      ['48000', '48000'],
    ]);
    fieldText('settings-audio', 'Volume (config only)', 'RTSP_AUDIO_VOLUME', keys);
    fieldInfo('settings-audio', 'Note', 'Audio applied at RTSP start. Volume has no driver backend yet.');

    /* --- OSD — text applied at start; color/size; timestamp is fixed --- */
    const o = document.getElementById('settings-osd');
    o.innerHTML = '';
    fieldText('settings-osd', 'Text', 'RTSP_OSD_TEXT', keys);
    fieldSelect('settings-osd', 'Background color (0-15)', 'RTSP_OSD_COLOR', keys, [
      ['', 'Default'],
      ['0', '0 (black)'],
      ['1', '1'],
      ['2', '2'],
      ['3', '3'],
      ['4', '4 (white)'],
      ['5', '5'],
      ['6', '6'],
      ['7', '7'],
      ['8', '8'],
      ['9', '9'],
      ['10', '10'],
      ['11', '11'],
      ['12', '12'],
      ['13', '13'],
      ['14', '14'],
      ['15', '15'],
    ]);
    fieldSelect('settings-osd', 'Font zoom (0-12)', 'RTSP_OSD_SIZE', keys, [
      ['', 'Default'],
      ['0', '0 (off)'],
      ['1', '1'],
      ['2', '2'],
      ['3', '3'],
      ['4', '4'],
      ['5', '5'],
      ['6', '6'],
      ['7', '7'],
      ['8', '8'],
      ['9', '9'],
      ['10', '10'],
      ['11', '11'],
      ['12', '12'],
    ]);
    fieldInfo('settings-osd', 'Timestamp', 'Always shown when OSD is on (rtspd default)');
    fieldInfo('settings-osd', 'Note', 'OSD text/color/size apply at RTSP start.');

    /* --- Network: wifi SSID/password (router MAC has no backend) --- */
    const n = document.getElementById('settings-network');
    n.innerHTML = '';
    fieldText('settings-network', 'WiFi SSID', 'WIFI_SSID', keys);
    fieldText('settings-network', 'WiFi password', 'WIFI_PASS', keys);
    fieldInfo('settings-network', 'Router MAC', 'Not supported by this firmware');
    fieldInfo('settings-network', 'Note', 'WiFi changes apply at next boot (configure_wifi).');

    /* --- Wire the buttons --- */
    const fSave = document.getElementById('btn-format-save');
    const fApply = document.getElementById('btn-format-apply');
    const securitySave = document.getElementById('btn-security-save');
    const daynightSave = document.getElementById('btn-daynight-save');
    const audioSave = document.getElementById('btn-audio-save');
    const osdSave = document.getElementById('btn-osd-save');
    const netSave = document.getElementById('btn-network-save');

    fSave.onclick = async () => {
      const wInp = document.querySelector('#settings-format input[data-cfgKey="RTSP_WIDTH"]');
      const hInp = document.querySelector('#settings-format input[data-cfgKey="RTSP_HEIGHT"]');
      const rv = resSel.value;
      if (rv !== 'custom') {
        const parts = rv.split('x');
        if (wInp) wInp.value = parts[0];
        if (hInp) hInp.value = parts[1];
      }
      saveCfgGroup('format');
    };
    fApply.onclick = async () => {
      const seq = [['bitrate', 'RTSP_BITRATE'], ['fps', 'RTSP_FRAMERATE'], ['mode', 'RTSP_BITRATE_MODE']];
      let any = false;
      for (const [sub, key] of seq) {
        const el = document.querySelector('#settings-format [data-cfgKey="' + key + '"]');
        if (el && el.value !== '') {
          await api('/codec/' + sub, {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: 'value=' + encodeURIComponent(el.value),
          });
          any = true;
        }
      }
      toast(any ? 'Bitrate/FPS/Mode applied (rtspd restarts)' : 'No values to apply', any ? '' : 'err');
    };
    securitySave.onclick = () => saveCfgGroup('security');
    daynightSave.onclick = () => saveCfgGroup('daynight');
    audioSave.onclick = () => saveCfgGroup('audio');
    osdSave.onclick = () => saveCfgGroup('osd');
    netSave.onclick = () => saveCfgGroup('network');

    document.getElementById('btn-rtsp-restart').onclick = async () => {
      const r = await api('/service/rtsp/restart');
      toast((r.output || r.status), r.status === 'ok' ? '' : 'err');
    };
  } catch (e) {
    toast('Failed to load settings: ' + e.message, 'err');
  }
}

/* ---------------- Device settings (Config tab) ---------------- */
const MODES = [
  ['nightmode', 'Night mode'],
  ['ir_cut', 'IR cut'],
  ['mirrormode', 'Mirror'],
  ['flipmode', 'Flip'],
];

function toggleFor(key, label, current, onCb, offCb) {
  const div = document.createElement('div');
  div.className = 'field';
  const l = document.createElement('label');
  l.textContent = label;
  const wrap = document.createElement('div');
  wrap.className = 'toggle';
  const on = document.createElement('button');
  on.textContent = 'On';
  const off = document.createElement('button');
  off.textContent = 'Off';
  const paint = () => {
    const v = current === 1;
    on.classList.toggle('on-state', v);
    off.classList.toggle('off-state', !v);
  };
  on.addEventListener('click', async () => { current = 1; paint(); await onCb(); });
  off.addEventListener('click', async () => { current = 0; paint(); await offCb(); });
  paint();
  wrap.appendChild(on); wrap.appendChild(off);
  div.appendChild(l); div.appendChild(wrap);
  return div;
}

function buildSettings(parentId, key, label, value) {
  const div = document.createElement('div');
  div.className = 'field';
  const l = document.createElement('label'); l.textContent = label;
  const input = document.createElement('input');
  input.type = 'number';
  input.value = (value === null || value === undefined) ? '' : value;
  input.dataset.setting = key;
  input.dataset.applied = (value === null || value === undefined) ? '' : value;
  div.appendChild(l); div.appendChild(input);
  document.getElementById(parentId).appendChild(div);
}

async function loadDeviceSettings() {
  try {
    const data = await api('/status');
    const st = data.status === 'ok' ? data : null;

    const imgWrap = document.getElementById('settings-image');
    imgWrap.innerHTML = '';
    MODES.forEach(([k, label]) => {
      const v = st && st.camera && st.camera[k];
      imgWrap.appendChild(toggleFor(k, label, v === 1 ? 1 : 0,
        async () => { await api('/mode/' + k + '/on', { method: 'POST' }); toast(k + ' on'); },
        async () => { await api('/mode/' + k + '/off', { method: 'POST' }); toast(k + ' off'); }));
    });

    const ledWrap = document.getElementById('settings-leds');
    ledWrap.innerHTML = '';
    [['blue', 'Blue LED'], ['yellow', 'Yellow LED'], ['ir', 'IR LED']].forEach(([k, label]) => {
      const v = st && st.led && st.led[k];
      ledWrap.appendChild(toggleFor('led-' + k, label, v === 1 ? 1 : 0,
        async () => await setMode(k === 'ir' ? 'ir_led' : (k + '_led'), 'on'),
        async () => await setMode(k === 'ir' ? 'ir_led' : (k + '_led'), 'off')));
    });

    const adjustWrap = document.getElementById('settings-adjust');
    adjustWrap.innerHTML = '';
    const adjustInputs = [];
    try {
      const adj = await api('/camera/info');
      const c = adj.camera || {};
      Object.keys(c).forEach((k) => {
        const trimmed = k.trim();
        const input = document.createElement('div');
        input.className = 'field';
        const l = document.createElement('label');
        l.textContent = trimmed.charAt(0).toUpperCase() + trimmed.slice(1);
        const el = document.createElement('input');
        el.type = 'text';
        el.value = c[k] === null || c[k] === undefined ? '' : c[k];
        el.dataset.adjust = trimmed;
        el.dataset.original = c[k];
        adjustInputs.push(el);
        input.appendChild(l); input.appendChild(el);
        adjustWrap.appendChild(input);
      });
      const applyBtn = document.createElement('button');
      applyBtn.className = 'primary';
      applyBtn.textContent = 'Apply camera';
      applyBtn.addEventListener('click', async () => {
        let applied = false;
        for (const el of adjustInputs) {
          if (el.value !== '' && String(el.dataset.original) !== el.value.trim()) {
            const r = await api('/camera/' + el.dataset.adjust, {
              method: 'POST',
              headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
              body: 'value=' + encodeURIComponent(el.value.trim()),
            });
            el.dataset.original = el.value.trim();
            applied = true;
            if (r.status !== 'ok') toast((r.message || 'Failed ' + el.dataset.adjust), 'err');
          }
        }
        toast(applied ? 'Camera adjustment applied' : 'No changes', applied ? '' : 'err');
      });
      adjustWrap.appendChild(applyBtn);
    } catch (e) { /* ignore */ }

    const codecWrap = document.getElementById('settings-codec');
    codecWrap.innerHTML = '';
    if (st && st.codec) {
      const v = st.codec.video || {};
      [['framerate', 'FPS (1-15)'], ['bitrate', 'Bitrate (kbps)'], ['gop', 'GOP'], ['rate_mode', 'Mode (1-4)']].forEach(([k, label]) => {
        buildSettings('settings-codec', 'codec-' + k, label, v[k]);
      });
      const applyBtn = document.createElement('button');
      applyBtn.className = 'primary';
      applyBtn.textContent = 'Apply encoder';
      applyBtn.addEventListener('click', applyCodec);
      codecWrap.appendChild(applyBtn);
    }

    await loadMotor();
    await loadServices();
  } catch (e) {
    toast('Failed to load device settings: ' + e.message, 'err');
  }
}

async function setMode(name, action) {
  const r = await api('/mode/' + name + '/' + action, { method: 'POST' });
  if (r.status !== 'ok') toast((r.message || 'Failed') , 'err');
  else toast(name + ' ' + action);
}

function applyCodec() {
  const map = {
    'codec-framerate': 'fps',
    'codec-bitrate': 'bitrate',
    'codec-gop': 'gop',
    'codec-rate_mode': 'mode',
  };
  const todo = [];
  for (const [elKey, sub] of Object.entries(map)) {
    const input = document.querySelector('#settings-codec input[data-setting="' + elKey + '"]');
    if (input && input.value !== '' && input.dataset.applied !== input.value) {
      todo.push(api('/codec/' + sub, {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'value=' + encodeURIComponent(input.value),
      }).then((r) => {
        input.dataset.applied = input.value;
        if (r.status !== 'ok') toast((r.message || 'Failed ' + sub), 'err');
      }));
    }
  }
  Promise.all(todo).then(() => {
    toast(todo.length ? 'Encoder settings staged (restarting rtspd)' : 'No changes', todo.length ? '' : 'err');
  });
}

async function loadMotor() {
  try {
    const r = await api('/motor/status');
    if (r.x !== undefined) {
      document.getElementById('pz-x').textContent = r.x;
      document.getElementById('pz-y').textContent = r.y;
    }
  } catch (e) { /* ignore */ }
}

async function loadServices() {
  const tbody = document.querySelector('#services-table tbody');
  tbody.innerHTML = '<tr><td colspan="3">Loading…</td></tr>';
  try {
    const r = await api('/services');
    const list = r.services || {};
    tbody.innerHTML = '';
    for (const [name, info] of Object.entries(list)) {
      const tr = document.createElement('tr');
      const tdN = document.createElement('td'); tdN.textContent = name;
      const tdS = document.createElement('td');
      tdS.textContent = info.running ? 'running' : 'stopped';
      tdS.style.color = info.running ? '#7bd88f' : '#f07178';
      const tdA = document.createElement('td');
      for (const act of ['start', 'stop', 'restart']) {
        const b = document.createElement('button');
        b.textContent = act;
        b.style.marginRight = '4px';
        b.addEventListener('click', async () => {
          const res = await api('/service/' + name + '/' + act);
          toast((res.output || res.message || act), res.status === 'ok' ? '' : 'err');
          loadServices();
        });
        tdA.appendChild(b);
      }
      tr.appendChild(tdN); tr.appendChild(tdS); tr.appendChild(tdA);
      tbody.appendChild(tr);
    }
  } catch (e) {
    tbody.innerHTML = '<tr><td colspan="3">Failed</td></tr>';
  }
}

/* ---------------- Config editor ---------------- */
async function loadConfig() {
  const area = document.getElementById('config-area');
  try {
    const r = await api('/config');
    area.value = r.raw || '';
    setMsg('');
  } catch (e) {
    setMsg('Failed to read config: ' + e.message, 'err');
  }
}
function setMsg(text, kind) {
  const el = document.getElementById('config-msg');
  el.textContent = text;
  el.className = 'msg ' + (kind || 'ok');
}
async function saveConfig() {
  const area = document.getElementById('config-area');
  const r = await fetch('/api/config', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: new URLSearchParams({ raw: area.value }),
  }).then((res) => res.json());
  if (r.status === 'ok') setMsg('Config saved.');
  else setMsg(r.message || 'Failed to save', 'err');
}

/* ---------------- Info dashboard ---------------- */
function kvRow(label, value, tableId) {
  const tr = document.createElement('tr');
  const tdL = document.createElement('td'); tdL.textContent = label;
  const tdV = document.createElement('td');
  if (value === null || value === undefined || value === '') tdV.textContent = '-';
  else if (typeof value === 'object') tdV.textContent = JSON.stringify(value);
  else tdV.textContent = String(value);
  tr.appendChild(tdL);
  tr.appendChild(tdV);
  document.getElementById(tableId).querySelector('tbody').appendChild(tr);
}

function fmtUptime(uptimeStr) {
  const m = /up\s+(\d+)\s+(min|hour|day)/.exec(uptimeStr || '');
  return uptimeStr || '-';
}

function fmtBytes(kb) {
  kb = parseInt(kb, 10);
  if (isNaN(kb)) return '-';
  if (kb >= 1048576) return (kb / 1048576).toFixed(2) + ' GB';
  if (kb >= 1024) return (kb / 1024).toFixed(1) + ' MB';
  return kb + ' kB';
}

async function loadInfo() {
  try {
    const [sys, st] = await Promise.all([api('/system'), api('/status')]);
    const cam = st.camera || {};
    const led = st.led || {};
    const codec = (st.codec && st.codec.video) || {};
    const mem = sys.meminfo || {};
    const net = sys.network || {};

    document.getElementById('info-camera').querySelector('tbody').innerHTML = '';
    kvRow('Night mode', cam.nightmode === 1 ? 'on' : 'off', 'info-camera');
    kvRow('IR cut', cam.ir_cut === 1 ? 'on' : 'off', 'info-camera');
    kvRow('Mirror', cam.mirrormode === 1 ? 'on' : 'off', 'info-camera');
    kvRow('Flip', cam.flipmode === 1 ? 'on' : 'off', 'info-camera');
    kvRow('Blue LED', led.blue === 1 ? 'on' : 'off', 'info-camera');
    kvRow('Yellow LED', led.yellow === 1 ? 'on' : 'off', 'info-camera');
    kvRow('IR LED', led.ir === 1 ? 'on' : 'off', 'info-camera');
    kvRow('Encoder', (codec.width || '?') + 'x' + (codec.height || '?') +
      ' @ ' + (codec.framerate || '?') + ' fps', 'info-camera');
    kvRow('Bitrate', codec.bitrate ? codec.bitrate + ' kbps' : '-', 'info-camera');
    kvRow('Mode', codec.rate_mode, 'info-camera');

    document.getElementById('info-system').querySelector('tbody').innerHTML = '';
    kvRow('Hostname', sys.hostname, 'info-system');
    kvRow('Kernel', sys.kernel, 'info-system');
    kvRow('Arch', sys.arch, 'info-system');
    kvRow('Uptime', fmtUptime(sys.uptime), 'info-system');
    kvRow('Load average', sys.loadavg, 'info-system');
    kvRow('Memory', fmtBytes(mem.MemTotal) + ' total / ' + fmtBytes(mem.MemFree) + ' free', 'info-system');

    document.getElementById('info-network').querySelector('tbody').innerHTML = '';
    kvRow('IP', net.ip, 'info-network');
    kvRow('MAC', net.mac, 'info-network');
    kvRow('Gateway', net.gateway, 'info-network');
    kvRow('SSID', net.essid, 'info-network');
    kvRow('AP', net.ap, 'info-network');
    kvRow('Bitrate', net.bitrate, 'info-network');
    kvRow('Signal', net.signal_dbm !== undefined && net.signal_dbm !== null
      ? net.signal_dbm + ' dBm' : '-', 'info-network');
    kvRow('Link quality', net.link_quality_raw, 'info-network');
  } catch (e) {
    toast('Failed to load dashboard: ' + e.message, 'err');
  }
}

/* ---------------- SDCard ---------------- */
async function loadSDCard() {
  const pre = document.getElementById('sdcard-pre');
  const media = document.getElementById('sdcard-media');
  pre.textContent = 'Loading…';
  try {
    const r = await api('/sdcard');
    const lines = [];
    if (r.last_image) lines.push('Last image: ' + r.last_image.url);
    if (r.last_video) lines.push('Last video: ' + r.last_video.url);
    lines.push(r.output || '');
    pre.textContent = lines.filter(Boolean).join('\n');

    media.innerHTML = '';
    [['last_image', 'Last image'], ['last_video', 'Last video']].forEach(([k, capKey]) => {
      const m = r[k];
      if (m) {
        const card = document.createElement('div');
        card.className = 'media-card';
        if (m.kind === 'image') {
          const img = document.createElement('img');
          img.src = m.url;
          card.appendChild(img);
        } else {
          const link = document.createElement('a');
          link.href = m.url;
          link.textContent = m.url;
          card.appendChild(link);
        }
        const cap = document.createElement('div');
        cap.className = 'cap';
        cap.textContent = m.path;
        cap.title = m.path;
        card.appendChild(cap);
        media.appendChild(card);
      }
    });
  } catch (e) {
    pre.textContent = 'Failed: ' + e.message;
  }
}

/* ---------------- Tab loaders ---------------- */
const tabLoaders = {
  settings: loadSettings,
  config: loadDeviceSettings,
  info: loadInfo,
  sdcard: loadSDCard,
};

/* ---------------- Wire up buttons ---------------- */
document.getElementById('btn-stream-start').addEventListener('click', startStream);
document.getElementById('btn-stream-stop').addEventListener('click', stopStream);

document.getElementById('config-reload').addEventListener('click', loadConfig);
document.getElementById('config-save').addEventListener('click', saveConfig);

document.getElementById('info-refresh').addEventListener('click', loadInfo);

document.getElementById('pz-goto').addEventListener('click', async () => {
  const x = document.getElementById('pz-goto-x').value;
  const y = document.getElementById('pz-goto-y').value;
  if (x === '' || y === '') { toast('Enter X and Y', 'err'); return; }
  await api('/motor/goto', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'x=' + encodeURIComponent(x) + '&y=' + encodeURIComponent(y),
  });
  loadMotor();
});
document.querySelectorAll('[data-motor]').forEach((b) => {
  b.addEventListener('click', async () => {
    const dir = b.dataset.motor;
    await api('/motor/' + dir);
    loadMotor();
  });
});

/* ---------------- Init ---------------- */
fetch('/api/status').then((r) => r.json()).then((d) => {
  document.getElementById('connstate').textContent = d.status === 'ok' ? '● Camera online' : '● Camera error';
}).catch(() => {
  document.getElementById('connstate').textContent = '● Offline';
});
showTab('home');