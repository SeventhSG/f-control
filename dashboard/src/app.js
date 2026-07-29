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
  noboard: false,
  unread: new Set(),
  networks: null,        // null = never scanned, [] = scanned and found nothing
  scanning: false,
  picked: null,          // ssid selected in the network list
  focusDraftNext: false, // refocus the compose field after this render only
};

const MAX_BYTES = 160;   // real FCP payload budget: 24 byte nonce for XChaCha20, see fcp.h
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
  /* Reached only when the page was served by a board but the live connection
     never opened. It gets its own screen rather than a banner, because the
     banners only render inside the roster and this failure happens before the
     roster is ever reached. */
  noboard: () => `
    <div class="gate">
      <div class="mark">f-control</div>
      <p class="why bad" style="margin-top:20px">No answer from the board.</p>
      <p class="why">This page came from the board, so it is powered and serving. The live connection did not open, which usually means the board is busy or was restarted while this page was loading.</p>
      <p class="why">If that does not help, power the board off and on. Nothing is shown until it answers, because showing anything else would be made up.</p>
      <button class="act" data-act="reload" style="margin-top:18px">Reload</button>
    </div>`,

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
        <b>You have not confirmed who this is.</b> Anyone can claim a name. Meet them, read the three words to each other, then press confirm. This also sends them your network key over the open radio, so you end up able to read each other without typing anything in Settings. It is not a secure exchange, it is exactly as safe as reading the key aloud yourselves, just faster.
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
        <p>Your name is what other boards show in their list. It is cosmetic and anybody can claim any name, so it identifies you the way a nickname does, not the way a passport does.</p>
        <div class="field" style="margin-bottom:14px">
          <input id="name" maxlength="20" placeholder="unnamed" autocomplete="off"
                 spellcheck="false" value="${esc(S.me?.name === 'unnamed' ? '' : S.me?.name)}">
          <button class="go" id="savename">Save</button>
        </div>
        <dl>
          <div class="rowline"><dt>Fingerprint</dt><dd><span class="words">${esc(S.me?.fp)}</span></dd></div>
        </dl>
      </div>

      <div class="grp">
        <h2>Encryption</h2>
        <p>Every board given the same network key can read each other's messages, and nobody else can. This is a shared secret among everyone on the network, not a private channel between two people: it does not say who sent a message, only that someone holding the key did.</p>
        <dl>
          <div class="rowline"><dt>Key</dt><dd><span class="words">${esc(S.me?.netkeyFp)}</span></dd></div>
        </dl>
        <p style="margin-top:10px">Read this fingerprint aloud against another board's. If they differ, the two boards cannot read each other yet.</p>

        <div class="field" style="margin-top:14px">
          <input id="key-phrase" placeholder="a phrase your friend also enters" autocomplete="off" spellcheck="false">
          <button class="go" id="setkey">Match</button>
        </div>
        <p style="margin-top:8px">Whatever you type here, type the exact same thing on your friend's board. It takes effect right away, no restart, and only changes what new messages are sealed with, not anything already sent.</p>

        <button class="act" data-act="newkey" style="margin-top:14px">Create a new key</button>
        <p style="margin-top:8px">Starts a private network nobody else can read into, including boards that had your old key. Give the new fingerprint to whoever should be able to read you now.</p>
      </div>

      <div class="grp">
        <h2>Network</h2>
        <p>${esc(netExplainer())}</p>
        <dl>
          <div class="rowline"><dt>Mode</dt><dd>${S.net?.mode === 'station'
            ? (S.net?.joined ? 'On your network' : 'Trying to join') : "This board's own"}</dd></div>
          ${S.net?.mode === 'station' ? `
          <div class="rowline"><dt>Network</dt><dd>${esc(S.net.ssid)}</dd></div>
          <div class="rowline"><dt>Address</dt><dd>${esc(S.net.ip || 'waiting')}</dd></div>` : ''}
          <div class="rowline"><dt>Channel</dt><dd>${S.net?.channel ?? '?'}</dd></div>
        </dl>

        ${S.net?.beta ? `<div class="banner warn" style="border-bottom:0;padding-left:14px;margin:12px 0">
          <b>The mesh moved to channel ${S.net.channel}.</b> This board has one radio, so joining a network on channel ${S.net.channel} took the mesh there too. It can only hear boards that are also on channel ${S.net.channel}, which in practice means boards on this same network.</div>` : ''}

        ${S.net?.mode === 'station'
          ? `<button class="act" data-act="leave">Leave this network</button>`
          : ''}
        <button class="act" data-act="scan" ${S.scanning ? 'disabled' : ''}>${
          S.scanning ? 'Scanning' : 'Find networks'}</button>

        ${S.networks === null ? '' : S.networks.length === 0
          ? `<p style="margin-top:14px">Nothing found. Try again closer to the router.</p>`
          : `<div class="ports" style="margin-top:14px">${S.networks.map(n => `
              <div class="port ${S.picked === n.ssid ? 'sel' : ''}" data-ssid="${esc(n.ssid)}">
                <span class="nm">${esc(n.ssid)}</span>
                <span class="meta">${n.secure ? 'locked' : 'open'} &nbsp; ${n.rssi} dBm</span>
              </div>`).join('')}</div>`}

        ${S.picked ? `
          <div class="field" style="margin-top:14px">
            <input id="wifipass" type="password" placeholder="password for ${esc(S.picked)}"
                   autocomplete="off" spellcheck="false">
            <button class="go" id="joinwifi">Join</button>
          </div>
          <p style="margin-top:10px">Leave it empty if the network is open. The board keeps these and rejoins on its own after a restart.</p>` : ''}
      </div>

      <div class="grp">
        <h2>Erase</h2>
        <p>Messages already live in memory only and vanish when the board loses power. This clears them now.</p>
        <button class="act danger" data-act="wipe">Wipe now</button>
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
        <b>${S.unread.has(p.id) ? '<span style="color:var(--gold)">new message</span>' : band(p)}</b>
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

  /* First, always, and not dismissible. A shared network key is real
     protection against a passive listener without it, and it is just as
     really NOT private between two people: everyone holding that key reads
     everything and nothing here is signed. Both halves need to reach the
     screen, or somebody reads only the reassuring half. */
  if (S.me?.crypto === 'netkey') out.push(`<div class="banner warn">
    <b>Messages are sealed with this board's network key,</b> fingerprint ${esc(S.me.netkeyFp)}. Anyone who was given the same key, including everyone else on this network, can read every message and claim to be anyone. This is not private, per-person encryption.</div>`);
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
  ? 'This board is on a network you already had, so the dashboard is reachable from anywhere on it. Boards sharing a network share a channel, so the mesh between them works normally.'
  : 'This board runs its own network. Nothing else is involved, which is the simplest and most reliable way to use it, and it means the dashboard is only reachable by joining this board directly.';

