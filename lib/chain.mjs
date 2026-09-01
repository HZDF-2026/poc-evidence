import { appendFile, mkdir, readFile } from 'node:fs/promises';
import path from 'node:path';
import { sha256Hex, stableStringify, nowIso } from './util.mjs';

export const STORE_DIR = '.poc-evidence';
export const CHAIN_FILE = path.join(STORE_DIR, 'chain.jsonl');
export const ARTIFACTS_DIR = path.join(STORE_DIR, 'artifacts');
export const GENESIS = '0'.repeat(64);

export async function ensureStore(cwd) {
  await mkdir(path.join(cwd, ARTIFACTS_DIR), { recursive: true });
}

export async function readChain(cwd) {
  let text;
  try {
    text = await readFile(path.join(cwd, CHAIN_FILE), 'utf8');
  } catch {
    return [];
  }
  const records = [];
  for (const line of text.split('\n')) {
    if (!line.trim()) continue;
    records.push(JSON.parse(line));
  }
  return records;
}

export async function chainHead(cwd) {
  const records = await readChain(cwd);
  return records.length ? records[records.length - 1].hash : GENESIS;
}

export async function appendRecord(cwd, type, payload) {
  await ensureStore(cwd);
  const records = await readChain(cwd);
  const prev = records.length ? records[records.length - 1].hash : GENESIS;
  const seq = records.filter((r) => r.type === type).length + 1;
  const id = `${type.slice(0, 3)}_${String(seq).padStart(3, '0')}`;
  const body = { id, type, ts: nowIso(), prev, payload };
  const hash = sha256Hex(stableStringify(body));
  const record = { ...body, hash };
  await appendFile(path.join(cwd, CHAIN_FILE), JSON.stringify(record) + '\n', 'utf8');
  return record;
}

export async function verifyChain(cwd) {
  let text;
  try {
    text = await readFile(path.join(cwd, CHAIN_FILE), 'utf8');
  } catch {
    return { ok: true, count: 0, head: GENESIS };
  }
  const lines = text.split('\n').filter((line) => line.trim());
  let prev = GENESIS;
  for (let i = 0; i < lines.length; i++) {
    let record;
    try {
      record = JSON.parse(lines[i]);
    } catch {
      return { ok: false, index: i, id: `line ${i + 1}`, reason: 'malformed JSON' };
    }
    if (!record || typeof record !== 'object' || typeof record.hash !== 'string') {
      return { ok: false, index: i, id: `line ${i + 1}`, reason: 'malformed record' };
    }
    const { hash, ...body } = record;
    if (sha256Hex(stableStringify(body)) !== hash) {
      return { ok: false, index: i, id: record.id, reason: 'record hash mismatch' };
    }
    if (record.prev !== prev) {
      return { ok: false, index: i, id: record.id, reason: 'chain link broken' };
    }
    prev = hash;
  }
  return { ok: true, count: lines.length, head: prev };
}
