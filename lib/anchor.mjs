import { spawnSync } from 'node:child_process';
import path from 'node:path';
import { existsSync } from 'node:fs';
import { appendRecord, chainHead, CHAIN_FILE } from './chain.mjs';

const GIT_CANDIDATES = [
  'git',
  'C:\\Program Files\\Git\\cmd\\git.exe',
  'C:\\Program Files (x86)\\Git\\cmd\\git.exe',
];

export function findGit() {
  for (const candidate of GIT_CANDIDATES) {
    try {
      const result = spawnSync(candidate, ['--version'], { encoding: 'utf8' });
      if (result.status === 0) return candidate;
    } catch {
      // try next candidate
    }
  }
  return null;
}

export async function runAnchor(cwd, { message = null } = {}) {
  if (!existsSync(path.join(cwd, '.git'))) {
    throw new Error('not a git repository — run `git init` first');
  }
  const git = findGit();
  if (!git) throw new Error('git executable not found');

  const head = await chainHead(cwd);
  const record = await appendRecord(cwd, 'anchor', { head, message });

  const subject = `poc-evidence anchor ${head.slice(0, 16)}${message ? `: ${message}` : ''}`;
  const run = (args) => spawnSync(git, args, { cwd, encoding: 'utf8' });

  let result = run(['add', '-f', CHAIN_FILE]);
  if (result.status !== 0) {
    throw new Error(`git add failed: ${result.stderr || result.error?.message || 'unknown error'}`);
  }
  result = run(['commit', '--allow-empty', '-m', subject]);
  if (result.status !== 0) {
    throw new Error(`git commit failed: ${result.stderr || result.error?.message || 'unknown error'}`);
  }
  const commit = run(['rev-parse', 'HEAD']).stdout.trim();

  return { record, commit, head, subject };
}
