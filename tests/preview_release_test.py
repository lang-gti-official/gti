#!/usr/bin/env python3
"""Reject stale, mixed and incomplete preview publication without network/builds."""
import base64
import copy
import io
import json
import os
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import preview_release as p
import release_artifacts as a

SHA = 'c' * 40
MANIFEST = {'schema': 1, 'features': [{'task': 'feature-a', 'head': 'd' * 40}]}


class PreviewTests(unittest.TestCase):
    def request(self, path):
        if '/contents/' in path:
            return {'content': base64.b64encode(json.dumps(MANIFEST).encode()).decode()}
        return {'object': {'sha': SHA}}

    def test_select_and_admit(self):
        self.assertEqual(p.select(self.request, 'push', 'refs/heads/preview'), SHA)
        self.assertEqual(p.select(self.request, 'workflow_dispatch', 'refs/heads/main'), SHA)
        for event, ref in [('pull_request', 'refs/heads/preview'), ('workflow_dispatch', 'refs/heads/feature'), ('push', 'refs/heads/main')]:
            with self.assertRaises(ValueError): p.select(self.request, event, ref)
        with self.assertRaisesRegex(ValueError, 'advanced'):
            p.admit(self.request, 'a' * 40)
        with self.assertRaisesRegex(ValueError, 'manifest'):
            p.manifest({'schema': 1})

    def test_stage_refuses_changed_source(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'source'; source.mkdir()
            destination = Path(tmp) / 'stage'; (destination / 'share/gti').mkdir(parents=True)
            (source / '.gti-preview.json').write_text(json.dumps(MANIFEST))
            with patch.object(p, 'command', side_effect=[SHA, ' M VERSION']):
                with self.assertRaisesRegex(ValueError, 'changed during validation'):
                    p.stage(source, destination, SHA)
            self.assertFalse((destination / 'share/gti/preview.json').exists())
            with patch.object(p, 'command', side_effect=[SHA, '']):
                p.stage(source, destination, SHA)
            self.assertEqual((destination / 'share/gti/PREVIEW_SOURCE').read_text().strip(), SHA)

    def make_packages(self, directory, platform_identity=SHA, inclusion=MANIFEST):
        env = {'GITHUB_SHA': SHA, 'GITHUB_REPOSITORY': p.REPOSITORY,
               'GITHUB_RUN_ID': '123', 'GITHUB_RUN_ATTEMPT': '2'}
        for platform in a.PLATFORMS:
            path = directory / f'gti-preview-{platform}.tar.gz'
            with tarfile.open(path, 'w:gz') as archive:
                for name, value in [('BUILD_ID', SHA), ('PREVIEW_SOURCE', platform_identity), ('preview.json', json.dumps(inclusion))]:
                    data = value.encode(); info = tarfile.TarInfo('./share/gti/' + name); info.size = len(data)
                    archive.addfile(info, io.BytesIO(data))
            (directory / (path.name + '.sha256')).write_text(a.digest(path) + '  ' + path.name + '\n')
            a.record(directory, platform, env, 'preview')

    def test_package_identity(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp); self.make_packages(directory)
            self.assertEqual(p.check_packages(directory, SHA, 123, 2), MANIFEST)
            for sha, run, attempt in [('a' * 40, 123, 2), (SHA, 124, 2), (SHA, 123, 1)]:
                with self.assertRaisesRegex(ValueError, 'provenance'):
                    p.check_packages(directory, sha, run, attempt)
            with self.assertRaises(ValueError): a.verify(directory, SHA, 123, 2, p.REPOSITORY)
            archive = directory / 'gti-preview-linux-x64.tar.gz'
            archive.write_bytes(b'corrupt')
            with self.assertRaisesRegex(ValueError, 'checksum'): p.check_packages(directory, SHA, 123, 2)
            self.make_packages(directory, platform_identity='e' * 40)
            with self.assertRaisesRegex(ValueError, 'wrong source'): p.check_packages(directory, SHA, 123, 2)
            self.make_packages(directory)
            (directory / 'unexpected').touch()
            with self.assertRaisesRegex(ValueError, 'artifact set'): p.check_packages(directory, SHA, 123, 2)

    def test_publication_rechecks_head_and_uses_lease(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp) / 'dist'; directory.mkdir(); self.make_packages(directory)
            calls = []
            def command(*args):
                calls.append(args)
                return 'f' * 40 + '\trefs/tags/preview' if args[1] == 'ls-remote' else ''
            with patch.object(p, 'command', side_effect=command), patch.object(p.subprocess, 'run') as run:
                run.return_value.returncode = 0
                p.publish(directory, SHA, self.request, 123, 2)
            push = next(call for call in calls if call[:2] == ('git', 'push'))
            self.assertIn('--force-with-lease=refs/tags/preview:' + 'f' * 40, push)
            self.assertIn(SHA + ':refs/tags/preview', push)
            self.assertEqual(calls[-1][-1], str(directory / 'BUILD_ID'))
            for path in (directory / 'BUILD_ID', directory / 'preview.json'): path.unlink()
            with patch.object(p, 'command', side_effect=AssertionError('must not publish')):
                with self.assertRaisesRegex(ValueError, 'advanced'):
                    p.publish(directory, SHA, lambda _: {'object': {'sha': 'a' * 40}}, 123, 2)

    def test_workflow_admission(self):
        workflow = (ROOT / '.github/workflows/preview.yml').read_text()
        self.assertIn('branches: [preview]', workflow)
        self.assertIn('bash scripts/configure_ci_toolchain.sh', workflow)
        self.assertNotIn('gtiup_test.py', workflow)
        self.assertIn("if: needs.select.outputs.available == 'true'", workflow)
        self.assertEqual(workflow.count("if: steps.publisher.outputs.available == 'true'"), 2)
        # Simulate initial rollout: missing publisher reports unavailable and
        # prevents package work; a main checkout carrying it enables admission.
        import subprocess
        script = workflow.split('id: publisher\n        run: |\n', 1)[1].split('      - name:', 1)[0]
        import textwrap
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            output = directory / 'output'
            for present in (False, True):
                if present:
                    (directory / 'scripts').mkdir()
                    (directory / 'scripts/preview_release.py').touch()
                output.write_text('')
                subprocess.run(['bash', '-e', '-c', textwrap.dedent(script)], cwd=directory,
                               env={**os.environ, 'GITHUB_OUTPUT': str(output)}, check=True,
                               capture_output=True)
                self.assertEqual(output.read_text(), f"available={str(present).lower()}\n")
        self.assertIn("hashFiles('source/CMakeLists.txt', 'source/cmake/**')", workflow)
        self.assertIn('cancel-in-progress: false', workflow)
        self.assertIn('needs: [select, package]', workflow)
        self.assertIn("-R '^preview_admission$' --no-tests=error", workflow)
        self.assertIn('-DGTI_RELEASE_BUILD=ON', workflow)
        self.assertIn('--channel preview --sha "${GTI_SOURCE_SHA}"', workflow)
        self.assertIn('ref: ${{ needs.select.outputs.control }}', workflow)
        self.assertIn('gti-preview-*-${{ github.run_attempt }}', workflow)
        self.assertEqual(workflow.count('contents: write'), 1)
        self.assertIn('preview_release.py check', workflow)
        self.assertIn('attest-build-provenance@', workflow)
        self.assertIn('tests/cli_smoke_test.py', workflow)
        self.assertIn('tests/lsp_smoke_test.py', workflow)
        self.assertIn('installed_library_smoke', workflow)
        # Nightly remains isolated; preview success cannot qualify its evidence.
        nightly = (ROOT / '.github/workflows/nightly.yml').read_text()
        self.assertNotIn('preview_release', nightly)


if __name__ == '__main__':
    unittest.main()
