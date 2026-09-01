import { createHash } from 'node:crypto';
import { createReadStream } from 'node:fs';
import { readFileSync } from 'node:fs';
import { readdir, stat } from 'node:fs/promises';
import { spawn } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

export const VERSION = JSON.parse(
  readFileSync(fileURLToPath(new URL('../package.json', import.meta.url)), 'utf8'),
).version;

export function sha256Hex(data) {
  return createHash('sha256').update(data).digest('hex');
}

export function sha256File(filePath) {
  return new Promise((resolve, reject) => {
    const hash = createHash('sha256');
    const stream = createReadStream(filePath);
    stream.on('data', (chunk) => hash.update(chunk));
    stream.on('end', () => resolve(hash.digest('hex')));
    stream.on('error', reject);
  });
}

export function stableStringify(value) {
  const sortDeep = (v) => {
    if (Array.isArray(v)) return v.map(sortDeep);
    if (v && typeof v === 'object') {
      const out = {};
      for (const key of Object.keys(v).sort()) out[key] = sortDeep(v[key]);
      return out;
    }
    return v;
  };
  return JSON.stringify(sortDeep(value));
}

export const nowIso = () => new Date().toISOString();

export function applySubstitutions(text, substitutions) {
  let out = text;
  for (const sub of substitutions || []) {
    out = out.replace(new RegExp(sub.pattern, sub.flags), sub.replacement);
  }
  return out;
}

export function parseRegexSpec(spec) {
  const match = /^\/(.+)\/([gimsuy]*)$/.exec(spec);
  if (match) {
    const flags = match[2].includes('g') ? match[2] : match[2] + 'g';
    return { pattern: match[1], flags };
  }
  return { pattern: spec, flags: 'g' };
}

export function runCommand(command, cwd) {
  return new Promise((resolve, reject) => {
    const proc = spawn(command[0], command.slice(1), { cwd, shell: false });
    const stdoutChunks = [];
    const stderrChunks = [];
    proc.stdout.on('data', (chunk) => stdoutChunks.push(chunk));
    proc.stderr.on('data', (chunk) => stderrChunks.push(chunk));
    proc.on('error', reject);
    proc.on('close', (code) => resolve({
      stdout: Buffer.concat(stdoutChunks).toString('utf8'),
      stderr: Buffer.concat(stderrChunks).toString('utf8'),
      exitCode: code,
    }));
  });
}

const IGNORED_DIRS = new Set(['.git', 'node_modules', '.poc-evidence']);

export async function collectFiles(target, baseDir) {
  const absolute = path.resolve(baseDir, target);
  const info = await stat(absolute).catch(() => null);
  if (!info) throw new Error(`path not found: ${target}`);
  const files = [];
  const toRel = (absolutePath) => path.relative(baseDir, absolutePath).split(path.sep).join('/');
  if (info.isFile()) {
    files.push(toRel(absolute));
  } else if (info.isDirectory()) {
    const walk = async (dir) => {
      for (const entry of await readdir(dir, { withFileTypes: true })) {
        const entryPath = path.join(dir, entry.name);
        if (entry.isDirectory()) {
          if (IGNORED_DIRS.has(entry.name)) continue;
          await walk(entryPath);
        } else if (entry.isFile()) {
          files.push(toRel(entryPath));
        }
      }
    };
    await walk(absolute);
  } else {
    throw new Error(`unsupported file type: ${target}`);
  }
  files.sort();
  return files;
}
