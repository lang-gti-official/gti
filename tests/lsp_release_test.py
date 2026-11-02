#!/usr/bin/env python3
"""Independent LSP admission, compatibility and immutable publication contracts."""
import copy
import io
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import lsp_ci
import lsp_compatibility as compatibility
import lsp_component as component
import release_artifacts as artifacts
import validation_checks as validation
from source_stdlib_ci_test import api_fixture

REPO = 'lang-gti-official/gti'
SHA = 'a' * 40
COMPAT = 'c' * 64
PLAN = dict(sha=SHA, run_id=7, attempt=2, compatibility=COMPAT)


def commit(root, path, value):
    file = root / path
    file.parent.mkdir(parents=True, exist_ok=True)
    file.write_text(value)
    subprocess.run(['git', '-C', str(root), 'add', '.'], check=True)
    subprocess.run(['git', '-C', str(root), '-c', 'user.name=Test', '-c', 'user.email=test@example.org',
                    'commit', '-qm', path], check=True)
    return subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip()


def packages(directory, compat=COMPAT, sha=SHA):
    directory.mkdir()
    for platform in artifacts.PLATFORMS:
        archive = directory / f'gti-nightly-{platform}.tar.gz'
        with tarfile.open(archive, 'w:gz') as stream:
            for name, data in {'bin/gti_lsp': b'validated adapter', 'bin/gti': b'compiler not in component',
                               'share/gti/BUILD_ID': sha.encode(), 'share/licenses/gti/LICENSE': b'license',
                               'share/gti/LSP_COMPATIBILITY.json': json.dumps(dict(schema=1, compatibility=compat)).encode()}.items():
                item = tarfile.TarInfo(name); item.size = len(data); stream.addfile(item, io.BytesIO(data))
        (directory / (archive.name + '.sha256')).write_text(artifacts.digest(archive) + '  ' + archive.name + '\n')
        artifacts.record(directory, platform, dict(GITHUB_SHA=SHA, GITHUB_REPOSITORY=REPO,
                                                  GITHUB_RUN_ID='7', GITHUB_RUN_ATTEMPT='2'))


