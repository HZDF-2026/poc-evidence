#!/usr/bin/env node
import path from 'node:path';
import { runCapture } from '../lib/capture.mjs';
import { runReplay } from '../lib/replay.mjs';
import { verifyChain } from '../lib/chain.mjs';
import { buildBundle } from '../lib/bundle.mjs';
import { runAnchor } from '../lib/anchor.mjs';
import { parseRegexSpec, VERSION } from '../lib/util.mjs';

const USAGE = `poc-evidence v${VERSION} — evidence-grade PoC capture, replay & timestamping

Usage:
  poc-evidence capture [options] -- <command> [args...]
      Run a command and record verifiable evidence of what it did.
        --input <path>       file or directory hashed before the run (repeatable)
        --output <path>      file or directory hashed after the run (repeatable)
        --redact <regex>     strip matches from captured output (repeatable,
                             default replacement: [REDACTED])
        --normalize <spec>   '<regex>=><replacement>' applied before hashing
                             (repeatable; regex may be written /pattern/flags)
        --label <text>       free-form label for the finding
        --env-values         store env VALUES instead of hashes (dangerous)

  poc-evidence replay <captureId>
      Re-run a captured command and check every digest (exit 0 = MATCH).

  poc-evidence verify
      Recompute every digest in the evidence chain (exit 0 = intact).

  poc-evidence bundle <captureId> [--out <dir>]
      Export a self-contained evidence bundle for submission.

  poc-evidence anchor [--message <text>]
      Append an anchor record and commit the chain head to git.
      Push to a remote to obtain a third-party timestamp.

Examples:
  poc-evidence capture --label "XSS in /search" --input fixtures/ -- node exploit.js
  poc-evidence capture --normalize 'time=\\d+=>time=X' -- node report.js
  poc-evidence replay cap_001 && poc-evidence bundle cap_001 && poc-evidence anchor

Notes:
  - Captured stdout/stderr are stored redacted; env values are hashed by default.
  - chain.jsonl contains command lines and file paths — check repository
    visibility before pushing anchors.
`;

function fail(message) {
  console.error(`error: ${message}`);
  process.exit(2);
}

function parseOptionToken(argv, i, spec) {
  const token = argv[i];
  let key;
  let value;
  let hasValue = false;
  const eq = token.indexOf('=');
  if (eq > -1) {
    key = token.slice(2, eq);
    value = token.slice(eq + 1);
    hasValue = true;
  } else {
    key = token.slice(2);
  }
  if (spec.bool.includes(key)) {
    return { key, value: hasValue ? value !== 'false' : true, hasValue: true, nextIndex: i + 1 };
  }
  if (!hasValue) {
    value = argv[i + 1];
    if (value === undefined) fail(`--${key} requires a value`);
    return { key, value, hasValue: true, nextIndex: i + 2 };
  }
  return { key, value, hasValue: true, nextIndex: i + 1 };
}

function newFlags(spec) {
  const flags = {};
  for (const key of spec.multi) flags[key] = [];
  for (const key of spec.single) flags[key] = undefined;
  for (const key of spec.bool) flags[key] = false;
  return flags;
}

function applyOption(flags, spec, key, value) {
  if (spec.multi.includes(key)) flags[key].push(value);
  else if (spec.single.includes(key)) flags[key] = value;
  else if (spec.bool.includes(key)) flags[key] = value;
  else fail(`unknown option --${key} (see 'poc-evidence help')`);
}

// capture: options before '--' or before the first positional token; the
// remainder (everything after that point, verbatim) is the command.
function parseCaptureArgs(argv) {
  const spec = { multi: ['input', 'output', 'redact', 'normalize'], single: ['label'], bool: ['env-values'] };
  const flags = newFlags(spec);
  let command = [];
  for (let i = 0; i < argv.length; i++) {
    const token = argv[i];
    if (command.length === 0 && token === '--') {
      command = argv.slice(i + 1);
      break;
    }
    if (command.length === 0 && token.startsWith('--') && token.length > 2) {
      const opt = parseOptionToken(argv, i, spec);
      applyOption(flags, spec, opt.key, opt.value);
      i = opt.nextIndex - 1;
    } else {
      command = argv.slice(i);
      break;
    }
  }
  return { flags, command };
}

function parseSimple(argv, spec = { multi: [], single: [], bool: [] }) {
  const flags = newFlags(spec);
  const positional = [];
  for (let i = 0; i < argv.length; i++) {
    const token = argv[i];
    if (token.startsWith('--') && token.length > 2) {
      const opt = parseOptionToken(argv, i, spec);
      applyOption(flags, spec, opt.key, opt.value);
      i = opt.nextIndex - 1;
    } else {
      positional.push(token);
    }
  }
  return { flags, positional };
}

function buildRedactions(specs) {
  return specs.map((spec) => {
    const { pattern, flags } = parseRegexSpec(spec);
    try {
      new RegExp(pattern, flags);
    } catch (err) {
      fail(`invalid --redact regex "${spec}": ${err.message}`);
    }
    return { pattern, flags, replacement: '[REDACTED]' };
  });
}

