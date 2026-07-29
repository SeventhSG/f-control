import { connect, send, on, isLive } from './api.js';

const root = document.getElementById('app');
const esc = s => String(s ?? '').replace(/[&<>"']/g, c =>
  ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

const S = {
  screen: 'gate',        // gate | roster | thread | settings
  locked: true,
  bad: false,
  attemptsLeft: 3,
  peers: [],
  me: null,
  net: null,
  peer: null,            // open thread
  messages: [],
  draft: '',
  down: false,
};

const MAX_BYTES = 180;   // real FCP payload budget, see the spec
const DENSE = 50;        // above this the mesh is past its tested density

// ---------------------------------------------------------------------------
// nearness
// ---------------------------------------------------------------------------

const NEAR_DBM = -45, FAR_DBM = -100;
const clamp = (n, a, b) => Math.min(b, Math.max(a, n));

/** 0 for a peer in the room, 1 for one at the edge of hearing. */
const distance = rssi => clamp((NEAR_DBM - rssi) / (NEAR_DBM - FAR_DBM), 0, 1);

/** Size, indent and opacity are derived from signal, never hard coded. This is
 *  the whole idea: the roster reads as a gradient of presence.
 *
 *  A relayed peer is pinned near the far end regardless of signal. We heard
 *  the relay, not them, so their distance is genuinely unknown, and unknown
 *  belongs at the quiet edge of the list rather than wherever the relay
 *  happens to be standing. */
function typeFor(p) {
  const t = p.hops ? 0.86 : distance(p.rssi);
  return {
    size: (33 - t * 22).toFixed(1),
    indent: (t * 92).toFixed(0),
    opacity: (1 - t * 0.5).toFixed(2),
    mono: t > 0.82 && !p.hops,
  };
}

/** Never state a distance we did not measure. */
const band = p => p.hops ? 'relayed'
  : p.rssi > -60 ? 'near' : p.rssi > -75 ? '10s of m' : p.rssi > -90 ? '100s of m' : 'far';

/** The signal line under the band, honest about whose signal it is. */
const signalLine = p => p.hops
  ? `through ${p.hops} ${p.hops === 1 ? 'node' : 'nodes'}`
  : `${p.rssi} dBm`;

const bytes = s => new TextEncoder().encode(s).length;

// ---------------------------------------------------------------------------
// screens
// ---------------------------------------------------------------------------

const screens = {
  gate: () => `
    <div class="gate">
      <div class="mark">f-control</div>
      <div class="field">
        <input id="pass" type="password" autocomplete="off" autocapitalize="off"
               spellcheck="false" placeholder="passphrase" ${S.locked ? '' : 'disabled'}>
        <button class="go" id="unlock">Unlock</button>
      </div>
      <p class="why ${S.bad ? 'bad' : ''}">${S.bad
        ? `That passphrase is wrong. ${S.attemptsLeft} ${S.attemptsLeft === 1 ? 'attempt' : 'attempts'} left before this board locks itself for a while.`
        : 'This board holds your private key and will not go on the air until it is unlocked. Nobody can recover this passphrase for you.'}</p>
    </div>`,

  roster: () => `
    ${header(`<span class="mark">f-control</span><span class="grow"></span>
              <span class="stat">${S.peers.length} heard</span>
              <span class="lnk" data-go="settings">Settings</span>`)}
    <div class="sub">${esc(netLine())}</div>
    ${banners()}
    <div class="body">
      ${S.peers.length ? `<div class="roster">${S.peers.map(entry).join('')}</div>` : `
        <div class="none">Nothing on the air.<br>
        <span style="color:var(--faint)">Beacons repeat every few seconds. Someone in range will appear on their own.</span></div>`}
    </div>`,

  thread: () => {
    const p = S.peer;
    const b = bytes(S.draft);
    return `
      ${header(`<span class="lnk" data-go="roster">Back</span>
                <span class="mark peer ${p.verified ? 'vf' : ''}"
                      style="${p.verified ? 'border-bottom:1px solid var(--gold);padding-bottom:2px' : ''}"
                >${esc(p.name || 'unnamed node')}</span>
                <span class="grow"></span>
                <span class="stat">${band(p)} &nbsp; ${signalLine(p)}</span>`)}
      <div class="sub">${p.verified
        ? `verified in person &nbsp; ${esc(p.fp)}`
        : `<span style="color:var(--red)">not verified</span> &nbsp; read the words aloud together, then confirm`}</div>
      ${p.verified ? '' : `<div class="banner warn">
        <b>You have not confirmed who this is.</b> Anyone can claim a name. Meet them, read the three words to each other, then press confirm.
        <span class="lnk" data-verify="1" style="color:var(--gold);margin-left:6px">Confirm</span></div>`}
      <div class="body"><div class="th" id="th">
        ${S.messages.map(msgHtml).join('') || `<div class="none">Nothing yet. Messages live in the board's memory and are gone when it loses power.</div>`}
      </div></div>
      <div class="compose">
        <input id="draft" placeholder="write" maxlength="400" autocomplete="off"
               autocapitalize="sentences" value="${esc(S.draft)}">
        <span class="budget ${b > MAX_BYTES ? 'over' : ''}">${b} / ${MAX_BYTES}</span>
        <button class="send" id="send" ${b === 0 || b > MAX_BYTES ? 'disabled' : ''}>Send</button>
      </div>`;
  },

  settings: () => `
    ${header(`<span class="lnk" data-go="roster">Back</span>
              <span class="mark">f-control</span><span class="grow"></span>`)}
    <div class="body"><div class="set">

      <div class="grp">
        <h2>You</h2>
        <p>These three words are your identity. Read them aloud to someone standing next to you and they can be certain it is really you they are talking to.</p>
        <dl>
          <div class="rowline"><dt>Name</dt><dd>${esc(S.me?.name)}</dd></div>
          <div class="rowline"><dt>Fingerprint</dt><dd><span class="words">${esc(S.me?.fp)}</span></dd></div>
          <div class="rowline"><dt>Contacts</dt><dd>${S.me?.contacts} of ${S.me?.max}</dd></div>
        </dl>
        <button class="act" data-act="backup">Export backup</button>
      </div>

      <div class="grp">
        <h2>Network</h2>
        <p>${esc(netExplainer())}</p>
        <dl>
          <div class="rowline"><dt>Mode</dt><dd>${S.net?.mode === 'station' ? 'Home network' : 'Own access point'}</dd></div>
          ${S.net?.mode === 'station'
            ? `<div class="rowline"><dt>Joined</dt><dd>${esc(S.net.ssid)}, channel ${S.net.channel}</dd></div>` : ''}
          <div class="rowline"><dt>Mesh</dt><dd>channel ${S.net?.meshChannel ?? 1}</dd></div>
        </dl>
        <button class="act" data-act="net">Change network</button>
      </div>

      <div class="grp">
        <h2>Erase</h2>
        <p>Messages already live in memory only and vanish when the board loses power. This clears them now, along with every session key.</p>
        <button class="act danger" data-act="wipe">Wipe now</button>
        <button class="act" data-act="lock">Lock the board</button>
      </div>

    </div></div>`,
};

// ---------------------------------------------------------------------------
// pieces
// ---------------------------------------------------------------------------

const header = inner => `<div class="top">${inner}</div>`;

function entry(p) {
  const ty = typeFor(p);
  const name = p.name || 'unnamed node';
  return `
    <div class="ent" data-peer="${esc(p.id)}"
         style="padding-left:${ty.indent}px;opacity:${ty.opacity}">
      <div>
        <span class="nm ${ty.mono ? 'far' : ''} ${p.verified ? 'vf' : ''}"
              style="${ty.mono ? '' : `font-size:${ty.size}px`}">${esc(name)}</span>
        <div class="fp">${p.verified
          ? esc(p.fp)
          : `<span class="blk"></span><span class="unv">UNVERIFIED</span>`}</div>
      </div>
      <div class="rt">
        <b>${band(p)}</b>
        <s>${signalLine(p)}</s>
      </div>
    </div>`;
}

function msgHtml(m) {
  const cls = m.dir === 'out' ? `out ${m.state}` : '';
  const tail = m.dir === 'out'
    ? m.state === 'heard' ? 'heard' : m.state === 'lost' ? 'not heard' : 'sending'
    : '';
  return `<div class="msg ${cls}"><p>${esc(m.text)}</p>
          <div class="mt">${esc(m.at)}${tail ? ' &nbsp; ' + tail : ''}</div></div>`;
}

function banners() {
  const out = [];
  /* First, always, and not dismissible. A person who flashed this after
     reading the README could reasonably believe their messages are protected.
     They are not, and the interface says so before anything else. */
  if (S.me?.nocrypto) out.push(`<div class="banner warn">
    <b>This build has no encryption.</b> Messages travel in clear over the air and anyone with a receiver can read them. Names are not signed, so anyone can claim to be anyone. Treat every conversation here as public.</div>`);
  if (S.down) out.push(`<div class="banner warn"><b>The board stopped answering.</b> Reconnecting.</div>`);
  if (S.net?.beta) out.push(`<div class="banner">
    <b>Home network mode, beta.</b> Your network runs on channel ${S.net.channel} and the mesh meets on channel ${S.net.meshChannel}, so this board can only listen to the mesh in short windows. Discovery and delivery are slower, and it will not relay for anyone else. Set your router to channel ${S.net.meshChannel}, or use the board's own access point.</div>`);
  if (S.peers.length > DENSE) out.push(`<div class="banner warn">
    <b>${S.peers.length} nodes in range.</b> Above ${DENSE} the mesh is past the density it was tested at, and messages may start to collide.</div>`);
  if (S.me && S.me.contacts >= S.me.max) out.push(`<div class="banner warn">
    <b>Contact list is full at ${S.me.max}.</b> Remove someone before you can verify anyone new.</div>`);
  return out.join('');
}

const netLine = () => !S.net ? 'starting'
  : S.net.mode === 'station'
    ? `home network ${S.net.ssid} · mesh channel ${S.net.meshChannel}`
    : `own access point · mesh channel ${S.net.meshChannel}`;

const netExplainer = () => S.net?.mode === 'station'
  ? 'This board is on a network you already had. That is convenient, and it costs range and speed on the mesh, because one radio cannot sit on two channels at once.'
  : 'This board runs its own access point on the mesh channel. This is the fastest and most reliable way to use it.';

// ---------------------------------------------------------------------------
// render
// ---------------------------------------------------------------------------

function render() {
  const atBottom = () => {
    const b = root.querySelector('.body');
    return !b || b.scrollHeight - b.scrollTop - b.clientHeight < 60;
  };
  const stick = S.screen === 'thread' && atBottom();

  root.innerHTML = screens[S.screen]();

  if (S.screen === 'gate') root.querySelector('#pass')?.focus();
  if (S.screen === 'thread') {
    const b = root.querySelector('.body');
    if (b && stick) b.scrollTop = b.scrollHeight;
    const d = root.querySelector('#draft');
    if (d) { d.focus(); d.setSelectionRange(d.value.length, d.value.length); }
  }
}

// ---------------------------------------------------------------------------
// events
// ---------------------------------------------------------------------------

root.addEventListener('click', e => {
  const go = e.target.closest('[data-go]')?.dataset.go;
  if (go) { S.screen = go; return render(); }

  const peerId = e.target.closest('[data-peer]')?.dataset.peer;
  if (peerId) {
    S.peer = S.peers.find(p => p.id === peerId);
    S.messages = [];
    S.draft = '';
    S.screen = 'thread';
    send({ t: 'open', peer: peerId });
    return render();
  }

  if (e.target.closest('[data-verify]')) return send({ t: 'verify', peer: S.peer.id });
  if (e.target.id === 'unlock') return doUnlock();
  if (e.target.id === 'send') return doSend();

  const act = e.target.closest('[data-act]')?.dataset.act;
  if (act === 'wipe' && confirm('Clear every message and session key on this board now?')) send({ t: 'wipe' });
  if (act === 'lock') send({ t: 'lock' });
  if (act === 'backup') alert('Backup export arrives with the firmware that has a key to export.');
  if (act === 'net') alert('Network setup arrives with the firmware.');
});

root.addEventListener('input', e => {
  if (e.target.id !== 'draft') return;
  S.draft = e.target.value;
  const b = bytes(S.draft);
  const badge = root.querySelector('.budget');
  badge.textContent = `${b} / ${MAX_BYTES}`;
  badge.classList.toggle('over', b > MAX_BYTES);
  root.querySelector('#send').disabled = b === 0 || b > MAX_BYTES;
});

root.addEventListener('keydown', e => {
  if (e.key !== 'Enter') return;
  if (e.target.id === 'draft') doSend();
  if (e.target.id === 'pass') doUnlock();
});

function doUnlock() {
  const el = root.querySelector('#pass');
  if (el?.value) send({ t: 'unlock', passphrase: el.value });
}

function doSend() {
  const text = S.draft.trim();
  if (!text || bytes(text) > MAX_BYTES) return;
  send({ t: 'send', peer: S.peer.id, text });
  S.draft = '';
  render();
}

// ---------------------------------------------------------------------------
// frames from the board
// ---------------------------------------------------------------------------

on(f => {
  switch (f.t) {
    case 'state':
      S.locked = f.locked;
      S.bad = !!f.bad;
      S.attemptsLeft = f.attemptsLeft ?? S.attemptsLeft;
      S.screen = f.locked ? 'gate' : 'roster';
      break;
    case 'roster':
      S.peers = [...f.peers].sort((a, b) => b.rssi - a.rssi);
      if (S.peer) S.peer = S.peers.find(p => p.id === S.peer.id) ?? S.peer;
      break;
    case 'me': S.me = f; break;
    case 'net': S.net = f; break;
    case 'thread':
      if (S.peer?.id === f.peer) S.messages = f.messages;
      break;
    case 'msg':
      if (S.peer?.id === f.peer) S.messages = [...S.messages, f.message];
      break;
    case 'ack': {
      const m = S.messages.find(x => x.id === f.id);
      if (m) { m.state = f.state; }
      break;
    }
    case 'wiped': S.messages = []; break;
    case 'down': S.down = true; break;
  }
  render();
});

// Development only: ?dev=roster|thread|settings skips the gate.
const dev = new URLSearchParams(location.search).get('dev');
if (dev && !isLive()) {
  send({ t: 'unlock', passphrase: 'devdev' });
  setTimeout(() => {
    if (dev === 'thread') {
      S.peer = S.peers[0];
      send({ t: 'open', peer: S.peer.id });
    }
    S.screen = dev;
    render();
  }, 400);
}

render();
connect();
