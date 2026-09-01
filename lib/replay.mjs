import path from 'node:path';
import { writeFile } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import { sha256Hex, sha256File, applySubstitutions, runCommand } from './util.mjs';
import { appendRecord, readChain, ARTIFACTS_DIR } from './chain.mjs';

export async function runReplay(cwd, captureId) {
  const records = await readChain(cwd);
  const capture = records.find((r) => r.id === captureId && r.type === 'capture');
  if (!capture) throw new Error(`capture not found: ${captureId}`);
  const captured = capture.payload;

  const inputChecks = [];
  for (const file of captured.inputFiles) {
    let actual = null;
    try {
      actual = await sha256File(path.join(cwd, file.path));
    } catch {
      // missing file
    }
    inputChecks.push({ path: file.path, expected: file.sha256, actual, match: actual === file.sha256 });
  }
  const inputsChanged = inputChecks.some((check) => !check.match);

  // Recorded output files are evidence artifacts: check them BEFORE re-running
  // the command, which would otherwise rewrite the very state under test.
  const outputFields = [];
  for (const file of captured.outputFiles) {
    let actual = null;
    try {
      actual = await sha256File(path.join(cwd, file.path));
    } catch {
      // missing file
    }
    outputFields.push({ field: `out:${file.path}`, expected: file.sha256, actual, match: file.sha256 === actual });
  }

  if (!existsSync(captured.cwd)) {
    throw new Error(`recorded working directory no longer exists: ${captured.cwd}`);
  }

  const { stdout, stderr, exitCode } = await runCommand(captured.command, captured.cwd);
  const stdoutFinal = applySubstitutions(applySubstitutions(stdout, captured.redactions), captured.normalizers);
  const stderrFinal = applySubstitutions(applySubstitutions(stderr, captured.redactions), captured.normalizers);

  const stdoutHash = sha256Hex(stdoutFinal);
  const stderrHash = sha256Hex(stderrFinal);
  const fields = [
    { field: 'exitCode', expected: captured.exitCode, actual: exitCode, match: captured.exitCode === exitCode },
    { field: 'stdout', expected: captured.stdoutHash, actual: stdoutHash, match: captured.stdoutHash === stdoutHash },
    { field: 'stderr', expected: captured.stderrHash, actual: stderrHash, match: captured.stderrHash === stderrHash },
    ...outputFields,
  ];

  const allMatch = fields.every((field) => field.match);
  const verdict = inputsChanged ? 'INVALID_INPUTS' : allMatch ? 'MATCH' : 'DRIFT';

  const record = await appendRecord(cwd, 'replay', {
    ref: captureId,
    verdict,
    inputsChanged,
    fields,
    runtime: { platform: process.platform, node: process.versions.node },
  });

  await writeFile(path.join(cwd, ARTIFACTS_DIR, `${record.id}.stdout.txt`), stdoutFinal, 'utf8');
  await writeFile(path.join(cwd, ARTIFACTS_DIR, `${record.id}.stderr.txt`), stderrFinal, 'utf8');

  return { record, verdict, fields, inputChecks };
}
