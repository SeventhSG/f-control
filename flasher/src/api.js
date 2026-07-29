// The only place that knows whether a real board is on the other end.
// Everything above this file talks to these four functions and nothing else,
// so the interface runs in a plain browser during development and against
// the Rust backend in the shipped app without changing a line.

// withGlobalTauri is on, so the full JS API is injected as window.__TAURI__
// and we need no bundler and no npm dependency to reach it.
const inTauri = typeof window !== 'undefined' && !!window.__TAURI__;

const invoke = (cmd, args) => window.__TAURI__.core.invoke(cmd, args);

/** Boards currently plugged in. Returns [] when there are none. */
export async function listBoards() {
  if (inTauri) return invoke('list_boards');
  return mock.listBoards();
}

/** Firmware image metadata bundled with this build of the app. */
export async function firmwareInfo() {
  if (inTauri) return invoke('firmware_info');
  return mock.firmwareInfo();
}

/**
 * Write the firmware. Calls onProgress({ phase, done, total }) as it goes.
 * Phases are 'connect', 'erase', 'write', 'verify' in that order.
 * Resolves with the board's first boot credentials.
 */
export async function writeFirmware(port, onProgress) {
  if (inTauri) {
    const un = await window.__TAURI__.event.listen('flash-progress', e => onProgress(e.payload));
    try {
      return await invoke('write_firmware', { port });
    } finally {
      un();
    }
  }
  return mock.writeFirmware(port, onProgress);
}

export function isMock() {
  return !inTauri;
}

// ---------------------------------------------------------------------------
// Development stand-in. Never reached in the packaged app.
// ---------------------------------------------------------------------------

const sleep = ms => new Promise(r => setTimeout(r, ms));

const mock = {
  async listBoards() {
    await sleep(420);
    if (Date.now() % 4 === 0) return [];
    return [{
      port: 'COM7',
      label: 'ESP32 board',
      adapter: 'CP210x',
      certain: false,
    }];
  },

  async firmwareInfo() {
    return {
      version: '0.1.0',
      built: '2026-07-29',
      sha256: '4f1c9e2ab7d05836c41ae9f7b2d3084c19e7a6f5b8c0d2e34a19f7c6b5d80e21',
      bytes: 1_284_096,
    };
  },

  async writeFirmware(port, onProgress) {
    const total = 1_284_096;
    onProgress({ phase: 'connect', done: 0, total });
    await sleep(700);
    onProgress({ phase: 'erase', done: 0, total });
    await sleep(1100);
    for (let done = 0; done < total; done += 41_000) {
      onProgress({ phase: 'write', done: Math.min(done, total), total });
      await sleep(45);
    }
    onProgress({ phase: 'write', done: total, total });
    onProgress({ phase: 'verify', done: total, total });
    await sleep(800);
    return { chip: 'Esp32', mac: 'ec:e3:34:da:c3:a0' };
  },
};
