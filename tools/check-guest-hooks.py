#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise Skyline's code patching and trampoline mapping ABIs in the actual guest CPU."""
from pathlib import Path
import hashlib
import json
import re
import struct
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
cache = Path((root / '.local/headless-cache').read_text().strip())
binary = cache / 'build/bin/eden-headless'
fixtures = (
    ('core-hooks', ('EDEN_GUEST_HOOKS_PASS',)),
    ('core-code-memory', ('EDEN_GUEST_CODE_MEMORY_PASS', 'EDEN_GUEST_PROCESS_CODE_PASS')),
)


def digest(path):
    with path.open('rb') as file:
        return hashlib.file_digest(file, 'sha256').hexdigest()


results = root / 'results'
results.mkdir(exist_ok=True)
run = Path(tempfile.mkdtemp(prefix='guest-hooks-', dir=results))
print(f'Guest hook evidence: {run}', flush=True)
binary_hash = digest(binary)
failures = []
for name, markers in fixtures:
    fixture = root / 'build/fixture' / f'{name}.nro'
    if name == 'core-hooks' and struct.unpack_from('<I', fixture.read_bytes(), 36)[0] != 4096:
        raise SystemExit('Hook fixture must fit one executable page to exercise cached instruction bytes')
    work = run / name
    (work / 'user').mkdir(parents=True)
    timed_out = False
    try:
        result = subprocess.run([str(binary), str(fixture)], cwd=work, capture_output=True,
                                text=True, timeout=45)
        stdout, stderr, status = result.stdout, result.stderr, result.returncode
    except subprocess.TimeoutExpired as error:
        # subprocess.run kills and waits for its child. Retain the evidence and test the other ABI.
        timed_out = True
        stdout = (error.stdout or b'').decode(errors='replace')
        stderr = (error.stderr or b'').decode(errors='replace')
        status = None
    (work / 'stdout.txt').write_text(stdout)
    (work / 'stderr.txt').write_text(stderr)
    console = stdout + stderr
    files = list((work / 'user/log').glob('*.txt'))
    logs = '\n'.join(file.read_text(errors='replace') for file in files) if files else console
    valid = (not timed_out and status == 0 and all(marker in logs for marker in markers)
             and 'EDEN_CORE_FIXTURE_PASS' in logs and 'EDEN_CORE_FIXTURE_FAIL' not in logs
             and 'HEADLESS_COMPLETE\tPASS' in console)
    if name == 'core-hooks':
        valid &= re.findall(r'EDEN_GUEST_HOOK_VALUE=([0-9a-f]{16})', logs) == ['0000000000000002'] * 4
    (work / 'result.json').write_text(json.dumps({
        'case': name, 'passed': bool(valid), 'exit_code': status, 'timed_out': timed_out,
        'binary_sha256': binary_hash, 'fixture_sha256': digest(fixture),
    }, indent=2) + '\n')
    if not valid:
        print(logs[-20000:])
        print(console[-4000:])
        print(f'{name}: FAIL (host exit {status}, timeout={timed_out})', flush=True)
        failures.append(name)
    else:
        print(f'{name}: PASS', flush=True)
if failures:
    raise SystemExit('Guest hook ABI failures: ' + ', '.join(failures))
print('Guest hooks PASS: four-core code patch, RW/RX code memory, process-code trampoline and cleanup')
