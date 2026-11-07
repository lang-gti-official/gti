#!/usr/bin/env python3
"""Independent LSP admission, compatibility and immutable publication contracts."""
import copy
import io
import json
import os
from pathlib import Path
import re
import shlex
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
    directory.mkdir(parents=True, exist_ok=True)
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
                     'tests/lsp_sdk_test.py', 'tests/lsp_release_test.py',
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

    def test_lsp_gate_requires_the_adapter_job_and_all_lint(self):
        results = {name:{'result':'success'} for name in validation.LINT_JOBS | {'lsp'}}
        validation.admission('lsp', results, 'merge_group')
        for name in results:
            broken = copy.deepcopy(results); broken[name]['result'] = 'skipped'
            with self.subTest(name=name), self.assertRaises(ValueError):
                validation.admission('lsp', broken, 'merge_group')

    def test_lsp_evidence_cannot_seed_compiler_sdk(self):
        jobs = [dict(name=n, head_sha=SHA, status='completed', conclusion='success')
                for n in ['CI gate', 'LSP (linux-x64)', 'LSP (darwin-arm64)']]
        api = api_fixture(SHA, source_lane=True, jobs=jobs)
        self.assertEqual(artifacts.release_evidence(REPO, SHA, api)['validation_mode'], 'lsp')
        with self.assertRaises(ValueError):
            artifacts.release_evidence(REPO, SHA, api, full_only=True)
        with self.assertRaises(ValueError):
            # The lane runs in the merge queue; a full-validation run never carries it.
            artifacts.release_evidence(REPO, SHA, api_fixture(SHA, jobs=jobs))
        for missing in range(len(jobs)):
            with self.assertRaises(ValueError):
                artifacts.release_evidence(REPO, SHA, api_fixture(SHA, source_lane=True, jobs=jobs[:missing]+jobs[missing+1:]))

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

    def test_workflow_packaging_isolated_from_checked_out_packages(self):
        workflow = (ROOT / '.github/workflows/lsp.yml').read_text()

        def step(name):
            return workflow.split('      - name: ' + name + '\n', 1)[1].split('\n      - ', 1)[0]

        def run_line(name):
            return re.search(r'^\s+run: (.+)$', step(name), re.MULTILINE).group(1)

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            checkout = root / 'checkout'; checkout.mkdir()
            runner = root / 'runner temp'; runner.mkdir()
            (checkout / 'scripts').symlink_to(ROOT / 'scripts', target_is_directory=True)
            # Actions downloads merge into an existing destination. This is a
            # source directory in the real checkout, not an empty artifact store.
            source = checkout / 'packages/README.md'
            source.parent.mkdir()
            source.write_text('GTI package sources must be preserved.\n')

            def expand(value):
                return value.replace('${{ runner.temp }}', str(runner)).replace('${RUNNER_TEMP}', str(runner))

            def argument(command, flag):
                argv = shlex.split(expand(command))
                return checkout / argv[argv.index(flag) + 1]

            download = re.search(r'^\s+path: (.+)$', step('Download exact admitted packages'), re.MULTILINE).group(1)
            inputs = checkout / expand(download)
            packages(inputs)
            selection = run_line('Select successful integration already on main')
            argument(selection, '--plan').write_text(json.dumps(PLAN))
            command = run_line('Verify and package compatible LSPs without rebuilding')
            result = subprocess.run(['bash', '-e', '-c', command], cwd=checkout,
                                    env=dict(os.environ, RUNNER_TEMP=str(runner), GITHUB_REPOSITORY=REPO),
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            output = argument(command, '--output')
            self.assertEqual(json.loads((output / 'lsp.json').read_text())['source'], SHA)
            self.assertEqual(argument(run_line('Publish immutable compatible updates'), '--directory'), output)
            subject = re.search(r'^\s+subject-path: (.+)$', step('Attest component archives'), re.MULTILINE).group(1)
            self.assertEqual((checkout / expand(subject)).parent, output)
            self.assertEqual(source.read_text(), 'GTI package sources must be preserved.\n')
            self.assertEqual(list(source.parent.iterdir()), [source])

    def test_package_rejects_unexpected_input_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); packages(root / 'input')
            (root / 'input/README.md').write_text('not an admitted artifact')
            with self.assertRaisesRegex(ValueError, 'unexpected release artifact set'):
                component.pack(root / 'input', root / 'output', PLAN)

    def test_already_published_source_skips_package_downloads(self):
        def api(path):
            if '/git/ref/' in path: return {'object':{'sha':SHA}}
            if '/workflows/' in path: return {'workflow_runs':[{'head_sha':SHA,'event':'merge_group'}]}
            if '/releases?' in path:
                return [dict(tag_name='lsp-'+COMPAT+'-7', draft=False, target_commitish=SHA)]
            raise AssertionError(path)
        with patch.object(artifacts, 'release_evidence', return_value={'run_id':'7','run_attempt':'2'}), \
             patch.object(compatibility, 'identity', return_value=COMPAT), patch.object(component.source, 'git'):
            self.assertIsNone(component.select(api))
        workflow = (ROOT / '.github/workflows/lsp.yml').read_text()
        self.assertIn("workflow_run.event == 'merge_group'", workflow)
        self.assertIn("workflow_run.event == 'push'", workflow)
        self.assertIn('workflows: [CI, Full validation]', workflow)
        self.assertIn('ref: main', workflow)
        self.assertIn('cancel-in-progress: false', workflow)
        self.assertNotIn('cmake --build', workflow)

    def test_draft_publication_resumes_without_overwriting_assets(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); packages(root / 'input')
            component.pack(root / 'input', root / 'out', PLAN)
            component.pack(root / 'input', root / 'repeat', PLAN)
            for file in (root / 'out').iterdir():
                self.assertEqual(file.read_bytes(), (root / 'repeat' / file.name).read_bytes())
            releases = []; commands = []; fail = [True]
            def api(path):
                if '/releases?' in path: return releases
                if '/git/ref/' in path: return {'object':{'sha':SHA}}
                if '/releases/tags/' in path: return releases[0]
                raise AssertionError(path)
            def command(args, check):
                commands.append(args)
                if args[2] == 'create':
                    releases.append(dict(tag_name=args[3], target_commitish=SHA, draft=True, assets=[]))
                elif args[2] == 'upload':
                    if fail[0] and releases[0]['assets']:
                        fail[0] = False
                        raise subprocess.CalledProcessError(1, args)
                    file = Path(args[4])
                    releases[0]['assets'].append(dict(name=file.name, digest='sha256:'+artifacts.digest(file)))
                elif args[2] == 'edit': releases[0]['draft'] = False
                else: raise AssertionError(args)
            with patch.object(component.subprocess, 'run', command), patch.object(artifacts, 'release_evidence',
                    return_value={'run_id':'7','run_attempt':'2'}):
                with self.assertRaises(subprocess.CalledProcessError):
                    component.publish(root / 'out', PLAN, api)
                self.assertTrue(releases[0]['draft'])
                self.assertTrue(component.publish(root / 'out', PLAN, api))
                self.assertFalse(releases[0]['draft'])
                self.assertEqual(sum(c[2] == 'create' for c in commands), 1)
                self.assertEqual(len(releases[0]['assets']), 5)
                self.assertFalse(component.publish(root / 'out', PLAN, api))

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
