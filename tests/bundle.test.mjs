import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import path from 'node:path';
import { runCapture } from '../lib/capture.mjs';
import { runReplay } from '../lib/replay.mjs';
import { buildBundle } from '../lib/bundle.mjs';
import { sha256File } from '../lib/util.mjs';
import { makeTempDir, nodeBin } from './helpers.mjs';

test('bundle contains verifiable artifacts and manifest', async () => {
  const cwd = await makeTempDir();
  const capture = await runCapture({
    cwd,
    command: [nodeBin, '-e', 'console.log("bundle-me")'],
    label: 'RCE in upload',
    outputs: [],
  });
  await runReplay(cwd, capture.id);

  const { out, manifest } = await buildBundle(cwd, capture.id, 'evidence-out');
  assert.equal(manifest.captureId, capture.id);
  assert.equal(manifest.capture.payload.label, 'RCE in upload');
  assert.equal(manifest.replays.length, 1);

  const report = await readFile(path.join(out, 'REPORT.md'), 'utf8');
  assert.ok(report.includes('RCE in upload'));
  assert.ok(report.includes(capture.payload.stdoutHash));
  assert.ok(report.includes('MATCH'));
  assert.ok(report.includes('does NOT prove'));

  const verifyText = await readFile(path.join(out, 'VERIFY.txt'), 'utf8');
  assert.ok(verifyText.includes('certutil -hashfile'));

  const manifestText = await readFile(path.join(out, 'manifest.json'), 'utf8');
  const parsed = JSON.parse(manifestText);
  assert.equal(parsed.tool, 'poc-evidence');
  for (const file of parsed.files) {
    const actual = await sha256File(path.join(out, file.path));
    assert.equal(actual, file.sha256);
  }

  const stdoutArtifact = await readFile(path.join(out, `${capture.id}.stdout.txt`), 'utf8');
  assert.equal(stdoutArtifact, 'bundle-me\n');
});

test('bundle of unknown capture id throws', async () => {
  const cwd = await makeTempDir();
  await assert.rejects(() => buildBundle(cwd, 'cap_404', 'out'), /capture not found/);
});

test('bundle default output directory name includes capture id', async () => {
  const cwd = await makeTempDir();
  const capture = await runCapture({ cwd, command: [nodeBin, '-e', 'console.log(1)'] });
  const { out } = await buildBundle(cwd, capture.id);
  assert.equal(path.basename(out), `evidence-${capture.id}`);
});
