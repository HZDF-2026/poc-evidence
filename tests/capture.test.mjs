import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { runCapture } from '../lib/capture.mjs';
import { readChain, ARTIFACTS_DIR } from '../lib/chain.mjs';
import { sha256Hex } from '../lib/util.mjs';
import { makeTempDir, nodeBin } from './helpers.mjs';

test('basic capture records command output with correct digests', async () => {
  const cwd = await makeTempDir();
  const record = await runCapture({
    cwd,
    command: [nodeBin, '-e', 'console.log("poc-evidence-test")'],
    label: 'smoke',
  });
  assert.equal(record.type, 'capture');
  assert.equal(record.payload.label, 'smoke');
  assert.equal(record.payload.exitCode, 0);
  assert.equal(record.payload.stdoutHash, sha256Hex('poc-evidence-test\n'));
  assert.equal(record.payload.stderrHash, sha256Hex(''));
  const artifact = await readFile(path.join(cwd, ARTIFACTS_DIR, `${record.id}.stdout.txt`), 'utf8');
  assert.equal(artifact, 'poc-evidence-test\n');
  const chain = await readChain(cwd);
  assert.equal(chain.length, 1);
});

test('nonzero exit code is recorded faithfully', async () => {
  const cwd = await makeTempDir();
  const record = await runCapture({
    cwd,
    command: [nodeBin, '-e', 'process.exit(3)'],
  });
  assert.equal(record.payload.exitCode, 3);
});

test('redaction strips secrets before hashing and storing', async () => {
  const cwd = await makeTempDir();
  const record = await runCapture({
    cwd,
    command: [nodeBin, '-e', 'console.log("token=sk-SECRET123 done")'],
    redactions: [{ pattern: 'sk-[A-Z0-9]+', flags: 'g', replacement: '[REDACTED]' }],
  });
  assert.equal(record.payload.stdoutHash, sha256Hex('token=[REDACTED] done\n'));
  const artifact = await readFile(path.join(cwd, ARTIFACTS_DIR, `${record.id}.stdout.txt`), 'utf8');
  assert.ok(!artifact.includes('SECRET123'));
  assert.ok(artifact.includes('[REDACTED]'));
});

test('env values are hashed, never stored by default', async () => {
  const cwd = await makeTempDir();
  const record = await runCapture({ cwd, command: [nodeBin, '-e', ''] });
  assert.equal(record.payload.envMode, 'hashes');
  const values = Object.values(record.payload.env);
  assert.ok(values.length > 0);
  for (const value of values) {
    assert.match(value, /^[0-9a-f]{64}$/);
  }
  const chainText = await readFile(path.join(cwd, '.poc-evidence', 'chain.jsonl'), 'utf8');
  assert.ok(!chainText.includes('Path=C:\\'));
});

test('input and output files are hashed', async () => {
  const cwd = await makeTempDir();
  await writeFile(path.join(cwd, 'seed.txt'), 'hello');
  const record = await runCapture({
    cwd,
    command: [nodeBin, '-e', "require('fs').writeFileSync('out.txt', 'result')"],
    inputs: ['seed.txt'],
    outputs: ['out.txt'],
  });
  assert.deepEqual(record.payload.inputFiles, [{ path: 'seed.txt', sha256: sha256Hex('hello') }]);
  assert.deepEqual(record.payload.outputFiles, [{ path: 'out.txt', sha256: sha256Hex('result') }]);
});

test('declared output path that does not exist fails loudly', async () => {
  const cwd = await makeTempDir();
  await assert.rejects(
    () => runCapture({
      cwd,
      command: [nodeBin, '-e', ''],
      outputs: ['missing.txt'],
    }),
    /path not found/,
  );
});
