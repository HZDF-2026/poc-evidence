import { mkdtemp } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';

export async function makeTempDir() {
  return mkdtemp(path.join(tmpdir(), 'poc-evd-'));
}

export const nodeBin = process.execPath;
