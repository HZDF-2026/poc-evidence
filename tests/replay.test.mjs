import test from 'node:test';
import assert from 'node:assert/strict';
import { writeFile } from 'node:fs/promises';
import path from 'node:path';
import { runCapture } from '../lib/capture.mjs';
import { runReplay } from '../lib/replay.mjs';
import { readChain } from '../lib/chain.mjs';
import { makeTempDir, nodeBin } from './helpers.mjs';

test('deterministic command replays as MATCH', async () => {
  const cwd = await makeTempDir();
  const capture = await runCapture({
    cwd,
    command: [nodeBin, '-e', 'console.log("stable output")'],
  });
  const result = await runReplay(cwd, capture.id);
  assert.equal(result.verdict, 'MATCH');
  for (const field of result.fields) {
    assert.equal(field.match, true);
  }
  const chain = await readChain(cwd);
  assert.equal(chain.length, 2);
  assert.equal(chain[1].type, 'replay');
  assert.equal(chain[1].payload.ref, capture.id);
  assert.equal(chain[1].payload.verdict, 'MATCH');
});

test('nondeterministic command reports DRIFT on stdout', async () => {
  const cwd = await makeTempDir();
  const capture = await runCapture({
    cwd,
    command: [nodeBin, '-e', 'console.log(Date.now())'],
  });
  await new Promise((resolve) => setTimeout(resolve, 5));
  const result = await runReplay(cwd, capture.id);
  assert.equal(result.verdict, 'DRIFT');
  const stdoutField = result.fields.find((f) => f.field === 'stdout');
  assert.equal(stdoutField.match, false);
});

test('normalization makes time-varying output replayable', async () => {
  const cwd = await makeTempDir();
  const capture = await runCapture({
    cwd,
    command: [nodeBin, '-e', 'console.log("t=" + Date.now())'],
    normalizers: [{ pattern: 't=\\d+', flags: 'g', replacement: 't=X' }],
  });
  await new Promise((resolve) => setTimeout(resolve, 5));
  const result = await runReplay(cwd, capture.id);
  assert.equal(result.verdict, 'MATCH');
});

test('changed input files invalidate the replay', async () => {
  const cwd = await makeTempDir();
  await writeFile(path.join(cwd, 'seed.txt'), 'v1');
  const capture = await runCapture({
    cwd,
    command: [nodeBin, '-e', 'console.log("ok")'],
    inputs: ['seed.txt'],
  });
  await writeFile(path.join(cwd, 'seed.txt'), 'v2');
  const result = await runReplay(cwd, capture.id);
  assert.equal(result.verdict, 'INVALID_INPUTS');
  assert.equal(result.inputChecks[0].match, false);
});

test('missing output file on replay is a per-field drift', async () => {
  const cwd = await makeTempDir();
  const capture = await runCapture({
    cwd,
    command: [nodeBin, '-e', "require('fs').writeFileSync('tmp-out.txt', 'x')"],
    outputs: ['tmp-out.txt'],
  });
  await writeFile(path.join(cwd, 'tmp-out.txt'), 'y');
  const result = await runReplay(cwd, capture.id);
  assert.equal(result.verdict, 'DRIFT');
  const field = result.fields.find((f) => f.field === 'out:tmp-out.txt');
  assert.equal(field.match, false);
});

test('replay of unknown capture id throws', async () => {
  const cwd = await makeTempDir();
  await assert.rejects(() => runReplay(cwd, 'cap_999'), /capture not found/);
});
