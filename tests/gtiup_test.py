#!/usr/bin/env python3
"""Offline acceptance of managed downloads, selection, proxies and unsafe inputs."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('gtiup', ROOT / 'scripts/gtiup.py')
g = importlib.util.module_from_spec(spec)
spec.loader.exec_module(g)
SHA = 'a' * 40


class ManagerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.manager = g.Manager(self.root / 'home')
        self.env = patch.dict(os.environ, {'GTIUP_HOME': str(self.manager.home), 'GTIUP_TOOLCHAIN': ''})
        self.env.start(); self.addCleanup(self.env.stop)
        self.assets = self.root / 'assets'; self.assets.mkdir()

    def archive(self, channel='nightly', identity=SHA, missing=None, version='0.1.0'):
        name = f'gti-{channel}-linux-x64.tar.gz'
        path = self.assets / name
        contents = {'bin/gti': '#!/bin/sh\nprintf "compiler:%s\\n" "$*"\n',
                    'bin/gti_lsp': '#!/bin/sh\nprintf "lsp:%s\\n" "$*"\n',
                    'share/gti/VERSION': version, 'share/gti/BUILD_ID': identity,
                    'share/gti/PREVIEW_SOURCE': identity,
                    'share/gti/preview.json': json.dumps({'schema': 1, 'features': [{'task': 'example'}]})}
        with tarfile.open(path, 'w:gz') as t:
            for name, data in contents.items():
                if name == missing: continue
                data = data.encode(); info = tarfile.TarInfo(name)
                info.size = len(data); info.mode = 0o755 if name.startswith('bin/') else 0o644
                t.addfile(info, io.BytesIO(data))
        (self.assets / (path.name + '.sha256')).write_text(g.sha256(path) + '  ' + path.name + '\n')
        (self.assets / 'BUILD_ID').write_text(SHA)
        return path

    def install(self, channel='nightly'):
        def download(url, destination, limit=0):
            self.assertTrue(url.startswith(g.DOWNLOAD + channel + '/'))
            shutil.copyfile(self.assets / url.rsplit('/', 1)[-1], destination)
        with patch.object(g, 'release', return_value=(channel, 'gti-' + channel, channel)), \
             patch.object(g, 'host', return_value='linux-x64'), patch.object(g, 'download', side_effect=download):
            self.manager.install(channel)

    def test_complete_channels_and_proxy_selection(self):
        self.archive(); self.install()
        self.archive('preview'); self.install('preview')
        state = self.manager.state(); state['default'] = 'nightly'; self.manager.save(state)
        self.assertEqual(self.manager.which('gti_lsp').name, 'gti_lsp')
        self.manager.bootstrap()
        project = self.root / 'project'; project.mkdir()
        (project / 'gti-toolchain').write_text('preview\n')
        nested = project / 'nested'; nested.mkdir()
        def execute(tool, *args, env=None):
            return subprocess.check_output([str(self.manager.home / 'bin' / tool), *args], cwd=nested,
                                           env=env or os.environ.copy(), text=True).strip()
        self.assertIn('preview-', execute('gtiup', 'which', 'gti'))
        self.assertEqual(execute('gti', 'file with spaces.gti', '--help'), 'compiler:file with spaces.gti --help')
        self.assertEqual(execute('gti_lsp', '+nightly', '--version'), 'lsp:--version')
        self.assertEqual(execute('gtiup', 'run', 'nightly', 'gti', '--version'), 'compiler:--version')
        self.assertIn('nightly-', execute('gtiup', 'which', 'gti', env={**os.environ, 'GTIUP_TOOLCHAIN': 'nightly'}))
        (project / 'gti-toolchain').write_text('bad')
        result = subprocess.run([str(self.manager.home / 'bin/gti'), '--help'], cwd=nested, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'expected preview', result.stderr)

    def test_failed_update_preserves_active_selection(self):
        self.archive(); self.install()
        before = self.manager.state()
        archive = self.archive(identity='b' * 40)
        with self.assertRaisesRegex(ValueError, 'changed during download'):
            self.install()
        self.assertEqual(self.manager.state(), before)
        archive.write_bytes(b'corrupted')
        with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
            self.install()
        self.assertEqual(self.manager.state(), before)
        self.assertFalse(list((self.manager.home / 'toolchains').glob('.install-*')))

    def test_missing_tool_and_preview_metadata(self):
        self.archive(missing='bin/gti_lsp')
        with self.assertRaisesRegex(ValueError, 'lacks executable'):
            self.install()
        self.archive('preview', missing='share/gti/preview.json')
        with self.assertRaises(FileNotFoundError): self.install('preview')
        self.assertEqual(self.manager.state()['toolchains'], {})

    def test_gc_preserves_active_and_unmanaged_directories(self):
        self.archive(); self.install()
        store = self.manager.home / 'toolchains'
        active = self.manager.which('gti', 'nightly').parent.parent
        old = store / ('nightly-' + '0' * 64); old.mkdir()
        unmanaged = store / 'user-work'; unmanaged.mkdir()
        with patch.dict(os.environ, {'GTIUP_HOME': str(self.manager.home), 'GTIUP_TOOLCHAIN': ''}):
            g.main(['gc'])
        self.assertTrue(active.exists()); self.assertTrue(unmanaged.exists())
        self.assertFalse(old.exists())

    def test_existing_commands_are_preserved(self):
        folder = self.manager.home / 'bin'; folder.mkdir(parents=True)
        (folder / 'gti').write_text('user executable')
        with self.assertRaisesRegex(ValueError, 'refusing to overwrite'):
            self.manager.bootstrap()
        self.assertEqual((folder / 'gti').read_text(), 'user executable')

    def test_unsafe_archive_never_extracts(self):
        cases = [('../outside', None), ('/absolute', None), ('link', '/outside'),
                 ('link', '../outside'), ('fifo', 'FIFO')]
        for name, link in cases:
            path = self.assets / 'bad.tar.gz'
            with tarfile.open(path, 'w:gz') as t:
                info = tarfile.TarInfo(name)
                if link == 'FIFO': info.type = tarfile.FIFOTYPE
                elif link: info.type = tarfile.SYMTYPE; info.linkname = link
                t.addfile(info)
            destination = self.root / 'stage'; destination.mkdir(exist_ok=True)
            with self.assertRaises(ValueError): g.extract(path, destination)
            self.assertEqual(list(destination.iterdir()), [])
        # Even contained links may not become a parent of subsequent writes.
        with tarfile.open(path, 'w:gz') as t:
            info = tarfile.TarInfo('link'); info.type = tarfile.SYMTYPE; info.linkname = 'actual'; t.addfile(info)
            t.addfile(tarfile.TarInfo('link/child'))
        with self.assertRaisesRegex(ValueError, 'through symlink'): g.extract(path, destination)

    def test_local_selection_never_downloads(self):
        with patch.object(g, 'download', side_effect=AssertionError('unexpected network')):
            with self.assertRaisesRegex(ValueError, 'not installed'):
                self.manager.which('gti', 'preview')
            with self.assertRaisesRegex(ValueError, 'no default'):
                self.manager.selected()


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.status = {'schema': 1, 'stable': ['0.1.0', '0.2.0'], 'yanked': [], 'withdrawn': []}
        self.beta = [{'tag_name': 'v0.3.0-beta.2', 'draft': False, 'prerelease': True}]

    def request(self, url):
        if url == g.STATUS: return self.status
        if '?per_page' in url: return self.beta
        tag = url.rsplit('/', 1)[-1]
        return {'tag_name': tag, 'draft': False, 'prerelease': '-beta.' in tag or tag in ('nightly', 'preview')}

    def test_channels_and_pins(self):
        with patch.object(g, 'remote_json', side_effect=self.request):
            self.assertEqual(g.release('stable'), ('v0.2.0', 'gti-v0.2.0', '0.2.0'))
            self.assertEqual(g.release('beta')[0], 'v0.3.0-beta.2')
            self.assertEqual(g.release('0.1.0')[0], 'v0.1.0')
            self.assertEqual(g.release('preview')[1], 'gti-preview')
            self.assertEqual(g.release('nightly')[1], 'gti-nightly')

    def test_revocations_fail_without_fallback(self):
        with patch.object(g, 'remote_json', side_effect=self.request):
            self.status['yanked'] = [{'version': '0.2.0'}]
            with self.assertRaisesRegex(ValueError, 'yanked'): g.release('stable')
            self.status['withdrawn'] = [{'tag': 'v0.3.0-beta.2'}]
            with self.assertRaisesRegex(ValueError, 'withdrawn'): g.release('beta')
            self.status['stable'] = []
            with self.assertRaisesRegex(ValueError, 'no stable'): g.release('stable')
            self.status['withdrawn'] = [{'tag': 'garbage'}]
            with self.assertRaisesRegex(ValueError, 'revocation'): g.release('0.1.0')

    def test_invalid_selector_and_platform(self):
        for value in ('../main', '--stable', 'v01.2.3', 'v1.2.3\n', 'main'):
            with self.assertRaises(ValueError): g.selector(value)
        with patch.object(g.platform, 'system', return_value='Windows'):
            with self.assertRaisesRegex(ValueError, 'supported hosts'): g.host()


if __name__ == '__main__':
    unittest.main()
