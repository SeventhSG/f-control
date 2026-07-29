import { listBoards, firmwareInfo, writeFirmware, isMock } from './api.js';

const view = document.getElementById('view');
const actions = document.getElementById('actions');
const stepEls = [...document.querySelectorAll('.step')];

const esc = s => String(s).replace(/[&<>"']/g, c =>
  ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

const genKey = () => {
  const bytes = new Uint8Array(12);
  crypto.getRandomValues(bytes);
  return [...bytes].map(b => b.toString(16).padStart(2, '0')).join('').match(/.{1,4}/g).join('-');
};

const state = {
  screen: 'scanning',   // scanning | found | writing | done | error
  boards: [],
  selected: null,
  firmware: null,
  progress: { phase: 'connect', done: 0, total: 1 },
  credentials: null,
  error: null,
  // Kept across boards on purpose: flashing several boards in one sitting
  // should mean they share a network key with no extra step, which is the
  // whole reason two boards can talk to each other at all.
  config: { name: '', ap: '', ssid: '', pass: '', netkey: genKey() },
  willConfigure: false,
};

const STEP_OF = { scanning: 0, found: 0, writing: 1, done: 2, error: 1 };

const PHASES_BASE = [
  ['connect', 'Handshake with the chip'],
  ['erase', 'Erase existing flash'],
  ['write', 'Write firmware'],
  ['verify', 'Verify what was written'],
];
const PHASE_CONFIGURE = ['configure', 'Send your settings'];

// ---------------------------------------------------------------------------
// screens
// ---------------------------------------------------------------------------

const screens = {
  scanning: () => `
    <h1>Plug in a board</h1>
    <p class="lede">Connect an ESP32 over USB. Any ESP32, ESP32-S3 or ESP32-C3 works, with no radio module and nothing else attached.</p>
    <p class="lede">Nothing is written until you ask for it.</p>
    <div class="ports">
      <div class="empty"><span class="scan"></span>Looking for a board<br>
        <span style="color:var(--faint);font-size:10.5px">If one is plugged in and nothing appears, the USB serial driver is probably missing. Most boards use CP210x or CH340.</span>
      </div>
    </div>`,

  found: () => {
    const b = state.selected;
    const f = state.firmware;
    const c = state.config;
    return `
      <h1>${esc(b.label)}</h1>
      <p class="lede">Found on ${esc(b.port)} through ${esc(b.adapter)}.${b.certain ? '' :
        ' The USB adapter does not say which chip is behind it, so the exact model is identified when the write begins.'}</p>
      <div class="facts">
        ${fact('Firmware', `${f.version}, built ${f.built}`)}
        ${fact('Size', `${(f.bytes / 1024).toFixed(0)} KB`)}
        ${fact('SHA-256', f.sha256, true)}
      </div>
      <p class="lede">Everyone gets the same image. Nothing personal is compiled into it. That checksum is verified before a byte reaches the board.</p>

      <div class="label" style="margin-top:26px">Set up this board</div>
      <div class="cfg">
        <div class="r"><span class="k">Name</span>
          <input id="cfg-name" placeholder="unnamed, shown to others" maxlength="20" value="${esc(c.name)}"></div>
        <div class="r"><span class="k">Network name</span>
          <input id="cfg-ap" placeholder="auto, f-control-xxxx" maxlength="24" value="${esc(c.ap)}"></div>
        <div class="r"><span class="k">Home wifi</span>
          <input id="cfg-ssid" placeholder="leave blank to skip" value="${esc(c.ssid)}"></div>
        <div class="r"><span class="k">Wifi password</span>
          <input id="cfg-pass" type="password" placeholder="leave blank if open" value="${esc(c.pass)}"></div>
        <div class="r wide"><span class="k">Network key</span>
          <input id="cfg-netkey" value="${esc(c.netkey)}">
          <button class="genbtn" data-act="genkey">New</button>
          <button class="copy" data-copy="${esc(c.netkey)}">copy</button></div>
      </div>
      <p class="lede">Every field above is optional and is sent to the board once, right after writing, so nothing needs to be typed into its dashboard by hand. Boards that should hear each other need the <b style="color:var(--bone)">same network key</b>: it carries over automatically if you flash more than one board in this session, or paste in one you used before.</p>

      <div class="notice warn"><b>Writing erases the board completely.</b> If it already runs f-control, its identity and verified contacts are destroyed, and only a backup you exported yourself can bring them back.</div>`;
  },

  writing: () => {
    const { phase, done, total } = state.progress;
    const phases = activePhases();
    const at = phases.findIndex(p => p[0] === phase);
    const pct = phase === 'write' ? (done / total) * 100 : (phase === 'connect' ? 0 : 100);
    return `
      <h1 class="sm">Writing</h1>
      <p class="lede">Leave the board plugged in until this finishes.</p>
      <div class="phases">
        ${phases.map(([id, text], i) => `
          <div class="phase ${i < at ? 'ok' : i === at ? 'on' : ''}">
            <span class="tick"></span>${esc(text)}
          </div>`).join('')}
      </div>
      <div class="track"><i style="width:${pct.toFixed(1)}%"></i></div>
      <div class="readout">
        <span>${esc(phase)}</span>
        <span>${phase === 'write' ? `${(done / 1024).toFixed(0)} of ${(total / 1024).toFixed(0)} KB` : ''}</span>
      </div>`;
  },

  done: () => {
    const cr = state.credentials;
    const cf = state.config;
    const apLine = cf.ap
      ? `Join <span style="color:var(--bone)">${esc(cf.ap)}</span>`
      : `Join the network named <span style="color:var(--bone)">f-control-</span> followed by four characters`;
    return `
      <h1>Written</h1>
      <p class="lede">The board is running f-control and has restarted${state.willConfigure ? ', with the settings from the last screen already applied' : ''}.</p>
      <div class="cred">
        <div class="row">
          <div class="k">Chip</div><div class="v">${esc(cr.chip)}</div>
        </div>
        <div class="row">
          <button class="copy" data-copy="${esc(cr.mac)}">copy</button>
          <div class="k">MAC</div><div class="v">${esc(cr.mac)}</div>
        </div>
      </div>
      <p class="lede">The board makes its own identity on first boot, from its own random number generator. <span style="color:var(--bone)">This program never sees it</span>, so it cannot show it to you and cannot keep a copy.</p>
      <p class="lede">${apLine} from a phone, then open <span style="color:var(--bone)">192.168.4.1</span>. The fingerprint and network key are both on the Settings screen there.</p>
      <div class="notice warn"><b>This is a shared network key, not private per-person encryption.</b> Anyone holding the same key, including everyone else on this network, can read every message and claim to be anyone. It stops a stranger with a receiver, nothing more.</div>`;
  },

  error: () => `
    <h1 class="sm">Not written</h1>
    <div class="notice warn"><b>${esc(state.error.title)}</b><br>${esc(state.error.detail)}</div>
    <p class="lede">${esc(state.error.fix)}</p>
    <p class="lede">The board is unchanged if this failed during handshake. If it failed while writing, the board is in an unknown state and needs to be written again before it will run.</p>`,
};

const fact = (k, v, muted = false) =>
  `<div class="fact"><dt>${esc(k)}</dt><dd class="${muted ? 'mut' : ''}">${esc(v)}</dd></div>`;

// ---------------------------------------------------------------------------
// footer actions
// ---------------------------------------------------------------------------

const footers = {
  scanning: () => btn('Write firmware', 'write', true) + spacer() + hint(isMock() ? 'development mode, no board is being touched' : 'waiting for a board'),
  found: () => btn('Write firmware', 'write') + btn('Rescan', 'rescan', false, true),
  writing: () => btn('Write firmware', 'write', true) + spacer() + hint('do not unplug the board'),
  done: () => btn('Flash another board', 'again') + spacer() + hint('this board is finished'),
  error: () => btn('Try again', 'again') + btn('Rescan', 'rescan', false, true),
};

const btn = (label, act, disabled = false, ghost = false) =>
  `<button class="btn ${ghost ? 'ghost' : ''}" data-act="${act}" ${disabled ? 'disabled' : ''}>${esc(label)}</button>`;
const spacer = () => `<span class="spacer"></span>`;
const hint = (t, err = false) => `<span class="hint ${err ? 'err' : ''}">${esc(t)}</span>`;

// ---------------------------------------------------------------------------
// render
// ---------------------------------------------------------------------------

// Progress arrives many times a second. Re-rendering the whole view for each
// tick remounts the bar, which restarts its width transition from zero and
// makes it stutter instead of advance. Mutate the two nodes that change.
function updateProgress() {
  const { phase, done, total } = state.progress;
  const at = activePhases().findIndex(p => p[0] === phase);
  const fill = view.querySelector('.track i');
  if (!fill) return render();

  fill.style.width =
    `${(phase === 'write' ? (done / total) * 100 : phase === 'connect' ? 0 : 100).toFixed(1)}%`;

  view.querySelectorAll('.phase').forEach((el, i) => {
    el.classList.toggle('ok', i < at);
    el.classList.toggle('on', i === at);
  });

  const [left, right] = view.querySelectorAll('.readout span');
  left.textContent = phase;
  right.textContent = phase === 'write'
    ? `${(done / 1024).toFixed(0)} of ${(total / 1024).toFixed(0)} KB` : '';
}

function render() {
  view.innerHTML = screens[state.screen]();
  actions.innerHTML = footers[state.screen]();
  const at = STEP_OF[state.screen];
  stepEls.forEach((el, i) => {
    el.classList.toggle('on', i === at);
    el.classList.toggle('done', i < at);
  });
}

// ---------------------------------------------------------------------------
// behaviour
// ---------------------------------------------------------------------------

let scanTimer = null;

async function scan() {
  if (state.screen !== 'scanning') return;
  try {
    const boards = await listBoards();
    if (state.screen !== 'scanning') return;
    if (boards.length) {
      state.boards = boards;
      state.selected = boards[0];
      state.firmware ??= await firmwareInfo();
      state.screen = 'found';
      render();
      return;
    }
  } catch (e) {
    fail('Cannot read the USB ports', String(e.message ?? e),
      'Close anything else that might be holding the serial port, such as a serial monitor or the Arduino IDE, then try again.');
    return;
  }
  scanTimer = setTimeout(scan, 900);
}

const activePhases = () => state.willConfigure ? [...PHASES_BASE, PHASE_CONFIGURE] : PHASES_BASE;

async function doWrite() {
  const c = state.config;
  const provision = Object.values(c).some(v => v.trim()) ? { ...c } : null;
  state.willConfigure = !!provision;

  state.screen = 'writing';
  state.progress = { phase: 'connect', done: 0, total: 1 };
  render();
  try {
    state.credentials = await writeFirmware(state.selected.port, provision, p => {
      state.progress = p;
      if (state.screen === 'writing') updateProgress();
    });
    state.screen = 'done';
    render();
  } catch (e) {
    fail('The write did not complete', String(e.message ?? e),
      'Unplug the board, plug it back in, and try again. Some boards need the BOOT button held while they are connected.');
  }
}

function fail(title, detail, fix) {
  state.error = { title, detail, fix };
  state.screen = 'error';
  render();
}

function reset() {
  clearTimeout(scanTimer);
  Object.assign(state, {
    screen: 'scanning', boards: [], selected: null,
    progress: { phase: 'connect', done: 0, total: 1 }, credentials: null, error: null,
    willConfigure: false,
  });
  // The network key carries over on purpose, see the note where it is
  // defined. Everything specific to one person or one board does not.
  state.config = { ...state.config, name: '', ap: '', ssid: '', pass: '' };
  render();
  scan();
}

actions.addEventListener('click', e => {
  const act = e.target.closest('[data-act]')?.dataset.act;
  if (act === 'write') doWrite();
  if (act === 'rescan' || act === 'again') reset();
});

view.addEventListener('click', async e => {
  if (e.target.closest('[data-act="genkey"]')) {
    state.config.netkey = genKey();
    render();
    return;
  }

  const el = e.target.closest('[data-copy]');
  if (!el) return;
  await navigator.clipboard.writeText(el.dataset.copy);
  el.textContent = 'copied';
  el.classList.add('ok');
  setTimeout(() => { el.textContent = 'copy'; el.classList.remove('ok'); }, 1400);
});

// Mirrors typed values into state without re-rendering, so the input the
// person is actively typing into never loses focus or cursor position.
view.addEventListener('input', e => {
  const field = { 'cfg-name': 'name', 'cfg-ap': 'ap', 'cfg-ssid': 'ssid',
                  'cfg-pass': 'pass', 'cfg-netkey': 'netkey' }[e.target.id];
  if (field) state.config[field] = e.target.value;
});

// Development only: ?dev=writing|done|error renders a screen without hardware.
const devScreen = new URLSearchParams(location.search).get('dev');
if (isMock() && devScreen) {
  Object.assign(state, {
    screen: devScreen,
    selected: { port: 'COM7', label: 'ESP32 board', adapter: 'CP210x', certain: false },
    firmware: await firmwareInfo(),
    progress: { phase: 'write', done: 712_000, total: 1_284_096 },
    credentials: { chip: 'Esp32', mac: 'ec:e3:34:da:c3:a0' },
    willConfigure: true,
    error: {
      title: 'The board stopped responding',
      detail: 'Timed out waiting for the chip to acknowledge a block at offset 0x21000.',
      fix: 'Unplug the board, hold the BOOT button, plug it back in, release the button, then try again. A cable that only carries power and not data is the other common cause.',
    },
  });
  render();
} else {
  render();
  scan();
}
