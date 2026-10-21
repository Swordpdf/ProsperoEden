#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host check of the NCZ reader (headless/ncz.h) that lets NSZ and XCZ files be played.

It builds headless/ncz_check.cpp with AddressSanitizer and UndefinedBehaviorSanitizer against the
system's zstd and OpenSSL (libcrypto: only the check's own reference encryption uses it) and runs
it: block and solid NCZs made up by the check must read back as the NCAs they were made from.
"""
from pathlib import Path
import os, shutil, subprocess, tempfile

root = Path(__file__).resolve().parents[1]


def flags(package):
    if shutil.which('pkg-config'):
        result = subprocess.run(['pkg-config', '--cflags', '--libs', package], capture_output=True, text=True)
        if result.returncode == 0:
            return result.stdout.split()
    return []


extra = flags('libzstd') + flags('libcrypto')
libraries = [library for library in ('-lzstd', '-lcrypto') if library not in extra]
# Homebrew keeps OpenSSL out of the default paths.
for prefix in ('/opt/homebrew/opt/openssl@3', '/usr/local/opt/openssl@3', '/opt/homebrew', '/usr/local'):
    if Path(prefix, 'include').is_dir():
        extra += ['-I' + prefix + '/include', '-L' + prefix + '/lib']
with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / 'ncz-check'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-I' + str(root / 'headless'),
                    str(root / 'headless/ncz_check.cpp'), *extra, *libraries, '-o', str(exe)],
                   check=True)
    subprocess.run([str(exe)], check=True, timeout=600, env={**os.environ, 'NCZ_CHECK_DIR': tmp})
print('NCZ: PASS block and solid NCZs read back byte for byte (random reads, every section type, '
      'unpacking a large solid one), damaged files rejected')
