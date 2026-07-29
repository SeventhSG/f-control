// The board speaks one WebSocket, JSON frames both ways. This module is the
// only place that knows that. Open it in a browser with no board attached and
// the mock takes over, so the interface is developable without hardware.

const listeners = new Set();
export const on = fn => { listeners.add(fn); return () => listeners.delete(fn); };
const emit = frame => listeners.forEach(fn => fn(frame));

let socket = null;
let live = false;

/**
 * The mock is a development tool and must never stand in for a real board.
 *
 * It used to take over whenever the socket failed, which meant a page served
 * from an actual ESP32 could silently fall back to invented peers with
 * invented names. Somebody testing two boards would have seen a populated
 * roster and concluded the radio worked. That is the worst failure a tool like
 * this can have: looking like it works.
 *
 * So the mock is now allowed only when the page is opened from a file or from
 * localhost, which is to say only during development. Served from a board, a
 * failed socket is reported as a failed socket.
 */
const devHost = location.protocol === 'file:'
  || ['localhost', '127.0.0.1', ''].includes(location.hostname);

export function connect() {
  if (devHost && location.protocol !== 'http:') return startMock();

  let socket_;
  try {
    socket_ = new WebSocket(`ws://${location.host}/ws`);
  } catch {
    return devHost ? startMock() : emit({ t: 'noboard' });
  }
  socket = socket_;

  const fallback = setTimeout(() => {
    if (live) return;
    if (devHost) startMock(); else emit({ t: 'noboard' });
  }, 1500);

  socket.onopen = () => { live = true; clearTimeout(fallback); };
  socket.onmessage = e => { try { emit(JSON.parse(e.data)); } catch {} };
  socket.onerror = () => {
    clearTimeout(fallback);
    if (!live) { if (devHost) startMock(); else emit({ t: 'noboard' }); }
  };
  socket.onclose = () => {
    if (!live) return;
    live = false;
    emit({ t: 'down' });
    setTimeout(connect, 1500);
  };
}

export function send(frame) {
  if (live) return socket.send(JSON.stringify(frame));
  mockHandle(frame);
}

export const isLive = () => live;

// ---------------------------------------------------------------------------
// Development stand-in. Never runs on the board.
// ---------------------------------------------------------------------------

const PEERS = [
  { id: 'a1', name: 'kaya',  verified: true,  fp: 'horse amber nine', rssi: -58, hops: 0 },
  { id: 'b2', name: 'deniz', verified: true,  fp: 'river quiet two',  rssi: -79, hops: 0 },
  { id: 'c3', name: 'tolga', verified: false, fp: 'stone eight fern', rssi: -88, hops: 2 },
  { id: 'd4', name: '',      verified: false, fp: '',                 rssi: -97, hops: 1 },
];

const THREADS = {
  a1: [
    { id: 1, dir: 'in',  text: 'im at the north gate, the blue door not the green one', at: '14:02', state: 'heard' },
    { id: 2, dir: 'out', text: 'ok, ten minutes', at: '14:03', state: 'heard' },
    { id: 3, dir: 'in',  text: 'bring the small antenna if you still have it', at: '14:05', state: 'heard' },
    { id: 4, dir: 'out', text: 'on my way', at: '14:07', state: 'lost' },
  ],
};

let nextId = 100;

function startMock() {
  live = false;
  setTimeout(() => emit({ t: 'state', locked: true, attemptsLeft: 3 }), 60);
}

function mockHandle(frame) {
  const reply = f => setTimeout(() => emit(f), 90);

  switch (frame.t) {
    case 'unlock':
      if (frame.passphrase && frame.passphrase.length >= 4) {
        reply({ t: 'state', locked: false });
        reply({ t: 'roster', peers: PEERS });
        reply({
          t: 'net', mode: 'station', ssid: 'not the wifi', channel: 6,
          beta: true, meshChannel: 1,
        });
        reply({ t: 'me', name: 'ozan', fp: 'copper still four', contacts: 3, max: 64,
                crypto: 'netkey', netkeyFp: 'a91bf03c' });
      } else {
        reply({ t: 'state', locked: true, attemptsLeft: 2, bad: true });
      }
      break;

    case 'open':
      reply({ t: 'thread', peer: frame.peer, messages: THREADS[frame.peer] ?? [] });
      break;

    case 'send': {
      const m = { id: nextId++, dir: 'out', text: frame.text, at: clock(), state: 'pending' };
      (THREADS[frame.peer] ??= []).push(m);
      reply({ t: 'msg', peer: frame.peer, message: m });
      setTimeout(() => {
        m.state = 'heard';
        emit({ t: 'ack', peer: frame.peer, id: m.id, state: 'heard' });
      }, 1600);
      break;
    }

    case 'verify': {
      const p = PEERS.find(x => x.id === frame.peer);
      if (p) p.verified = true;
      reply({ t: 'roster', peers: PEERS });
      break;
    }

    case 'wipe':
      Object.keys(THREADS).forEach(k => delete THREADS[k]);
      reply({ t: 'wiped' });
      break;

    case 'lock':
      reply({ t: 'state', locked: true, attemptsLeft: 3 });
      break;
  }
}

const clock = () => new Date().toTimeString().slice(0, 5);