class IndependentLSP(unittest.TestCase):
    def test_routing_rejects_shared_and_unknown_inputs(self):
        paths = ['src/lsp/main.cpp', 'tests/lsp_smoke_test.py', 'docs/architecture/lsp.md']
        self.assertEqual(validation.ci_mode({}, paths), 'lsp')
        for path in ['src/compiler/frontend.cpp', 'include/gti/frontend.h', 'stdlib/std/string.gti',
                     'runtime/src/thread.cpp', 'tree-sitter-gti/src/parser.c', 'CMakeLists.txt',
                     'tests/CMakeLists.txt', 'tests/ownership.toml', 'scripts/lsp_ci.py',
                     '.github/workflows/ci.yml', 'unknown.txt', 'tests/native.cpp']:
            with self.subTest(path=path):
                self.assertEqual(validation.ci_mode({}, paths + [path]), 'full')
        self.assertFalse(lsp_ci.eligible(['tests/lsp_smoke_test.py']))

    def test_shared_identity_and_full_sdk_admission(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            subprocess.run(['git', 'init', '-q', str(root)], check=True)
            base = commit(root, 'src/compiler/frontend.cpp', 'shared')
            identity = compatibility.identity(root)
            head = commit(root, 'src/lsp/main.cpp', 'adapter')
            for path in ['lua/gti/init.lua', 'docs/lsp.md', 'tests/lsp_smoke_test.py', 'changes/lsp.feature.md']:
                head = commit(root, path, 'adapter companion')
                self.assertEqual(compatibility.identity(root), identity)
            self.assertEqual(lsp_ci.baseline(REPO, head, api_fixture(base), root)['sha'], base)
            for api in [api_fixture(base, source_lane=True), api_fixture(base, assets=[]),
                        api_fixture(base, run={'event': 'pull_request'})]:
                with self.assertRaises(ValueError):
                    lsp_ci.baseline(REPO, head, api, root)
            for path in ['stdlib/std/string.gti', 'src/compiler/frontend.cpp', 'runtime/a.h',
                         'CMakeLists.txt', 'scripts/a.py', 'VERSION', 'unknown.txt']:
                before = compatibility.identity(root)
                commit(root, path, 'different shared input')
                self.assertNotEqual(compatibility.identity(root), before)
            with self.assertRaises(ValueError):
                lsp_ci.baseline(REPO, subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip(), api_fixture(base), root)

    def test_lsp_evidence_cannot_seed_compiler_sdk(self):
        jobs = [dict(name=n, head_sha=SHA, status='completed', conclusion='success')
                for n in ['CI gate', 'LSP (linux-x64)', 'LSP (darwin-arm64)']]
        api = api_fixture(SHA, jobs=jobs)
        self.assertEqual(artifacts.release_evidence(REPO, SHA, api)['validation_mode'], 'lsp')
        with self.assertRaises(ValueError):
            artifacts.release_evidence(REPO, SHA, api, full_only=True)
        for missing in range(len(jobs)):
            with self.assertRaises(ValueError):
                artifacts.release_evidence(REPO, SHA, api_fixture(SHA, jobs=jobs[:missing]+jobs[missing+1:]))

    def test_package_embeds_exact_identity_without_compiler(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); packages(root / 'input')
            manifest = component.pack(root / 'input', root / 'output', PLAN)
            self.assertEqual(set(manifest['platforms']), set(artifacts.PLATFORMS))
            for platform, row in manifest['platforms'].items():
                self.assertEqual(row['sha256'], artifacts.digest(root / 'output' / row['asset']))
                with tarfile.open(root / 'output' / row['asset']) as archive:
                    self.assertNotIn('bin/gti', archive.getnames())
                    meta = json.loads(component.read_member(archive, 'share/gti/LSP_COMPONENT.json', 4096))
                    self.assertEqual(meta, dict(schema=1, kind='gti-lsp', compatibility=COMPAT,
                                               source=SHA, run_id=7, attempt=2, platform=platform))
                    self.assertTrue(archive.getmember('bin/gti_lsp').mode & 0o111)
            with self.assertRaisesRegex(ValueError, 'compiler origin'):
                component.pack(root / 'input', root / 'missing-origin', {**PLAN, 'validation_mode':'lsp'})
            for key, value in [('sha', 'b'*40), ('attempt', 3), ('run_id', 8), ('compatibility', 'd'*64)]:
                with self.subTest(key=key), self.assertRaises(ValueError):
                    component.pack(root / 'input', root / ('bad-' + key), {**PLAN, key: value})
            archive = root / 'input/gti-nightly-linux-x64.tar.gz'
            archive.write_bytes(archive.read_bytes() + b'altered')
            with self.assertRaisesRegex(ValueError, 'checksum'):
                component.pack(root / 'input', root / 'corrupt', PLAN)

    def test_embedded_source_and_resources_must_match(self):
        for key in ['sha', 'compat']:
            with self.subTest(key=key), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                packages(root / 'input', **{key: 'd'*(40 if key == 'sha' else 64)})
                with self.assertRaises(ValueError):
                    component.pack(root / 'input', root / 'out', PLAN)

    def test_older_or_divergent_components_never_replace_newer(self):
        old, new, main = '1'*40, '2'*40, '3'*40
        def ancestor(a, b, api=None):
            return a == b or (a, b) in {(old, new), (old, main), (new, main)}
        manifest = dict(source=new, compatibility=COMPAT, run_id=9)
        release = dict(tag_name='lsp-'+COMPAT+'-7', target_commitish=old, draft=False)
        with patch.object(component, 'ancestor', ancestor):
            self.assertTrue(component.publication_admitted(manifest, [release], main))
            self.assertFalse(component.publication_admitted({**manifest, 'source':old}, [release], main))
            self.assertFalse(component.publication_admitted({**manifest, 'source':old},
                [{**release, 'target_commitish':new}], main))
            with self.assertRaisesRegex(ValueError, 'backwards'):
                component.publication_admitted(manifest, [{**release, 'tag_name':'lsp-'+COMPAT+'-10'}], main)
            with self.assertRaisesRegex(ValueError, 'diverges'):
                component.publication_admitted(manifest, [{**release, 'target_commitish':'4'*40}], main)
            with self.assertRaisesRegex(ValueError, 'not reached main'):
                component.publication_admitted({**manifest, 'source':'4'*40}, [], main)

    def test_immutable_payload_covers_resources_and_permissions(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); (root / 'bin').mkdir(); (root / 'share/gti/stdlib').mkdir(parents=True)
            (root / 'bin/gti').write_text('compiler'); (root / 'bin/gti_lsp').write_text('old')
            (root / 'share/gti/stdlib/a.gti').write_text('library')
            before = lsp_ci.preserved_payload(root)
            (root / 'bin/gti_lsp').write_text('new')
            self.assertEqual(before, lsp_ci.preserved_payload(root))
            (root / 'share/gti/stdlib/a.gti').write_text('wrong')
            self.assertNotEqual(before, lsp_ci.preserved_payload(root))


if __name__ == '__main__':
    unittest.main()