// ---------------------------------------------------------------------------
// render
// ---------------------------------------------------------------------------

let lastRenderedScreen = null;
let renderPending = false;

/* Every incoming frame, including a roster update that just means "a beacon
 * arrived," used to call render() unconditionally, which replaces the whole
 * screen's DOM. Replacing a DOM node that currently has keyboard focus blurs
 * it, on every browser, with no way to opt out, whether or not the code ever
 * calls .focus() itself. On a phone that closes the keyboard mid-sentence,
 * which is exactly "one second of keyboard then kicked out" for any field on
 * any screen, not just the compose box the first fix addressed.
 *
 * The actual fix has to be at this level: while a text field in view has
 * focus, do not touch the DOM at all. State updates still land in S, they are
 * just not painted until the field loses focus, at which point whatever is
 * pending is applied in one shot. A render the USER caused, by tapping Send
 * or a settings button, is unaffected: the click already moved focus off the
 * input before its handler runs, so this guard is not looking at that input
 * by the time it checks. */
function isTypingSomewhere() {
  const el = document.activeElement;
  return !!el && root.contains(el) && (el.tagName === 'INPUT' || el.tagName === 'TEXTAREA');
}

function render() {
  if (isTypingSomewhere()) { renderPending = true; return; }
  renderPending = false;

  const atBottom = () => {
    const b = root.querySelector('.body');
    return !b || b.scrollHeight - b.scrollTop - b.clientHeight < 60;
  };
  const stick = S.screen === 'thread' && atBottom();

  /* Roster updates arrive on their own schedule, every few seconds, as
     beacons come in, and used to force a render no matter what screen was on
     screen. Rebuilding innerHTML resets scroll to the top, so anyone reading
     Settings while a beacon happened to land got yanked back up mid-scroll.
     A render that lands on the SAME screen the person is already looking at
     restores where they were; only an actual navigation gets to reset it. */
  const sameScreen = lastRenderedScreen === S.screen;
  const prevScroll = sameScreen ? root.querySelector('.body')?.scrollTop : 0;
  lastRenderedScreen = S.screen;

  root.innerHTML = screens[S.screen]();

  if (sameScreen && prevScroll) {
    const b = root.querySelector('.body');
    if (b) b.scrollTop = prevScroll;
  }

  if (S.screen === 'thread') {
    const b = root.querySelector('.body');
    if (b && stick) b.scrollTop = b.scrollHeight;
    /* Deliberately not auto-focused here. Focusing a text field opens the
       keyboard on mobile immediately, which used to happen on every render,
       including the very first time the thread opened, before anyone had a
       chance to reach the Confirm button underneath it. Opening a chat should
       not fight opening a keyboard the moment you look at someone's name. */
    if (S.focusDraftNext) {
      S.focusDraftNext = false;
      const d = root.querySelector('#draft');
      if (d) { d.focus(); d.setSelectionRange(d.value.length, d.value.length); }
    }
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
    S.unread.delete(peerId);
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
  if (act === 'wipe' && confirm('Clear every message on this board now?')) send({ t: 'wipe' });
  if (act === 'reload') location.reload();
  if (act === 'scan') { S.scanning = true; S.picked = null; send({ t: 'scan' }); render(); }
  if (act === 'leave') { S.networks = null; S.picked = null; send({ t: 'leave' }); }

  const ssid = e.target.closest('[data-ssid]')?.dataset.ssid;
  if (ssid) { S.picked = ssid; render(); }

  if (e.target.id === 'savename') {
    const el = root.querySelector('#name');
    if (el) send({ t: 'setname', name: el.value.trim() });
  }
  if (e.target.id === 'setkey') {
    const el = root.querySelector('#key-phrase');
    if (el && el.value.trim()) { send({ t: 'setkey', passphrase: el.value }); el.value = ''; }
  }
  if (act === 'newkey' && confirm('This board will stop reading messages from anyone still on the old key. Continue?')) {
    send({ t: 'newkey' });
  }
  if (e.target.id === 'joinwifi') {
    const pw = root.querySelector('#wifipass');
    send({ t: 'join', ssid: S.picked, password: pw ? pw.value : '' });
    S.picked = null; S.networks = null; render();
  }
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

// 'blur' does not bubble, 'focusout' does. Whatever a person was typing into
// just lost focus, so any render that arrived while they were typing and was
// deferred is safe to apply now. Deferred by one tick rather than run here
// directly: focusout fires as part of losing focus to whatever was just
// tapped, commonly a button, and replacing the DOM before that button's own
// click finishes dispatching would swap the element out from under the
// click and the button press would be silently lost.
root.addEventListener('focusout', () => {
  if (renderPending) setTimeout(() => { if (renderPending) render(); }, 0);
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
  /* The keyboard was already open, since typing is how a send happens, so
     keeping focus here is a convenience for a fast follow-up message rather
     than the surprise it is when it happens on first opening the thread. */
  S.focusDraftNext = true;
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
      if (S.peer?.id === f.peer) {
        S.messages = [...S.messages, f.message];
      } else if (f.message?.dir === 'in') {
        /* A message for a thread that is not open would otherwise vanish with
         * no trace anywhere in the interface, and the person who sent it would
         * be told nothing either. Mark the sender so the roster shows it. */
        S.unread.add(f.peer);
      }
      break;
    case 'noboard':
      S.noboard = true;
      S.screen = 'noboard';
      break;
    case 'ack': {
      const m = S.messages.find(x => x.id === f.id);
      if (m) { m.state = f.state; }
      break;
    }
    case 'wiped': S.messages = []; break;
    case 'networks':
      S.networks = f.list ?? [];
      S.scanning = false;
      break;
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
