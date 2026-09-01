# poc-evidence

Evidence-grade PoC capture, deterministic replay and hash-chain timestamping for security researchers and bug bounty reports.

**Pain point.** A bug bounty report typically says: "run this script, you will see the admin token". The triager cannot tell whether that output really came from that script, whether it was edited afterwards, or when it was actually produced. Screenshots can be doctored, terminal output can be pasted from anywhere, and a `script` transcript proves nothing about what produced it. Reports get downgraded or duplicated because evidence is not verifiable.

`poc-evidence` turns a PoC run into a small, self-contained, independently checkable evidence bundle:

- Every capture runs the command **for you** and records what it actually produced — stdout/stderr digests, exit code, input/output file digests, environment (hashed, not stored), platform, duration.
- Records are appended to a local **SHA-256 hash chain**: each record commits to the previous one, so any after-the-fact edit is detectable by recomputation alone — no trusted third party, no server, no account needed.
- `replay` re-runs the recorded command and reports a per-field verdict (`MATCH` / `DRIFT` / `INVALID_INPUTS`), so both you and the triager can prove the PoC still reproduces.
- `bundle` exports a submission-ready directory (`REPORT.md`, `manifest.json`, `VERIFY.txt`, artifacts, `chain.jsonl`) whose file digests can be checked with nothing but `sha256sum` / `certutil`.
- `anchor` commits a chain head into git; pushing that commit turns any git host into a third-party timestamp for your evidence.

Zero dependencies. Node.js ≥ 18 (uses `node:test`, `node:crypto`, ESM).

## Install

Not yet on npm — clone and link:

```sh
git clone https://github.com/HZDF-2026/poc-evidence.git
cd poc-evidence
npm link          # or run directly: node bin/poc-evidence.mjs <command>
```

## Quick start

```sh
mkdir sqli-demo && cd sqli-demo && git init

# 1. Capture: run the PoC and record verifiable evidence of what it did
poc-evidence capture --label "SQLi in /api/users?id" --input fixtures/ \
  --redact 'sk-[A-Z0-9]+' -- node exploit.js

# 2. Prove it reproduces (re-run + per-field digest comparison)
poc-evidence replay cap_001

# 3. Check the evidence chain is intact (recompute every digest)
poc-evidence verify

# 4. Export the submission bundle
poc-evidence bundle cap_001        # -> evidence-cap_001/

# 5. Timestamp: bind the chain head into a git commit, then push
poc-evidence anchor --message "SQLi report for program X"
git push                           # remote commit time = third-party timestamp
```

## Commands

| Command | What it does | Exit code |
|---|---|---|
| `capture [opts] -- <command>` | Run a command, record digests + redacted artifacts, append to chain | the command's own exit code (evidence is still recorded when it fails) |
| `replay <captureId>` | Re-run the recorded command, compare every digest field by field | 0 = `MATCH`, 1 = `DRIFT` / `INVALID_INPUTS` |
| `verify` | Recompute every record hash and chain link | 0 = intact, 1 = `TAMPERED` |
| `bundle <captureId>` | Export `REPORT.md` + `manifest.json` + `VERIFY.txt` + artifacts | 0 |
| `anchor [--message]` | Append anchor record and commit the chain head to git | 0 |

Capture options:

- `--input <path>` — file or directory hashed **before** the run (repeatable). Changed inputs invalidate replays (`INVALID_INPUTS`).
- `--output <path>` — file or directory hashed **after** the run (repeatable). Output files are checked **before** a replay re-runs the command, because they are evidence artifacts the re-run would overwrite.
- `--redact <regex>` — strip matches from captured output before hashing *and* storing (default replacement `[REDACTED]`). Secrets never touch disk.
- `--normalize '<regex>=><replacement>'` — e.g. `'time=\d+=>time=X'`; makes time-varying output replayable by hashing it modulo known noise.
- `--label <text>` — free-form finding label.
- `--env-values` — store environment **values** instead of SHA-256 hashes. Dangerous; off by default.

## How the chain works

Every record — capture, replay, anchor — is one JSON line in `.poc-evidence/chain.jsonl`:

```json
{"id":"cap_001","type":"capture","ts":"2026-09-01T08:00:00.000Z",
 "prev":"000…000","payload":{…},"hash":"<sha256 of the sorted record body>"}
```

- `hash` = SHA-256 of the record body with keys recursively sorted — recomputable by anyone from the line itself.
- `prev` = hash of the previous line (`0`×64 for the first record), so records are linked into a chain.
- `verify` walks the whole file recomputing both. Editing any byte of any older record breaks either its own hash or the link of its successor — the failure names the record and the reason (`record hash mismatch`, `chain link broken`, `malformed JSON`).

Records are never rewritten: replays and anchors are *appended*, so the history of every replay attempt (including failed ones) is itself part of the evidence.

## The bundle

`poc-evidence bundle cap_001` writes `evidence-cap_001/`:

| File | Purpose |
|---|---|
| `REPORT.md` | Human-readable report: command, digests, determinism table, redaction/normalization rules, scope & limits |
| `manifest.json` | Machine-readable: full capture/replay/anchor records + SHA-256 of every file in the bundle |
| `VERIFY.txt` | Step-by-step verification instructions for the reader (no tooling beyond `sha256sum`/`certutil` needed for file digests) |
| `cap_001.stdout.txt` / `cap_001.stderr.txt` | Captured output, **after** redaction |
| `chain.jsonl` | The evidence chain itself, verifiable with `poc-evidence verify` in an empty directory |

A reviewer who distrusts everything can verify the bundle in three independent ways: file digests (`sha256sum`), chain integrity (`poc-evidence verify`), and reproduction (`poc-evidence replay cap_001`).

## Threat model — what this does and does not prove

Written plainly, because overselling evidence tools makes reports worse, not better.

**What it proves:**

- The recorded command produced exactly the recorded digests at capture time.
- The chain has not been edited since each record was appended.
- Pushed anchor commits give third-party timestamps for chain heads.

**What it does NOT prove:**

- **When the vulnerability was discovered.** Only when the evidence was hashed. Nothing stops you from capturing an old exploit today.
- **That the command is honest.** A malicious wrapper can print fabricated output and `poc-evidence` will faithfully hash the fabrication. This tool attests *what a command produced*, not *how the command came to exist*.
- **Who ran it.** No identity is bound to records. Git anchors inherit whatever identity the commits carry.
- **Secrecy of anything matching your redaction patterns is only as good as your patterns.**

`poc-evidence` raises the cost of casual evidence tampering from "edit the paste" to "forge a SHA-256-consistent chain and a git history" — it is not a cryptographic court record. Treat it as a strong anti-tamper format that makes your good-faith report independently checkable.

Also note: `chain.jsonl` contains command lines and file paths. Check repository visibility before pushing anchors.

## Development

```sh
npm test        # 31 tests, zero dependencies, node:test only
```

`tests/cli.test.mjs` exercises the real bin via `spawnSync` — no mocking of the call path.

## License

Apache-2.0 — see [LICENSE](LICENSE).
