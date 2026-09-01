import test from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { readFile, writeFile } from 'node:fs/promises';
import { mkdirSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { makeTempDir, nodeBin } from './helpers.mjs';

const BIN = fileURLToPath(new URL('../bin/poc-evidence.mjs', import.meta.url));

function cli(args, cwd) {
  return spawnSync(nodeBin, [BIN, ...args], { cwd, encoding: 'utf8' });
}

test('cli smoke: capture, replay, verify real call path', () => {
  const cwd = makeTempDirSync();
  const captured = cli(['capture', '--label', 'cli-smoke', '--', nodeBin, '-e', 'console.log("cli-ok")'], cwd);
  assert.equal(captured.status, 0, captured.stderr);
  assert.match(captured.stdout, /captured cap_001/);
  assert.match(captured.stdout, /cli-smoke/);

  const replayed = cli(['replay', 'cap_001'], cwd);
  assert.equal(replayed.status, 0, replayed.stderr);
  assert.match(replayed.stdout, /MATCH/);

  const verified = cli(['verify'], cwd);
  assert.equal(verified.status, 0, verified.stderr);
  assert.match(verified.stdout, /chain intact: 2 record/);
});

test('cli propagates PoC exit code while still recording evidence', async () => {
  const cwd = await makeTempDir();
  const result = cli(['capture', '--', nodeBin, '-e', 'process.exit(4)'], cwd);
  assert.equal(result.status, 4);
  const chain = await readFile(path.join(cwd, '.poc-evidence', 'chain.jsonl'), 'utf8');
  assert.match(chain, /"exitCode":4/);
});

test('cli replay exits 1 on drift', () => {
  const cwd = makeTempDirSync();
  cli(['capture', '--', nodeBin, '-e', 'console.log(Date.now())'], cwd);
  const result = cli(['replay', 'cap_001'], cwd);
  assert.equal(result.status, 1);
  assert.match(result.stdout, /DRIFT/);
});

test('cli verify exits 1 on tampered chain', async () => {
  const cwd = await makeTempDir();
  cli(['capture', '--', nodeBin, '-e', 'console.log("x")'], cwd);
  const file = path.join(cwd, '.poc-evidence', 'chain.jsonl');
  const lines = (await readFile(file, 'utf8')).trim().split('\n');
  const record = JSON.parse(lines[0]);
  record.hash = record.hash.slice(0, -1) + (record.hash.endsWith('0') ? '1' : '0');
  lines[0] = JSON.stringify(record);
  await writeFile(file, lines.join('\n') + '\n', 'utf8');
  const result = cli(['verify'], cwd);
  assert.equal(result.status, 1);
  assert.match(result.stderr, /TAMPERED/);
});

test('cli normalize flag enables replay of time-varying output', () => {
  const cwd = makeTempDirSync();
  const captured = cli(
    ['capture', '--normalize', 't=\\d+=>t=X', '--', nodeBin, '-e', 'console.log("t=" + Date.now())'],
    cwd,
  );
  assert.equal(captured.status, 0, captured.stderr);
  const replayed = cli(['replay', 'cap_001'], cwd);
  assert.equal(replayed.status, 0, replayed.stderr);
  assert.match(replayed.stdout, /MATCH/);
});

test('cli redact flag strips secrets from artifacts', async () => {
  const cwd = await makeTempDir();
  const result = cli(
    ['capture', '--redact', 'sk-[A-Z0-9]+', '--', nodeBin, '-e', 'console.log("key=sk-ABC123")'],
    cwd,
  );
  assert.equal(result.status, 0, result.stderr);
  const artifact = await readFile(path.join(cwd, '.poc-evidence', 'artifacts', 'cap_001.stdout.txt'), 'utf8');
  assert.ok(!artifact.includes('sk-ABC123'));
  assert.ok(artifact.includes('[REDACTED]'));
});

test('cli bundle writes report, manifest and verify instructions', async () => {
  const cwd = await makeTempDir();
  cli(['capture', '--', nodeBin, '-e', 'console.log("b")'], cwd);
  const result = cli(['bundle', 'cap_001'], cwd);
  assert.equal(result.status, 0, result.stderr);
  assert.match(result.stdout, /evidence-cap_001/);
  for (const name of ['REPORT.md', 'manifest.json', 'VERIFY.txt', 'chain.jsonl']) {
    await readFile(path.join(cwd, 'evidence-cap_001', name), 'utf8');
  }
});

test('cli unknown command and missing values fail with exit 2', () => {
  const cwd = makeTempDirSync();
  assert.equal(cli(['frobnicate'], cwd).status, 2);
  assert.equal(cli(['capture', '--label'], cwd).status, 2);
  assert.equal(cli(['capture'], cwd).status, 2);
  assert.equal(cli(['replay'], cwd).status, 2);
});

test('cli help and version exit 0', () => {
  const cwd = makeTempDirSync();
  assert.match(cli(['help'], cwd).stdout, /poc-evidence v/);
  assert.match(cli(['--version'], cwd).stdout, /^\d+\.\d+\.\d+/);
});

function makeTempDirSync() {
  const dir = path.join(
    tmpdir(),
    `poc-evd-sync-${process.pid}-${Date.now()}-${Math.random().toString(36).slice(2)}`,
  );
  mkdirSync(dir, { recursive: true });
  return dir;
}
