// Collapses the dashboard into a single file the ESP32 can serve straight out
// of flash: no second request, no bundler, no dependency. Fails the build if
// the result breaks the size budget, because a promise nobody measures is not
// a promise.

import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { gzipSync, constants } from 'node:zlib';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const src = join(here, 'src');
const out = join(here, 'build');

const BUDGET = 80 * 1024;

const read = f => readFileSync(join(src, f), 'utf8');

// --- font, inlined so the board answers one request and is done -------------
const font = readFileSync(join(src, 'fonts', 'baskerville.woff2')).toString('base64');
const css = read('app.css').replace(
  /url\(['"]?fonts\/baskerville\.woff2['"]?\)/,
  `url(data:font/woff2;base64,${font})`,
);

// --- the two modules become one classic script ------------------------------
// Neither file declares a name the other uses, so concatenating inside one
// closure is enough and saves shipping a bundler for two files.
const api = read('api.js')
  .replace(/^export\s+/gm, '')
  .replace(/^export\s+\{[^}]*\};?$/gm, '');

const app = read('app.js').replace(/^import\s+.*?from\s+'\.\/api\.js';?$/m, '');

const js = `(()=>{\n${api}\n${app}\n})();`;

// --- assemble ---------------------------------------------------------------
const html = read('index.html')
  .replace('<link rel="stylesheet" href="app.css">', `<style>${css}</style>`)
  .replace('<script type="module" src="app.js"></script>', `<script>${js}</script>`);

const gz = gzipSync(Buffer.from(html, 'utf8'), { level: constants.Z_BEST_COMPRESSION });

mkdirSync(out, { recursive: true });
writeFileSync(join(out, 'index.html'), html);
writeFileSync(join(out, 'index.html.gz'), gz);

const kb = n => (n / 1024).toFixed(1) + ' KB';
const pct = ((gz.length / BUDGET) * 100).toFixed(0);

console.log(`raw      ${kb(html.length)}`);
console.log(`gzipped  ${kb(gz.length)}   ${pct}% of the ${kb(BUDGET)} budget`);

if (gz.length > BUDGET) {
  console.error(`\nOver budget by ${kb(gz.length - BUDGET)}. The board serves this from flash, so it does not ship.`);
  process.exit(1);
}
