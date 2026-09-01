import test from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { runCapture } from '../lib/capture.mjs';
import { runAnchor, findGit } from '../lib/anchor.mjs';
import { readChain } from '../lib/chain.mjs';
import { makeTempDir, nodeBin } from './helpers.mjs';

const git = findGit();
const skip = !git;

test('anchor appends record and commits chain head to git', { skip: skip ? 'git not available' : false }, async () => {
  const cwd = await makeTempDir();
  const run = (args) => spawnSync(git, args, { cwd, encoding: 'utf8' });
  assert.equal(run(['init']).status, 0);
  run(['config', 'user.email', 'test@example.com']);
  run(['config', 'user.name', 'poc-evidence-test']);

  const capture = await runCapture({ cwd, command: [nodeBin, '-e', 'console.log(1)'] });
  const anchored = await runAnchor(cwd, { message: 'first finding' });

  assert.equal(anchored.head, capture.hash);
  assert.match(anchored.subject, /poc-evidence anchor/);

  const chain = await readChain(cwd);
  const anchorRecord = chain.find((r) => r.type === 'anchor');
  assert.equal(anchorRecord.payload.head, capture.hash);
  assert.equal(anchorRecord.payload.message, 'first finding');

  const log = run(['log', '-1', '--format=%s']).stdout;
  assert.match(log, /poc-evidence anchor/);
});

test('anchor outside a git repository throws', { skip }, async () => {
  const cwd = await makeTempDir();
  await assert.rejects(() => runAnchor(cwd, {}), /not a git repository/);
});
