// gen_golden.mjs — generates tests/golden/golden.json from the Node reference
// implementation. The C++ port must reproduce every vector bit-for-bit.
//
//   node tests/golden/gen_golden.mjs
import { mkdirSync, writeFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { sha256Hex, stableStringify, parseRegexSpec } from '../../lib/util.mjs';
import { GENESIS, verifyChain } from '../../lib/chain.mjs';
import { makeTempDir } from '../helpers.mjs';

const root = fileURLToPath(new URL('../../', import.meta.url));

// --------------------------------------------------------------------- sha256
const sha256Vectors = [];
for (const input of [
  '',
  'abc',
  'The quick brown fox jumps over the lazy dog',
  'a'.repeat(10000),
  'héllo 世界 🚀 done',
  'line1\r\nline2\n',
  'µ-shift: Âçñ',
  JSON.stringify({ deep: true }),
]) {
  sha256Vectors.push({ input, hex: sha256Hex(input) });
}

// ---------------------------------------------------------- stableStringify
const stableVectors = [];
for (const value of [
  null,
  true,
  0,
  -0,
  1,
  0.1,
  1 / 3,
  1e21,
  5e-324,
  1.7976931348623157e308,
  9007199254740993,
  'plain',
  'quote " backslash \\ newline \n tab \t ctrl \u0001 esc \u001b',
  'surrogate pair 🚀 and lone \ud800 high \udc00 low',
  [],
  [1, 'two', null, false, [3, [4]]],
  {},
  { z: 1, a: 2, m: 3 },
  { b: { y: 1, x: 2 }, a: [{ q: null, c: 'z' }] },
  { 'é': 1, 'e': 2, 'E': 3, 'A': 4, 'a': 5, '😀': 6 },
  { nested: { deep: { deeper: { key: 'value', 'z': [1, { k: 2 }] } } } },
]) {
  stableVectors.push({ value, out: stableStringify(value) });
}

// ------------------------------------------------------------------ jsonParse
const parseVectors = [];
for (const text of [
  '{"a":1}',
  '{ "a" : [ 1.5e2 , -0.25 ] }',
  '[1,2,3]',
  '[]',
  '{}',
  '"lone high \\ud800"',
  '"lone low \\udc00"',
  '"pair \\ud83d\\ude80"',
  '1e999',
  '-0',
  '0.1',
  '  {"a":1}  ',
  '\t\n[true,false,null]\r\n',
]) {
  let ok = true;
  let out;
  try {
    out = JSON.stringify(JSON.parse(text));
  } catch {
    ok = false;
  }
  parseVectors.push({ text, ok, out });
}
for (const text of [
  '',
  '{',
  '[1,2',
  '{"a":1,}',
  "{'a':1}",
  'undefined',
  'NaN',
  'Infinity',
  '"raw\nnewline"',
  '"\t"',
  '{"a":1} extra',
  '[1,2] [3]',
  '+1',
  '.5',
  '01',
  '"\\x41"',
  '{"a" 1}',
  '"unterminated',
]) {
  parseVectors.push({ text, ok: false, out: null });
}

// ------------------------------------------------------------ parseRegexSpec
const regexSpecVectors = [];
for (const spec of [
  'plain',
  '/abc/',
  '/abc/g',
  '/abc/i',
  '/abc/gi',
  '/abc/m',
  '/abc/u',
  '/abc/uy',
  '/a\\/b/',
  '/\\/x\\//g',
  '/[]/',
  'weird//path',
  '/plain',
  'plain/',
]) {
  const { pattern, flags } = parseRegexSpec(spec);
  regexSpecVectors.push({ spec, pattern, flags });
}

// ------------------------------------------------------- String.replace (V8)
const replaceVectors = [];
for (const [subject, pattern, flags, replacement] of [
  ['hello world', 'o', 'g', '0'],
  ['hello world', 'o', '', '0'],
  ['abc', 'b', 'g', '[$&]'],
  ['abc', 'b', 'g', "<$`|$'"],
  ['abc', 'b', 'g', '$$'],
  ['abc', 'b', 'g', '[$1]'],
  ['2026-09-04', '(\\d+)-(\\d+)-(\\d+)', 'g', '$3/$2/$1'],
  ['abc', '(b)', 'g', '[$2]'],
  ['ab', '(?<x>a)(?<y>b)', '', '$<y>$<x>'],
  ['ab', '(?<x>a)(?<y>b)', '', '[$<z>]'],
  ['abc', '(?=b)', 'g', '-'],
  ['abc', 'a(?!c)', 'g', '-'],
  ['abc', '(?<=a)b', '', 'X'],
  ['abc', '(?<!a)b', '', 'X'],
  ['aa', 'a*', 'g', '-'],
  ['abab', 'a.', 'y', '-'],
  ['abab', 'a.', 'gy', '-'],
  ['a\nb', 'a.b', 's', 'X'],
  ['a\nb', 'a.b', '', 'X'],
  ['a\nb', '^b', 'm', 'X'],
  ['hello', '(l)(l)', 'g', '$2$1'],
  ['xax', '(a)|x', 'g', '[$1]'],
  ['Aµ K', 'µ', 'gi', 'U'],
  ['ΣΊΣΥΦΟΣ', 'σίσυφος', 'i', 'X'],
  ['kK', 'k', 'gi', 'K'],
  ['abc', '(a)(b)?(c)', 'g', '<$1|$2|$3>'],
  ['--', '-', 'g', '[$`][$\']'],
  ['one two', '\\btwo\\b', 'g', '2'],
  ['aaa', '(?=a)', 'g', 'b'],
  ['abc', 'b', 'g', '$0'],
  ['abc', 'b', 'g', '$&$&'],
  ['path/to/file', '/', 'g', '$`$&$\''],
  ['ab', '(a)|(b)', 'g', '$1$2'],
  ['zz', '(z)\\1', 'g', '($1)'],
]) {
  let out;
  let error = null;
  try {
    out = subject.replace(new RegExp(pattern, flags), replacement);
  } catch (e) {
    error = String(e.message);
  }
  replaceVectors.push({ subject, pattern, flags, replacement, out, error });
}

// ----------------------------------------------------------------- regexError
const regexErrorVectors = [];
for (const [pattern, flags] of [
  ['[', 'g'],
  ['(', 'g'],
  ['a)', 'g'],
  ['[a-', 'g'],
  ['(?<a>x)(?<a>y)', 'g'],
  ['a{2,1}', 'g'],
  ['\\', 'g'],
  ['(?<a>x)\\k<b>', 'g'],
  ['a**', 'g'],
  ['[z-a]', 'g'],
  ['(?i)x', 'g'],
  ['(*)', 'g'],
  ['\\p{Foo}', 'gu'],
  ['\\p{Foo}', 'g'],
  ['(?<1a>x)', 'g'],
  ['(?P<n>x)', 'g'],
  ['a{1000000}', 'g'],
  ['[\\', 'g'],
  [')', 'g'],
  ['x{2,1}{3,4}', 'g'],
]) {
  try {
    new RegExp(pattern, flags);
  } catch (e) {
    regexErrorVectors.push({ pattern, flags, message: e.message });
  }
}

// ---------------------------------------------------------------------- chain
const fixedRecord = (id, type, ts, prev, payload) => {
  const body = { id, type, ts, prev, payload };
  const hash = sha256Hex(stableStringify(body));
  return { ...body, hash };
};
const mk = (type, n, ts, prev, payload) =>
  fixedRecord(`${type.slice(0, 3)}_${String(n).padStart(3, '0')}`, type, ts, prev, payload);

const r1 = mk('capture', 1, '2026-01-02T03:04:05.678Z', GENESIS, {
  zeta: 'last',
  alpha: 1,
  mid: { yy: 2, xx: 3 },
});
const r2 = mk('replay', 1, '2026-01-02T03:04:06.000Z', r1.hash, { ok: true, count: 1 });
const r3 = mk(
  'capture',
  2,
  '2026-01-02T03:04:07.123Z',
  r2.hash,
  { 'é': 1, e: 2, '😀': [1, 2, { k: null }] },
);
const intactText = [r1, r2, r3].map((r) => JSON.stringify(r)).join('\n') + '\n';

const tampered = { ...r2, payload: { ok: false, count: 1 } };
const tamperedText = [r1, tampered, r3].map((r) => JSON.stringify(r)).join('\n') + '\n';

const relinkedBody = { id: r3.id, type: r3.type, ts: r3.ts, prev: GENESIS, payload: r3.payload };
const relinked = { ...relinkedBody, hash: sha256Hex(stableStringify(relinkedBody)) };
const relinkedText = [r1, r2, relinked].map((r) => JSON.stringify(r)).join('\n') + '\n';

const noHashText = [JSON.stringify(r1), JSON.stringify({ id: 'rep_001', type: 'replay' })].join('\n') + '\n';
const malformedText = JSON.stringify(r1) + '\nnot json\n';
const blankText = '\n\n';

const chainVectors = [];
for (const text of [intactText, tamperedText, relinkedText, noHashText, malformedText, blankText]) {
  const cwd = await makeTempDir();
  const fs = await import('node:fs/promises');
  await fs.mkdir(path.join(cwd, '.poc-evidence'), { recursive: true });
  await fs.writeFile(path.join(cwd, '.poc-evidence', 'chain.jsonl'), text, 'utf8');
  const expect = await verifyChain(cwd);
  await fs.rm(cwd, { recursive: true, force: true });
  chainVectors.push({ text, expect: JSON.parse(JSON.stringify(expect)) });
}

// ------------------------------------------------------------------------ out
const golden = {
  sha256: sha256Vectors,
  stableStringify: stableVectors,
  jsonParse: parseVectors,
  parseRegexSpec: regexSpecVectors,
  replace: replaceVectors,
  regexError: regexErrorVectors,
  chain: chainVectors,
};

const dir = path.join(root, 'tests', 'golden');
mkdirSync(dir, { recursive: true });
writeFileSync(path.join(dir, 'golden.json'), JSON.stringify(golden, null, 2) + '\n', 'utf8');
console.log(`golden.json: ${sha256Vectors.length} sha256, ${stableVectors.length} stableStringify, ` +
  `${parseVectors.length} jsonParse, ${regexSpecVectors.length} parseRegexSpec, ` +
  `${replaceVectors.length} replace, ${chainVectors.length} chain vectors`);
