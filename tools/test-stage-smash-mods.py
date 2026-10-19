#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
import json
from pathlib import Path
import stat
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location('stage_smash', Path(__file__).with_name('stage-smash-mods.py'))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class StageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.skyline = self.archive('skyline.zip', {'exefs/subsdk9': b'NSO0', 'exefs/main.npdm': b'META'})
        self.arcropolis = self.archive('arcropolis.zip', {
            module.CONTENT + 'romfs/skyline/plugins/libarcropolis.nro': b'\0' * 16 + b'NRO0',
            'ultimate/arcropolis/resources/config.json': b'{}',
        })
        self.output = self.root / 'output'

    def archive(self, name, entries):
        path = self.root / name
        with zipfile.ZipFile(path, 'w') as archive:
            for entry, data in entries.items():
                archive.writestr(entry, data)
        return path

    def test_framework_and_character_are_in_separate_mounts(self):
        character = self.root / 'Custom Mario'
        (character / 'fighter/mario').mkdir(parents=True)
        (character / 'fighter/mario/model.bin').write_bytes(b'character')
        self.assertEqual(module.stage(self.skyline, self.arcropolis, [character], self.output), 5)
        self.assertEqual((self.output / module.FRAMEWORK / 'exefs/subsdk9').read_bytes(), b'NSO0')
        self.assertTrue((self.output / module.FRAMEWORK / 'romfs/skyline/plugins/libarcropolis.nro').is_file())
        self.assertEqual((self.output / 'user/sdmc/ultimate/mods/Custom Mario/fighter/mario/model.bin').read_bytes(), b'character')
        self.assertTrue((self.output / 'user/sdmc/ultimate/arcropolis/resources/config.json').is_file())
        manifest = json.loads((self.output / 'smash-mods-manifest.json').read_text())
        self.assertEqual(len(manifest['files']), 5)
        self.assertEqual(len(manifest['archives'][0]['sha256']), 64)

    def test_plugin_only_release_for_smash_13_0_4(self):
        archive = self.archive('arcropolis-v4.0.8.zip', {
            module.CONTENT + 'romfs/skyline/plugins/libarcropolis.nro': b'\0' * 16 + b'NRO0',
        })
        self.assertEqual(module.stage(self.skyline, archive, [], self.output), 3)
        self.assertTrue((self.output / module.FRAMEWORK / 'romfs/skyline/plugins/libarcropolis.nro').is_file())

    def test_companion_plugin_is_in_romfs_not_character_storage(self):
        plugin = self.root / 'libsmashline.nro'
        plugin.write_bytes(b'\0' * 16 + b'NRO0')
        module.stage(self.skyline, self.arcropolis, [], self.output, [plugin])
        self.assertTrue((self.output / module.FRAMEWORK / 'romfs/skyline/plugins/libsmashline.nro').is_file())
        manifest = json.loads((self.output / 'smash-mods-manifest.json').read_text())
        self.assertEqual(manifest['plugins'][0]['name'], plugin.name)

    def test_duplicate_or_invalid_plugin_does_not_write_output(self):
        plugin = self.root / 'libarcropolis.nro'
        plugin.write_bytes(b'\0' * 16 + b'NRO0')
        with self.assertRaises(ValueError):
            module.stage(self.skyline, self.arcropolis, [], self.output, [plugin])
        plugin.write_bytes(b'wrong')
        with self.assertRaises(ValueError):
            module.stage(self.skyline, self.arcropolis, [], self.output, [plugin])
        self.assertFalse(self.output.exists())

    def test_existing_install_is_untouched(self):
        self.output.mkdir()
        sentinel = self.output / 'settings.json'
        sentinel.write_text('keep')
        with self.assertRaises(ValueError):
            module.stage(self.skyline, self.arcropolis, [], self.output)
        self.assertEqual(sentinel.read_text(), 'keep')

    def test_archive_traversal_and_links_are_rejected(self):
        for name in ('../escape', '/absolute', 'exefs/../../escape', 'C:/escape', 'exefs\\escape'):
            with self.subTest(name=name):
                archive = self.archive('unsafe.zip', {name: b'x'})
                with self.assertRaises(ValueError):
                    module.archive_files(archive)
        archive = self.root / 'link.zip'
        info = zipfile.ZipInfo('exefs/link')
        info.external_attr = (stat.S_IFLNK | 0o777) << 16
        with zipfile.ZipFile(archive, 'w') as target:
            target.writestr(info, '/outside')
        with self.assertRaises(ValueError):
            module.archive_files(archive)

    def test_wrong_framework_leaves_no_output(self):
        skyline = self.archive('wrong.zip', {'exefs/subsdk9': b'wrong', 'exefs/main.npdm': b'META'})
        with self.assertRaises(ValueError):
            module.stage(skyline, self.arcropolis, [], self.output)
        self.assertFalse(self.output.exists())

    def test_character_symlink_and_case_collisions_are_rejected(self):
        character = self.root / 'Character'
        character.mkdir()
        (character / 'outside').symlink_to(self.skyline)
        with self.assertRaises(ValueError):
            module.stage(self.skyline, self.arcropolis, [character], self.output)
        (character / 'outside').unlink()
        (character / 'A').write_text('a')
        (character / 'a').write_text('b')
        with self.assertRaises(ValueError):
            module.stage(self.skyline, self.arcropolis, [character], self.output)
        self.assertFalse(self.output.exists())


if __name__ == '__main__':
    unittest.main()
