// mkcorpus.mjs: collect conformance inputs for libdssl from a checkout of EHS-Studio/dssl (MIT).
//
//   node --permission --allow-fs-read=REPO --allow-fs-read=TOOLS --allow-fs-write=CORPUS \
//        tools/mkcorpus.mjs REPO CORPUS
//
// Pure text processing: nothing from the checkout is evaluated. Inputs:
//   s-*   the stylesheets the project ships (public/styles, package/styles, examples)
//   g-*   every fenced ```dssl block of the guide, readme, usage notes and the native spec
//   f-*   string literals of the project's own test files that look like DSSL (fixtures)
//   h-*   hand-written cases from tools/handcases.txt (one per feature and diagnostic)
//   multi/NAME/NN.dssl   hand-written multi-sheet cases (tokens and motions flow forward)
import { readFileSync, writeFileSync, readdirSync, mkdirSync, existsSync } from 'node:fs';
import { join, basename } from 'node:path';
import { createHash } from 'node:crypto';

const [repo, out, handFile] = process.argv.slice(2);
if (!repo || !out) { console.error('usage: mkcorpus.mjs REPO CORPUS [handcases.txt]'); process.exit(2); }
mkdirSync(out, { recursive: true });
mkdirSync(join(out, 'multi'), { recursive: true });

const seen = new Set();
let count = 0;
function emit(name, text) {
  if (!text.trim()) return;
  const h = createHash('sha1').update(text).digest('hex');
  if (seen.has(h)) return;
  seen.add(h);
  writeFileSync(join(out, name + '.dssl'), text);
  count++;
}
const slug = (s) => s.replace(/[^A-Za-z0-9]+/g, '-').replace(/^-|-$/g, '').toLowerCase();

// s-*: shipped stylesheets
for (const dir of ['public/styles', 'package/styles', 'public/examples']) {
  const d = join(repo, dir);
  if (!existsSync(d)) continue;
  for (const f of readdirSync(d).filter((x) => x.endsWith('.dssl')).sort()) emit('s-' + slug(dir.split('/')[0] + '-' + f.replace(/\.dssl$/, '')), readFileSync(join(d, f), 'utf8'));
}

// g-*: fenced blocks
const docs = ['README.md', 'GUIDE.md', 'engine-package/USAGE.md', 'package/USAGE.md', 'dssl-vscode/README.md', 'dssl-vscode/CHANGELOG.md', 'llms.txt', 'AGENTS.md', 'PARITY.md']
  .concat(readdirSync(join(repo, 'native/spec')).filter((f) => f.endsWith('.md')).map((f) => 'native/spec/' + f));
for (const f of docs) {
  const p = join(repo, f);
  if (!existsSync(p)) continue;
  const text = readFileSync(p, 'utf8');
  let i = 0;
  for (const m of text.matchAll(/```dssl( error)?\n([\s\S]*?)```/g)) { i++; emit(`g-${slug(f)}-${String(i).padStart(3, '0')}`, m[2]); }
}

// f-*: string literals in the project's tests that look like DSSL. A small JS string scanner, no evaluation.
function literals(src) {
  const res = [];
  const n = src.length;
  for (let i = 0; i < n; i++) {
    const c = src[i];
    if (c === '/' && src[i + 1] === '/') { while (i < n && src[i] !== '\n') i++; continue; }
    if (c === '/' && src[i + 1] === '*') { i = src.indexOf('*/', i + 2); if (i < 0) break; i++; continue; }
    if (c === "'" || c === '"' || c === '`') {
      let j = i + 1, s = '', hasInterp = false;
      while (j < n && src[j] !== c) {
        if (src[j] === '\\') {
          const e = src[j + 1];
          if (e === 'n') s += '\n'; else if (e === 't') s += '\t'; else if (e === 'r') s += '\r';
          else if (e === 'u' && /^[0-9a-fA-F]{4}$/.test(src.slice(j + 2, j + 6))) { s += String.fromCharCode(parseInt(src.slice(j + 2, j + 6), 16)); j += 4; }
          else if (e === '\n') { /* line continuation */ } else s += e;
          j += 2; continue;
        }
        if (c === '`' && src[j] === '$' && src[j + 1] === '{') hasInterp = true;
        if (c !== '`' && src[j] === '\n') break; // not a string
        s += src[j]; j++;
      }
      if (src[j] === c && !hasInterp) res.push(s);
      i = j;
    }
  }
  return res;
}
for (const f of readdirSync(join(repo, 'scripts')).filter((x) => /^test-.*\.mjs$/.test(x) && x !== 'test-native-spec.mjs').sort()) {
  const lits = literals(readFileSync(join(repo, 'scripts', f), 'utf8'));
  let i = 0;
  for (const s of lits) {
    if (s.length < 6 || s.length > 60000) continue;
    if (!/[{}]/.test(s) || !/[:{]/.test(s)) continue;
    if (/^[\w./-]+$/.test(s) || /^\s*[<\[]/.test(s)) continue;
    i++; emit(`f-${slug(f.replace(/^test-|\.mjs$/g, ''))}-${String(i).padStart(3, '0')}`, s);
  }
}

// h-*: hand-written; "=== name" starts a case, "=== multi:name" starts a multi-sheet case whose sheets are split by "--- sheet"
if (handFile) {
  const text = readFileSync(handFile, 'utf8');
  const parts = text.split(/^=== (.+)$/m);
  for (let k = 1; k < parts.length; k += 2) {
    const name = parts[k].trim();
    const body = parts[k + 1].replace(/^\n/, '').replace(/\n$/, '') + '\n';
    if (name.startsWith('multi:')) {
      const d = join(out, 'multi', slug(name.slice(6)));
      mkdirSync(d, { recursive: true });
      body.split(/^--- sheet\n/m).filter((x) => x.trim()).forEach((sh, i) => writeFileSync(join(d, String(i + 1).padStart(2, '0') + '.dssl'), sh));
      count++;
    } else {
      // crlf-* cases are written with Windows line ends, bom-* cases start with a byte order mark
      let text = body;
      if (name.startsWith('crlf-')) text = text.replace(/\n/g, '\r\n');
      if (name.startsWith('bom-')) text = '\uFEFF' + text;
      emit('h-' + slug(name), text);
    }
  }
}
console.log(count + ' corpus inputs written to ' + out);
