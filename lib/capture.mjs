import path from 'node:path';
import { writeFile } from 'node:fs/promises';
import { sha256Hex, sha256File, applySubstitutions, collectFiles, runCommand } from './util.mjs';
import { appendRecord, ensureStore, ARTIFACTS_DIR } from './chain.mjs';

export async function runCapture(opts) {
  const {
    cwd = process.cwd(),
    command,
    inputs = [],
    outputs = [],
    redactions = [],
    normalizers = [],
    label = null,
    storeEnvValues = false,
  } = opts;
  if (!Array.isArray(command) || command.length === 0) {
    throw new Error('command must be a non-empty array');
  }

  await ensureStore(cwd);

  const inputFiles = [];
  for (const target of inputs) {
    for (const rel of await collectFiles(target, cwd)) {
      inputFiles.push({ path: rel, sha256: await sha256File(path.join(cwd, rel)) });
    }
  }

  const startedAt = Date.now();
  const { stdout, stderr, exitCode } = await runCommand(command, cwd);
  const durationMs = Date.now() - startedAt;

  const stdoutFinal = applySubstitutions(applySubstitutions(stdout, redactions), normalizers);
  const stderrFinal = applySubstitutions(applySubstitutions(stderr, redactions), normalizers);

  const outputFiles = [];
  for (const target of outputs) {
    for (const rel of await collectFiles(target, cwd)) {
      outputFiles.push({ path: rel, sha256: await sha256File(path.join(cwd, rel)) });
    }
  }

  const env = {};
  for (const key of Object.keys(process.env).sort()) {
    const value = String(process.env[key] ?? '');
    env[key] = storeEnvValues ? value : sha256Hex(value);
  }

  const payload = {
    label,
    command,
    cwd,
    exitCode,
    durationMs,
    stdoutHash: sha256Hex(stdoutFinal),
    stderrHash: sha256Hex(stderrFinal),
    stdoutBytes: Buffer.byteLength(stdoutFinal, 'utf8'),
    stderrBytes: Buffer.byteLength(stderrFinal, 'utf8'),
    envMode: storeEnvValues ? 'values' : 'hashes',
    env,
    redactions,
    normalizers,
    inputFiles,
    outputFiles,
    runtime: {
      platform: process.platform,
      arch: process.arch,
      node: process.versions.node,
    },
  };

  const record = await appendRecord(cwd, 'capture', payload);

  await writeFile(path.join(cwd, ARTIFACTS_DIR, `${record.id}.stdout.txt`), stdoutFinal, 'utf8');
  await writeFile(path.join(cwd, ARTIFACTS_DIR, `${record.id}.stderr.txt`), stderrFinal, 'utf8');

  return record;
}
