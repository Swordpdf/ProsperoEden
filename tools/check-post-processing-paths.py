#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the generated effect and catalog path computations, including failed canonicalization."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile


build = Path(sys.argv[1]).resolve()
source = (build / 'headless/fx_effect.cpp').read_text()
effect = re.search(
    r'FxEffectDesc DescribeEffect\([^\n]*\) \{\n(.*?)\n    desc.name =', source, re.S)
catalog = re.search(
    r'for \(auto& path : found\) \{\n(.*?)\n            if \(folder == own\)', source, re.S)
if not effect or not catalog:
    raise SystemExit('Generated effect/catalog path computations changed')

program = r'''
#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
namespace Common::FS {
std::string PathToUTF8String(const std::filesystem::path& path) {
    return path.generic_string();
}
}
struct FxEffectDesc { std::string file; };
static std::string EffectName(const std::filesystem::path& path,
                              const std::filesystem::path& root) {
EFFECT
    return desc.file;
}
static std::string CatalogName(const std::filesystem::path& path,
                               const std::filesystem::path& folder) {
CATALOG
    return name;
}
int main(int argc, char** argv) {
    assert(argc == 2);
    const std::filesystem::path root = argv[1];
    assert(EffectName(root / "Bloom.fx", root) == "Bloom.fx");
    assert(CatalogName(root / "Bloom.fx", root) == "Bloom.fx");
    assert(EffectName(root / "Sub/Depth.fx", root) == "Sub/Depth.fx");
    assert(CatalogName(root / "Sub/Depth.fx", root) == "Sub/Depth.fx");
    // Preserve successful canonical naming, so aliases and their targets de-duplicate alike.
    assert(EffectName(root / "Alias.fx", root) == "Sub/Depth.fx");
    assert(CatalogName(root / "Alias.fx", root) == "Sub/Depth.fx");
    // A symlink loop forces a real filesystem error on both macOS and Linux, even as root.
    // This exercises the same fallback used when the PS5 refuses canonicalization with EPERM.
    const auto folder = root / "loop";
    const auto path = folder / "Bloom.fx";
    std::error_code error;
    (void)std::filesystem::relative(path, folder, error);
    assert(error);
    assert(EffectName(path, folder) == "Bloom.fx");
    assert(CatalogName(path, folder) == "Bloom.fx");
    std::cout << "Post-processing names PASS: normal, nested, canonical alias, and "
                 "failed-canonicalization fallback\n";
}
'''.replace('EFFECT', effect.group(1)).replace('CATALOG', catalog.group(1))

with tempfile.TemporaryDirectory(prefix='eden-post-paths-') as temporary:
    directory = Path(temporary)
    effects = directory / 'Effects With Spaces'
    (effects / 'Sub').mkdir(parents=True)
    (effects / 'Bloom.fx').write_text('// effect\n')
    (effects / 'Sub/Depth.fx').write_text('// nested effect\n')
    (effects / 'Alias.fx').symlink_to('Sub/Depth.fx')
    (effects / 'loop').symlink_to('loop', target_is_directory=True)
    cpp = directory / 'check.cpp'
    binary = directory / 'check'
    cpp.write_text(program)
    subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary), str(effects)], check=True)
