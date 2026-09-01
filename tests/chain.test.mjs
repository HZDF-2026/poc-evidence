import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { appendRecord, verifyChain, GENESIS, CHAIN_FILE } from '../lib/chain.mjs';
import { sha256Hex, stableStringify } from '../lib/util.mjs';
import { makeTempDir } from './helpers.mjs';

test('append + verify roundtrip with correct linking', async () => {
  const cwd = await makeTempDir();
  const r1 = await appendRecord(cwd, 'capture', { a: 1 });
  const r2 = await appendRecord(cwd, 'replay', { b: 2 });
  assert.equal(r1.id, 'cap_001');
  assert.equal(r2.id, 'rep_001');
  assert.equal(r1.prev, GENESIS);
  assert.equal(r2.prev, r1.hash);
  const verdict = await verifyChain(cwd);
  assert.equal(verdict.ok, true);
  assert.equal(verdict.count, 2);
  assert.equal(verdict.head, r2.hash);
});

test('verify on empty store reports intact with genesis head', async () => {
  const cwd = await makeTempDir();
  const verdict = await verifyChain(cwd);
  assert.equal(verdict.ok, true);
  assert.equal(verdict.count, 0);
  assert.equal(verdict.head, GENESIS);
});

test('tampering with a payload is detected', async () => {
  const cwd = await makeTempDir();
  await appendRecord(cwd, 'capture', { secret: 'original' });
  const file = path.join(cwd, CHAIN_FILE);
  const text = await readFile(file, 'utf8');
  await writeFile(file, text.replace('original', 'edited'), 'utf8');
  const verdict = await verifyChain(cwd);
  assert.equal(verdict.ok, false);
  assert.equal(verdict.reason, 'record hash mismatch');
});

test('relinking a record (with recomputed hash) is detected as broken link', async () => {
  const cwd = await makeTempDir();
  const r1 = await appendRecord(cwd, 'capture', { a: 1 });
  await appendRecord(cwd, 'capture', { a: 2 });
  const file = path.join(cwd, CHAIN_FILE);
  const lines = (await readFile(file, 'utf8')).trim().split('\n');
  const second = JSON.parse(lines[1]);
  assert.equal(second.prev, r1.hash);
  second.prev = GENESIS;
  const { hash, ...body } = second;
  second.hash = sha256Hex(stableStringify(body));
  lines[1] = JSON.stringify(second);
  await writeFile(file, lines.join('\n') + '\n', 'utf8');
  const verdict = await verifyChain(cwd);
  assert.equal(verdict.ok, false);
  assert.equal(verdict.reason, 'chain link broken');
});

test('malformed JSON line is reported, not thrown', async () => {
  const cwd = await makeTempDir();
  await appendRecord(cwd, 'capture', { a: 1 });
  const file = path.join(cwd, CHAIN_FILE);
  const text = await readFile(file, 'utf8');
  await writeFile(file, text + 'not json\n', 'utf8');
  const verdict = await verifyChain(cwd);
  assert.equal(verdict.ok, false);
  assert.equal(verdict.reason, 'malformed JSON');
});
