#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check that a game with several updates beside it gets everything from the newest (issue 281).

Usage: check-update-versions.py BUILT_REGISTERED_CACHE_CPP

The argument is the registered_cache.cpp the build writes (headless/CMakeLists.txt, "several
updates"). This compiles its ExternalContentProvider entry callback and its version list with
stand-ins for Eden's types, and feeds it updates in every order: the newest must be the update's
entries and version whatever order the folder lists them in, every version must stay listed, and
DLC must be taken as before.
"""
from pathlib import Path
import os, subprocess, sys, tempfile

source = Path(sys.argv[1]).read_text()
upsert = source[source.index('static void UpsertExternalVersionEntry('):source.index('template <typename EntryMap, typename VersionMap>')]
start = source.index('static bool AddExternalEntriesFromContainer(')
callback = source[source.index('[&entries, &versions,', start):source.index('        });', start) + len('        }')]
assert 'known->second > version' in callback, 'the build does not keep the newest update'

code = r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>
#include <algorithm>
using u32 = std::uint32_t;
using u64 = std::uint64_t;
enum class TitleType { Application, Update, AOC };
enum class ContentRecordType { Meta, Program, Data, Control, HtmlDocument, LegalInformation, DeltaFragment };
using VirtualFile = std::shared_ptr<std::string>;
struct ExternalUpdateEntry {
    u64 title_id;
    u32 version;
    std::string version_string;
    std::array<VirtualFile, 7> files;
};
using EntryMap = std::map<std::tuple<u64, ContentRecordType, TitleType>, VirtualFile>;
using VersionMap = std::map<u64, u32>;
''' + upsert + r'''
struct Provider {
    EntryMap entries;
    VersionMap versions;
    std::vector<ExternalUpdateEntry> multi_version_entries;
    // What one update or DLC file adds, as ForEachContainerEntry calls back for each of its contents.
    void Add(TitleType title_type, u64 title_id, u32 version, const std::string& name) {
        auto& entries = this->entries;
        auto& versions = this->versions;
        auto& multi_version_entries = this->multi_version_entries;
        auto on_entry = ''' + callback + r''';
        for (const auto type : {ContentRecordType::Program, ContentRecordType::Control, ContentRecordType::Meta})
            on_entry(title_type, type, title_id, std::make_shared<std::string>(name), version, name);
    }
    std::string Entry(u64 title_id, ContentRecordType type, TitleType title_type) const {
        const auto it = entries.find({title_id, type, title_type});
        return it == entries.end() ? std::string{} : *it->second;
    }
};

int main() {
    constexpr u64 update = 0x0100152000022800, dlc = 0x0100152000023001;
    const std::vector<std::pair<u32, std::string>> updates = {
        {0x20000, "v2.0.0"}, {0x30000, "v3.0.3"}, {0x10000, "v1.7.0"}};
    // Every order the folder could list three updates in.
    std::vector<int> order = {0, 1, 2};
    int orders = 0;
    do {
        Provider provider;
        for (const int i : order) provider.Add(TitleType::Update, update, updates[i].first, updates[i].second);
        for (const auto type : {ContentRecordType::Program, ContentRecordType::Control, ContentRecordType::Meta})
            assert(provider.Entry(update, type, TitleType::Update) == "v3.0.3");
        assert(provider.versions.at(update) == updates[1].first);
        assert(provider.multi_version_entries.size() == 3);  // all of them stay listed
        ++orders;
    } while (std::next_permutation(order.begin(), order.end()));
    // The same version twice (one file, or a copy): the file found last, as before.
    {
        Provider provider;
        provider.Add(TitleType::Update, update, 0x20000, "first copy");
        provider.Add(TitleType::Update, update, 0x20000, "second copy");
        assert(provider.Entry(update, ContentRecordType::Control, TitleType::Update) == "second copy");
    }
    // DLC is taken as before: the file found last.
    {
        Provider provider;
        provider.Add(TitleType::AOC, dlc, 0x20000, "dlc new");
        provider.Add(TitleType::AOC, dlc, 0x10000, "dlc old");
        assert(provider.Entry(dlc, ContentRecordType::Program, TitleType::AOC) == "dlc old");
        assert(!provider.versions.contains(dlc));
    }
    std::printf("update versions: PASS (%d orders)\n", orders);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / 'update-versions'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra', '-Werror', '-x', 'c++', '-',
                    '-o', str(exe)], input=code, text=True, check=True)
    subprocess.run([str(exe)], check=True)
print('Update versions: PASS the newest of several updates is the update, in every order; all stay listed')
