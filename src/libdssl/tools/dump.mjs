// dump.mjs: write the reference engine's output for every corpus input, in the canonical form of
// native spec chapter 10 (10.3). This is the stand-in for the spec's scripts/native-dump.mjs, which
// the 0.16.3 checkout does not contain.
//
//   node --permission --allow-fs-read=ENGINE --allow-fs-read=TOOLS --allow-fs-read=CORPUS \
//        --allow-fs-write=EXPECTED tools/dump.mjs ENGINE_DIR CORPUS_DIR EXPECTED_DIR
//
// ENGINE_DIR is public/dssl of the checkout. Only parser.js, resolver.js and static.js are loaded
// (they import nothing but each other and pure helpers; they were read before this was run).
//
// Per input NAME.dssl:
//   NAME.l1.json   { errors, tree }       level 1: the syntax tree and the parser's errors (2.9)
//   NAME.json      the resolved document  level 2 (10.2), lint warnings removed (DIA-12)
//   NAME.css       toCSS(doc) of the engine's static lane, only when nothing in the document needs
//                  the engine (so it is the part that lowers to CSS and nothing else)
//   NAME.norm.json the same document with the value of every @texture declaration replaced by the
//                  string "<engine>", written only when texture is the one engine-only property the
//                  sheet uses and the reference reported no error (libdssl does not compute texture
//                  layers, so this checks everything around them)
// Per multi-sheet case multi/NAME/: expected/multi-NAME.json = [doc, doc, ...], resolved in load order
// with the token and motion registries of the engine's index.js carried from sheet to sheet.
import { readFileSync, writeFileSync, readdirSync, mkdirSync, statSync } from 'node:fs';
import { join, basename } from 'node:path';
import { pathToFileURL } from 'node:url';

const [engineDir, corpus, outDir] = process.argv.slice(2);
if (!engineDir || !corpus || !outDir) { console.error('usage: dump.mjs ENGINE_DIR CORPUS_DIR EXPECTED_DIR'); process.exit(2); }
const load = (f) => import(pathToFileURL(join(engineDir, f)).href);
const { parse } = await load('parser.js');
const { resolve, DEFAULT_TOKENS } = await load('resolver.js');
const { toCSS } = await load('static.js');
mkdirSync(outDir, { recursive: true });

// canonical JSON (10.3): sorted keys, undefined omitted, Sets as sorted arrays, Infinity as a string
function canon(v) {
  if (v === undefined) return undefined;
  if (v === null) return null;
  if (v instanceof Set) return [...v].sort().map(canon);
  if (Array.isArray(v)) return v.map((x) => { const c = canon(x); return c === undefined ? null : c; });
  if (typeof v === 'number') return v === Infinity ? 'Infinity' : v === -Infinity ? '-Infinity' : v === 0 ? 0 : v;
  if (typeof v === 'object') {
    const o = {};
    for (const k of Object.keys(v).sort()) { const c = canon(v[k]); if (c !== undefined) o[k] = c; }
    return o;
  }
  return v;
}
const json = (v) => JSON.stringify(canon(v));

const isLint = (w) => /^text contrast is/.test(w.message) || /^the focus outline is removed/.test(w.message) || /the smallest target WCAG 2\.2 allows$/.test(w.message);

function tree(n) {
  if (n.kind === 'decl') return { kind: 'decl', prop: n.prop, value: n.value, line: n.line };
  const o = { ...n };
  o.children = (n.children || []).map(tree);
  return o;
}

function docOf(p, d) {
  return {
    errors: [...p.errors, ...d.errors],
    warnings: (d.warnings || []).filter((w) => !isLint(w)),
    rules: d.rules, shadowRules: d.shadowRules, raws: d.raws, fontLinks: d.fontLinks,
    motionNames: Object.keys(d.motions), motionFrames: d.motionFrames,
    mediaQueries: d.mediaQueries, tokens: d.tokens, registryRule: d.registryRule,
  };
}

let n = 0, crashed = 0;
for (const f of readdirSync(corpus).filter((x) => x.endsWith('.dssl')).sort()) {
  const name = f.replace(/\.dssl$/, '');
  const src = readFileSync(join(corpus, f), 'utf8');
  try {
    const p = parse(src, f);
    const l1 = { errors: p.errors, tree: tree(p.ast) };
    const d = resolve(p.ast, f, { ...DEFAULT_TOKENS }, {});
    writeFileSync(join(outDir, name + '.l1.json'), json(l1) + '\n');
    const doc = docOf(p, d);
    writeFileSync(join(outDir, name + '.json'), json(doc) + '\n');
    {
      const engineProps = new Set();
      for (const r of [...d.rules, ...d.shadowRules]) for (const x of r.decls) if (x.prop.startsWith('@') && !/^@(animate|stack|yield|stagger|overlap|bend|bend-y|bend-up|fold|stretch|bend-around|gradient-flow|tilt)$/.test(x.prop)) engineProps.add(x.prop);
      if (engineProps.size === 1 && engineProps.has('@texture') && !doc.errors.length) {
        const copy = JSON.parse(json(doc));
        for (const r of [...copy.rules, ...copy.shadowRules]) for (const x of r.decls) if (x.prop === '@texture') x.value = '<engine>';
        writeFileSync(join(outDir, name + '.norm.json'), json(copy) + '\n');
      }
    }
    const { css, report } = toCSS(d, { banner: false });
    const pure = !report.engineDecls && !report.skippedSignals && !report.seenStripped && !report.skippedEngine;
    if (pure) writeFileSync(join(outDir, name + '.css'), css);
    n++;
  } catch (e) {
    crashed++;
    console.error('reference engine threw on ' + f + ': ' + (e && e.message));
  }
}

// multi-sheet cases: the registries of index.js (tokens of every sheet, frames of every motion), in load order
const multi = join(corpus, 'multi');
try { statSync(multi); } catch { process.exit(0); }
for (const d of readdirSync(multi).sort()) {
  const files = readdirSync(join(multi, d)).filter((x) => x.endsWith('.dssl')).sort();
  const tokenRegistry = new Map(Object.entries(DEFAULT_TOKENS));
  const motionRegistry = new Map();
  const docs = [];
  for (const f of files) {
    const src = readFileSync(join(multi, d, f), 'utf8');
    const file = d + '-' + f;
    const p = parse(src, file);
    const r = resolve(p.ast, file, Object.fromEntries(tokenRegistry), Object.fromEntries(motionRegistry));
    for (const [k, v] of Object.entries(r.motionFrames || {})) motionRegistry.set(k, v);
    for (const [k, v] of Object.entries(r.tokens || {})) tokenRegistry.set(k, v);
    docs.push(docOf(p, r));
  }
  writeFileSync(join(outDir, 'multi-' + d + '.json'), json(docs) + '\n');
  n++;
}
console.log(`${n} inputs dumped, ${crashed} crashed the reference engine`);
