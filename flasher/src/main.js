import { listBoards, firmwareInfo, writeFirmware, isMock } from './api.js';

const view = document.getElementById('view');
const actions = document.getElementById('actions');
const stepEls = [...document.querySelectorAll('.step')];

const esc = s => String(s).replace(/[&<>"']/g, c =>
  ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

const state = {
  screen: 'scanning',   // scanning | found | writing | done | error
  boards: [],
  selected: null,
  firmware: null,
  progress: { phase: 'connect', done: 0, total: 1 },
  credentials: null,
  error: null,
};

const STEP_OF = { scanning: 0, found: 0, writing: 1, done: 2, error: 1 };

const PHASES = [
  ['connect', 'Handshake with the chip'],
  ['erase', 'Erase existing flash'],
  ['write', 'Write firmware'],
  ['verify', 'Verify what was written'],
];

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
      <div class="notice warn"><b>Writing erases the board completely.</b> If it already runs f-control, its identity and verified contacts are destroyed, and only a backup you exported yourself can bring them back.</div>`;
  },

  writing: () => {
    const { phase, done, total } = state.progress;
    const at = PHASES.findIndex(p => p[0] === phase);
    const pct = phase === 'write' ? (done / total) * 100 : (phase === 'connect' ? 0 : 100);
    return `
      <h1 class="sm">Writing</h1>
      <p class="lede">Leave the board plugged in until this finishes.</p>
      <div class="phases">
        ${PHASES.map(([id, text], i) => `
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
    const c = state.credentials;
    return `
      <h1>Ready</h1>
      <p class="lede">The board generated its own keypair on first boot. The private key is on the chip and has never been anywhere else, including this computer.</p>
      <div class="cred">
        <div class="row">
          <button class="copy" data-copy="${esc(c.ssid)}">copy</button>
          <div class="k">Network</div><div class="v">${esc(c.ssid)}</div>
        </div>
        <div class="row">
          <button class="copy" data-copy="${esc(c.password)}">copy</button>
          <div class="k">Password</div><div class="v">${esc(c.password)}</div>
        </div>
        <div class="row">
          <div class="k">Fingerprint</div><div class="v words">${esc(c.fingerprint)}</div>
        </div>
      </div>
      <p class="lede">Join that network from your phone or laptop, then open <span style="color:var(--bone)">192.168.4.1</span>. The board asks you to set a passphrase, and after that it will not beacon or accept a conversation until it is unlocked.</p>
      <div class="notice good">Those three words are your identity. Read them aloud to someone in person and they can confirm they are really talking to you. Nobody can do that for you, and no server will vouch for you.</div>
      <div class="notice warn"><b>Write the network password down now.</b> It is generated on the board and this screen is the only place it is shown.</div>`;
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
  const at = PHASES.findIndex(p => p[0] === phase);
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

async function doWrite() {
  state.screen = 'writing';
  state.progress = { phase: 'connect', done: 0, total: 1 };
  render();
  try {
    state.credentials = await writeFirmware(state.selected.port, p => {
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
  });
  render();
  scan();
}

actions.addEventListener('click', e => {
  const act = e.target.closest('[data-act]')?.dataset.act;
  if (act === 'write') doWrite();
  if (act === 'rescan' || act === 'again') reset();
});

view.addEventListener('click', async e => {
  const el = e.target.closest('[data-copy]');
  if (!el) return;
  await navigator.clipboard.writeText(el.dataset.copy);
  el.textContent = 'copied';
  el.classList.add('ok');
  setTimeout(() => { el.textContent = 'copy'; el.classList.remove('ok'); }, 1400);
});

// Development only: ?dev=writing|done|error renders a screen without hardware.
const devScreen = new URLSearchParams(location.search).get('dev');
if (isMock() && devScreen) {
  Object.assign(state, {
    screen: devScreen,
    selected: { port: 'COM7', label: 'ESP32 board', adapter: 'CP210x', certain: false },
    firmware: await firmwareInfo(),
    progress: { phase: 'write', done: 712_000, total: 1_284_096 },
    credentials: { ssid: 'f-control-a91b', password: 'kt7m-2xvp-r94q', fingerprint: 'horse amber nine' },
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
