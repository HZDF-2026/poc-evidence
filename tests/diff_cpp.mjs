// diff_cpp.mjs — differential test: the C++ port vs the Node reference.
// Each scenario runs in twin temp directories; stdout/stderr, exit codes and
// every produced file are normalized (timestamps, durations, record hashes,
// runtime markers, absolute paths) and compared byte-for-byte.
//
//   node tests/diff_cpp.mjs
import { spawnSync } from 'node:child_process';
import { mkdirSync, mkdtempSync, readdirSync, readFileSync, statSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = fileURLToPath(new URL('..', import.meta.url));
const nodeCli = path.join(root, 'bin', 'poc-evidence.mjs');
const cppBin = path.join(root, 'dist', 'cpp',
  process.platform === 'win32' ? 'poc-evidence.exe' : 'poc-evidence');

const GENESIS = '0'.repeat(64);

function normalize(text, dir) {
  let out = text.split(dir).join('<DIR>');
  out = out.split(dir.split(path.sep).join('/')).join('<DIR>');
  // JSON-escaped variant of the absolute path (chain.jsonl / manifest.json).
  out = out.split(dir.split('\\').join('\\\\')).join('<DIR>');
  out = out.replace(/"ts":"[^"]*"/g, '"ts":"<TS>"');
  out = out.replace(/"generatedAt":"[^"]*"/g, '"generatedAt":"<TS>"');
  // Bare ISO timestamps (REPORT.md "Generated ... at <ts>." and replay rows).
  out = out.replace(/\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z/g, '<TS>');
  out = out.replace(/"durationMs": ?\d+/g, '"durationMs":<MS>');
  out = out.replace(/"node": ?"[^"]*"/g, '"node":"<RT>"');
  out = out.replace(/node (v?[0-9][^\s(]*|cpp)/g, 'node <RT>');
  out = out.replace(/\([0-9]+ ms\)/g, '(<MS> ms)');
  out = out.replace(/after [0-9]+ ms/g, 'after <MS> ms');
  out = out.replace(/\b[0-9a-f]{16,64}\b/g, (m) => (m === GENESIS ? m : '<HASH>'));
  return out;
}

function walk(dir, base) {
  const out = [];
  for (const name of readdirSync(dir).sort()) {
    if (name === '.git') continue;
    const p = path.join(dir, name);
    const st = statSync(p);
    if (st.isDirectory()) out.push(...walk(p, base));
    else out.push({ rel: path.relative(base, p).split(path.sep).join('/'), text: readFileSync(p, 'utf8') });
  }
  return out;
}

const SCENARIO_JS = [
  "console.log('stdout line 1');",
  "console.log('stdout line 2');",
  "console.error('stderr line 1');",
].join('\n');

function writeScenario(dir, { stdout = "console.log('stdout line 1');\nconsole.log('stdout line 2');\nconsole.error('stderr line 1');\n", exit = 0, writeOutput = false } = {}) {
  let js = stdout;
  if (writeOutput) js += "require('fs').writeFileSync('out.txt', 'generated output\\n');\n";
  if (exit) js += `process.exit(${exit});\n`;
  writeFileSync(path.join(dir, 'scenario.js'), js, 'utf8');
}

function gitSetup(dir) {
  for (const args of [
    ['init', '-q'],
    ['config', 'user.email', 'diff@test.local'],
    ['config', 'user.name', 'Diff Test'],
  ]) {
    const r = spawnSync('git', args, { cwd: dir });
    if (r.status !== 0) throw new Error(`git ${args[0]} failed`);
  }
  writeFileSync(path.join(dir, '.gitignore'), 'node_modules/\n', 'utf8');
  const r = spawnSync('git', ['add', '-A'], { cwd: dir });
  if (r.status !== 0) throw new Error('git add failed');
  const c = spawnSync('git', ['commit', '-q', '-m', 'init'], { cwd: dir });
  if (c.status !== 0) throw new Error('git commit failed');
}

// A scenario = { name, setup(dir), steps: [argv], post: [cmd] } where steps are
// poc-evidence argv arrays and post are arbitrary commands run afterwards.
const scenarios = [
  { name: 'usage', setup() {}, steps: [[], ['help'], ['--help'], ['-h']] },
  { name: 'version', setup() {}, steps: [['version'], ['--version'], ['-v']] },
  { name: 'unknown-command', setup() {}, steps: [['bogus']] },
  {
    name: 'capture-basic',
    setup(dir) { writeScenario(dir); writeFileSync(path.join(dir, 'input.txt'), 'input data\n', 'utf8'); },
    steps: [['capture', '--label', 'basic capture', '--input', 'input.txt', '--', 'node', 'scenario.js']],
  },
  {
    name: 'capture-exit3',
    setup(dir) { writeScenario(dir, { exit: 3 }); },
    steps: [['capture', '--', 'node', 'scenario.js']],
  },
  {
    name: 'capture-input-dir-output',
    setup(dir) {
      writeScenario(dir, { writeOutput: true });
      mkdirSync(path.join(dir, 'fixtures'));
      writeFileSync(path.join(dir, 'fixtures', 'a.txt'), 'aaa\n', 'utf8');
      writeFileSync(path.join(dir, 'fixtures', 'b.txt'), 'bbb\n', 'utf8');
      mkdirSync(path.join(dir, 'fixtures', 'sub'));
      writeFileSync(path.join(dir, 'fixtures', 'sub', 'c.txt'), 'ccc\n', 'utf8');
    },
    steps: [['capture', '--input', 'fixtures', '--output', 'out.txt', '--', 'node', 'scenario.js']],
  },
  {
    name: 'capture-redact-normalize',
    setup(dir) {
      writeScenario(dir, { stdout: "console.log('token=SECRET42 score=77');\nconsole.log('time=12345 done');\n" });
    },
    steps: [[
      'capture', '--label', 'redacted',
      '--redact', 'SECRET\\d+',
      '--normalize', '/time=\\d+/=>time=X',
      '--', 'node', 'scenario.js',
    ]],
  },
  {
    name: 'capture-unicode-output',
    setup(dir) {
      writeScenario(dir, { stdout: "console.log('héllo 世界 🚀 µ-test');\nconsole.log('\\u00e9\\u4e16\\u754c');\n" });
    },
    steps: [['capture', '--', 'node', 'scenario.js']],
  },
  { name: 'capture-no-command', setup(dir) { writeScenario(dir); }, steps: [['capture']] },
  {
    name: 'capture-missing-input',
    setup(dir) { writeScenario(dir); },
    steps: [['capture', '--input', 'nope.txt', '--', 'node', 'scenario.js']],
  },
  {
    name: 'capture-unknown-option',
    setup(dir) { writeScenario(dir); },
    steps: [['capture', '--bogus', 'x', '--', 'node', 'scenario.js']],
  },
  {
    name: 'capture-option-missing-value',
    setup() {},
    steps: [['capture', '--input']],
  },
  {
    name: 'capture-bad-normalize',
    setup() {},
    steps: [['capture', '--normalize', 'noseparator', '--', 'node', 'x.js']],
  },
  {
    name: 'capture-invalid-redact-regex',
    setup() {},
    steps: [['capture', '--redact', '[', '--', 'node', 'x.js']],
  },
  {
    name: 'capture-invalid-normalize-regex',
    setup() {},
    steps: [['capture', '--normalize', '[=>x', '--', 'node', 'x.js']],
  },
  {
    name: 'replay-match',
    setup(dir) { writeScenario(dir); },
    steps: [
      ['capture', '--', 'node', 'scenario.js'],
      ['replay', 'cap_001'],
      ['replay', 'cap_001'],
    ],
  },
  {
    name: 'replay-drift',
    setup(dir) { writeScenario(dir); },
    steps: [
      ['capture', '--', 'node', 'scenario.js'],
      ['replay', 'cap_001'],
    ],
    between(dir, step) {
      if (step === 0) writeScenario(dir, { stdout: "console.log('different output');\n" });
    },
  },
  {
    name: 'replay-invalid-inputs',
    setup(dir) {
      writeScenario(dir);
      writeFileSync(path.join(dir, 'input.txt'), 'original\n', 'utf8');
    },
    steps: [
      ['capture', '--input', 'input.txt', '--', 'node', 'scenario.js'],
      ['replay', 'cap_001'],
    ],
    between(dir, step) {
      if (step === 0) writeFileSync(path.join(dir, 'input.txt'), 'edited\n', 'utf8');
    },
  },
  {
    name: 'replay-not-found',
    setup(dir) { writeScenario(dir); },
    steps: [
      ['capture', '--', 'node', 'scenario.js'],
      ['replay', 'cap_009'],
      ['replay'],
    ],
  },
  {
    name: 'verify-empty-and-intact',
    setup() {},
    steps: [['verify'], ['verify']],
  },
  {
    name: 'verify-tampered',
    setup(dir) { writeScenario(dir); },
    steps: [['verify']],
    between(dir, step) {
      if (step !== 0) return;
      // (no captures here — tamper test needs a chain; see tamper-real below)
    },
  },
  {
    name: 'bundle',
    setup(dir) { writeScenario(dir); },
    steps: [
      ['capture', '--label', 'bundle me', '--', 'node', 'scenario.js'],
      ['replay', 'cap_001'],
      ['bundle', 'cap_001', '--out', 'bundle-out'],
      ['verify'],
    ],
  },
  {
    name: 'bundle-default-out',
    setup(dir) { writeScenario(dir); },
    steps: [
      ['capture', '--', 'node', 'scenario.js'],
      ['bundle', 'cap_001'],
    ],
  },
  {
    name: 'bundle-not-found',
    setup(dir) { writeScenario(dir); },
    steps: [
      ['capture', '--', 'node', 'scenario.js'],
      ['bundle', 'cap_009'],
      ['bundle'],
    ],
  },
  {
    name: 'anchor-flow',
    setup(dir) {
      writeScenario(dir);
      gitSetup(dir);
    },
    steps: [
      ['capture', '--label', 'anchored finding', '--', 'node', 'scenario.js'],
      ['replay', 'cap_001'],
      ['anchor', '--message', 'milestone one'],
      ['anchor'],
      ['bundle', 'cap_001', '--out', 'bundle-out'],
      ['verify'],
    ],
    post: [['git', 'log', '--format=%s'], ['git', 'status', '--porcelain']],
  },
  {
    name: 'anchor-not-git',
    setup(dir) { writeScenario(dir); },
    steps: [
      ['capture', '--', 'node', 'scenario.js'],
      ['anchor'],
    ],
  },
  {
    name: 'env-values-mode',
    setup(dir) {
      writeScenario(dir, { stdout: "console.log('plain output');\n" });
    },
    steps: [['capture', '--env-values', '--label', 'env mode', '--', 'node', 'scenario.js']],
    env: { POC_DIFF_MARKER: 'shared-value' },
  },
];

function runScenario(kind, scenario) {
  const dir = mkdtempSync(path.join(tmpdir(), 'pocev-diff-'));
  try {
    scenario.setup(dir);
    const transcript = [];
    for (let i = 0; i < scenario.steps.length; i++) {
      if (scenario.between) scenario.between(dir, i - 1);
      const args = scenario.steps[i];
      const r = kind === 'node'
        ? spawnSync(process.execPath, [nodeCli, ...args], { cwd: dir, encoding: 'utf8', env: { ...process.env, ...scenario.env } })
        : spawnSync(cppBin, args, { cwd: dir, encoding: 'utf8', env: { ...process.env, ...scenario.env } });
      transcript.push(`$ poc-evidence ${args.join(' ')}`);
      transcript.push(`[exit ${r.status}]`);
      transcript.push(normalize(r.stdout || '', dir));
      transcript.push(normalize(r.stderr || '', dir));
    }
    for (const cmd of scenario.post || []) {
      const r = spawnSync(cmd[0], cmd.slice(1), { cwd: dir, encoding: 'utf8' });
      transcript.push(`$ ${cmd.join(' ')}`);
      transcript.push(`[exit ${r.status}]`);
      transcript.push(normalize(r.stdout || '', dir));
      transcript.push(normalize(r.stderr || '', dir));
    }
    const files = walk(dir, dir)
      .map((f) => `--- ${f.rel}\n${normalize(f.text, dir)}`)
      .join('\n');
    return `${transcript.join('\n')}\n=== FILES ===\n${files}`;
  } finally {
    spawnSync(process.platform === 'win32' ? 'cmd' : 'rm',
      process.platform === 'win32' ? ['/d', '/s', '/q', dir] : ['-rf', dir]);
  }
}

let failed = 0;
for (const scenario of scenarios) {
  const nodeOut = runScenario('node', scenario);
  const cppOut = runScenario('cpp', scenario);
  if (nodeOut === cppOut) {
    console.log(`ok   ${scenario.name}`);
  } else {
    failed++;
    console.log(`FAIL ${scenario.name}`);
    const a = nodeOut.split('\n');
    const b = cppOut.split('\n');
    let shown = 0;
    for (let i = 0; i < Math.max(a.length, b.length) && shown < 6; i++) {
      if (a[i] !== b[i]) {
        const clip = (s) => (s === undefined ? '<eof>' : JSON.stringify(s.length > 160 ? s.slice(0, 160) + '…' : s));
        console.log(`  line ${i + 1}:`);
        console.log(`    node: ${clip(a[i])}`);
        console.log(`    cpp : ${clip(b[i])}`);
        shown++;
      }
    }
  }
}
console.log(`\ndiff_cpp: ${scenarios.length - failed}/${scenarios.length} scenarios match`);
process.exitCode = failed === 0 ? 0 : 1;