function buildNormalizers(specs) {
  return specs.map((spec) => {
    const sep = spec.indexOf('=>');
    if (sep === -1) fail(`--normalize expects '<regex>=><replacement>' (got "${spec}")`);
    const patternPart = spec.slice(0, sep);
    const replacement = spec.slice(sep + 2);
    const { pattern, flags } = parseRegexSpec(patternPart);
    try {
      new RegExp(pattern, flags);
    } catch (err) {
      fail(`invalid --normalize regex "${spec}": ${err.message}`);
    }
    return { pattern, flags, replacement };
  });
}

const [cmd, ...rest] = process.argv.slice(2);

switch (cmd) {
  case 'capture': {
    const { flags, command } = parseCaptureArgs(rest);
    if (command.length === 0) fail('no command given — usage: poc-evidence capture [options] -- <command>');
    let record;
    try {
      record = await runCapture({
        cwd: process.cwd(),
        command,
        inputs: flags.input,
        outputs: flags.output,
        redactions: buildRedactions(flags.redact),
        normalizers: buildNormalizers(flags.normalize),
        label: flags.label ?? null,
        storeEnvValues: flags['env-values'],
      });
    } catch (err) {
      fail(err.message);
    }
    const p = record.payload;
    console.log(`captured ${record.id}${p.label ? ` (${p.label})` : ''}`);
    console.log(`  command : ${p.command.join(' ')}`);
    console.log(`  exit    : ${p.exitCode} (${p.durationMs} ms)`);
    console.log(`  stdout  : sha256:${p.stdoutHash}`);
    console.log(`  stderr  : sha256:${p.stderrHash}`);
    console.log(`  files   : ${p.inputFiles.length} in / ${p.outputFiles.length} out`);
    console.log(`  record  : ${record.hash}`);
    process.exitCode = p.exitCode ?? 1;
    break;
  }

  case 'replay': {
    const { positional } = parseSimple(rest);
    const id = positional[0];
    if (!id) fail('usage: poc-evidence replay <captureId>');
    let result;
    try {
      result = await runReplay(process.cwd(), id);
    } catch (err) {
      fail(err.message);
    }
    console.log(`replay ${result.record.id} of ${id}: ${result.verdict}`);
    for (const field of result.fields) {
      if (field.match) {
        console.log(`  ${field.field.padEnd(24)} match`);
      } else {
        const expected = String(field.expected).slice(0, 16);
        const actual = field.actual === null ? 'missing' : String(field.actual).slice(0, 16);
        console.log(`  ${field.field.padEnd(24)} DRIFT (expected ${expected}… got ${actual}…)`);
      }
    }
    for (const check of result.inputChecks.filter((check) => !check.match)) {
      console.log(`  input ${check.path} changed or missing (expected ${check.expected.slice(0, 16)}…)`);
    }
    process.exitCode = result.verdict === 'MATCH' ? 0 : 1;
    break;
  }

  case 'verify': {
    let verdict;
    try {
      verdict = await verifyChain(process.cwd());
    } catch (err) {
      fail(err.message);
    }
    if (verdict.ok) {
      console.log(`chain intact: ${verdict.count} record(s), head ${verdict.head}`);
    } else {
      console.error(`chain TAMPERED at record ${verdict.index + 1} (${verdict.id}): ${verdict.reason}`);
      process.exitCode = 1;
    }
    break;
  }

  case 'bundle': {
    const { flags, positional } = parseSimple(rest, { multi: [], single: ['out'], bool: [] });
    const id = positional[0];
    if (!id) fail('usage: poc-evidence bundle <captureId> [--out <dir>]');
    let built;
    try {
      built = await buildBundle(process.cwd(), id, flags.out);
    } catch (err) {
      fail(err.message);
    }
    console.log(`bundle written: ${path.relative(process.cwd(), built.out) || built.out}`);
    for (const file of built.manifest.files) {
      console.log(`  ${file.path}  ${file.sha256}`);
    }
    break;
  }

  case 'anchor': {
    const { flags } = parseSimple(rest, { multi: [], single: ['message'], bool: [] });
    let anchored;
    try {
      anchored = await runAnchor(process.cwd(), { message: flags.message ?? null });
    } catch (err) {
      fail(err.message);
    }
    console.log(`anchored head ${anchored.head.slice(0, 16)}… at commit ${anchored.commit.slice(0, 16)}…`);
    console.log('  note: push to a remote to obtain a third-party timestamp');
    console.log('  note: chain.jsonl contains command lines — check repo visibility before pushing');
    break;
  }

  case 'help':
  case '--help':
  case '-h':
  case undefined:
    console.log(USAGE);
    break;

  case 'version':
  case '--version':
  case '-v':
    console.log(VERSION);
    break;

  default:
    fail(`unknown command "${cmd}" (see 'poc-evidence help')`);
}
