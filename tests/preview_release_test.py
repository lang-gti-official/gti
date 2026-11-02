#!/usr/bin/env python3
"""Publish admitted snapshots in source order; reject mixed/incomplete artifacts."""
import base64
import copy
import io
import json
import os
import subprocess
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
import preview_build as build_preview

SHA = 'c' * 40
MANIFEST = {'schema': 1, 'features': [{'task': 'feature-a', 'head': 'd' * 40}]}


class PreviewTests(unittest.TestCase):
    def api(self, current=SHA, documents=None):
        documents = documents or {}
        history = ['a' * 40, 'b' * 40, SHA, 'd' * 40, 'e' * 40]
        def request(path):
            if '/contents/' in path:
                document = documents.get(path.split('?ref=')[1], MANIFEST)
                return {'content': base64.b64encode(json.dumps(document).encode()).decode()}
            if '/compare/' in path:
                base, head = path.split('/compare/')[1].split('...')
                if base not in history or head not in history:
                    return {'status': 'diverged'}
                return {'status': 'ahead' if history.index(base) < history.index(head) else 'behind'}
            self.assertEqual(path, p.BASE + '/git/ref/heads/preview')
            return {'object': {'sha': current}}
        return request

    def request(self, path):
        return self.api()(path)

    def test_preview_build_and_parallel_admission_contract(self):
        workflow = (ROOT / '.github/workflows/preview.yml').read_text()
        self.assertIn('touch build/.cmake/api/v1/query/codemodel-v2', workflow)
        self.assertIn('python3 ../control/scripts/preview_build.py --source . --build build', workflow)
        admission = workflow.split('      - name: Validate combined preview on this platform')[1].split('      - name: Stage toolchain')[0]
        self.assertIn('export CTEST_PARALLEL_LEVEL="${GTI_TEST_JOBS}"', admission)
        self.assertIn("-R '^preview_admission$' --no-tests=error", admission)
        self.assertIn('path: source/build/Testing', admission)
        self.assertNotIn('ctest --test-dir build --parallel', admission)
        self.assertIn('cancel-in-progress: false', workflow)

    def test_parallel_budget_reaches_nested_ctest(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve(); inner = root / 'inner'; inner.mkdir()
            worker = root / 'worker.py'
            worker.write_text("import pathlib,sys,time\n"
                              "root=pathlib.Path(sys.argv[1]); name=sys.argv[2]\n"
                              "(root/name).touch()\n"
                              "deadline=time.monotonic()+10\n"
                              "while not all((root/n).exists() for n in ('a','b')):\n"
                              "    assert time.monotonic()<deadline, 'nested tests ran serially'\n"
                              "    time.sleep(.01)\n")
            (inner / 'CTestTestfile.cmake').write_text(''.join(
                f'add_test({name} "{sys.executable}" "{worker}" "{root}" "{name}")\n'
                for name in ('a', 'b')))
            (root / 'CTestTestfile.cmake').write_text(
                f'add_test(preview_admission "ctest" "--test-dir" "{inner}" "--output-on-failure")\n'
                'set_tests_properties(preview_admission PROPERTIES RUN_SERIAL TRUE)\n')
            subprocess.run(['ctest', '--test-dir', str(root), '-R', '^preview_admission$',
                            '--output-on-failure', '--no-tests=error'],
                           env={**os.environ, 'CTEST_PARALLEL_LEVEL': '2'},
                           check=True, capture_output=True, timeout=30)

    def test_preview_target_selection_builds_real_installed_and_test_closure(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve(); source = root / 'source'; source.mkdir()
            build = root / 'build'; query = build / '.cmake/api/v1/query'; query.mkdir(parents=True)
            (query / 'codemodel-v2').touch()
            (source / 'main.c').write_text('int main(void) { return 0; }\n')
            (source / 'lib.c').write_text('int value(void) { return 1; }\n')
            (source / 'CMakeLists.txt').write_text(r'''cmake_minimum_required(VERSION 3.20)
project(preview C)
enable_testing()
foreach(name IN ITEMS gti gti_lsp gti_tree_sitter owner_tests unrelated_tests)
  add_executable(${name} main.c)
endforeach()
add_library(sdk STATIC lib.c)
add_library(file_archive STATIC lib.c)
add_library(dependency STATIC lib.c)
target_link_libraries(gti PRIVATE dependency)
install(TARGETS gti gti_lsp gti_tree_sitter sdk
        RUNTIME DESTINATION bin COMPONENT gti_toolchain
        ARCHIVE DESTINATION lib COMPONENT gti_toolchain)
install(FILES "$<TARGET_FILE:file_archive>" DESTINATION lib COMPONENT gti_toolchain)
add_test(NAME focused COMMAND "$<TARGET_FILE:owner_tests>"
         "suite=$<TARGET_FILE:sdk>" "XDG_CACHE_HOME=${CMAKE_BINARY_DIR}/cache")
''')
            subprocess.run(['cmake', '-S', str(source), '-B', str(build), '-G', 'Ninja',
                            '-DCMAKE_BUILD_TYPE=Release'], check=True, capture_output=True)
            targets = build_preview.build_targets(source, build, {'focused'})
            self.assertEqual(set(targets), {'gti', 'gti_lsp', 'gti_tree_sitter', 'sdk', 'file_archive', 'owner_tests'})
            subprocess.run(['cmake', '--build', str(build), '--parallel', '2', '--target', *targets],
                           check=True, capture_output=True)
            subprocess.run(['ctest', '--test-dir', str(build), '-R', '^focused$', '--no-tests=error'],
                           check=True, capture_output=True)
            subprocess.run(['cmake', '--install', str(build), '--prefix', str(root / 'stage'),
                            '--component', 'gti_toolchain'], check=True, capture_output=True)
            self.assertTrue((build / 'libdependency.a').is_file())
            self.assertTrue((root / 'stage/lib/libfile_archive.a').is_file())
            self.assertFalse((build / 'unrelated_tests').exists())
            with self.assertRaisesRegex(ValueError, 'missing configured'):
                build_preview.build_targets(source, build, {'missing'})
            with self.assertRaisesRegex(ValueError, 'optimized'):
                build_preview.build_targets(root / 'wrong-source', build, {'focused'})
            cache = build / 'CMakeCache.txt'
            cache.write_text(cache.read_text().replace('CMAKE_BUILD_TYPE:STRING=Release', 'CMAKE_BUILD_TYPE:STRING=Debug'))
            with self.assertRaisesRegex(ValueError, 'optimized'):
                build_preview.build_targets(source, build, {'focused'})

    def test_select_and_admit(self):
        self.assertEqual(p.select(self.request, 'push', 'refs/heads/preview'), SHA)
        self.assertEqual(p.select(self.request, 'workflow_dispatch', 'refs/heads/main'), SHA)
        for event, ref in [('pull_request', 'refs/heads/preview'), ('workflow_dispatch', 'refs/heads/feature'), ('push', 'refs/heads/main')]:
            with self.assertRaises(ValueError): p.select(self.request, event, ref)
        self.assertTrue(p.admit(self.request, 'a' * 40, '', MANIFEST))
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

    def publication(self, request, published='a' * 40, push_error=False):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp) / 'dist'; directory.mkdir(); self.make_packages(directory)
            calls = []
            def command(*args):
                calls.append(args)
                if args[:2] == ('git', 'push') and push_error:
                    raise subprocess.CalledProcessError(1, args, stderr='stale lease')
                return published + '\trefs/tags/preview' if args[1] == 'ls-remote' and published else ''
            with patch.object(p, 'command', side_effect=command), patch.object(p.subprocess, 'run') as run:
                run.return_value.returncode = 0
                if push_error:
                    with self.assertRaises(subprocess.CalledProcessError):
                        p.publish(directory, SHA, request, 123, 2)
                    self.assertFalse(any(c[0] == 'gh' for c in calls))
                    return None, calls
                result = p.publish(directory, SHA, request, 123, 2)
            if result:
                push = next(call for call in calls if call[:2] == ('git', 'push'))
                self.assertIn('--force-with-lease=refs/tags/preview:' + published, push)
                self.assertIn(SHA + ':refs/tags/preview', push)
                self.assertEqual(calls[-1][-1], str(directory / 'BUILD_ID'))
            return result, calls

    def test_publish_successful_build_after_branch_advances(self):
        result, _ = self.publication(self.api(current='e' * 40))
        self.assertTrue(result)

    def test_first_publication_and_same_commit_retry(self):
        # Same-source retries must finish partial uploads after the tag moved.
        for published in ('', SHA):
            with self.subTest(published=published):
                self.assertTrue(self.publication(self.api(current='e' * 40), published)[0])

    def test_older_build_cannot_replace_newer_release(self):
        result, calls = self.publication(self.api(current='e' * 40), published='d' * 40)
        self.assertFalse(result)
        self.assertEqual([call[:2] for call in calls], [('git', 'ls-remote')])

    def test_branch_advancement_between_checks_is_allowed(self):
        reads = 0
        def request(path):
            nonlocal reads
            if '/git/ref/' in path:
                reads += 1
            return self.api(current=SHA if reads == 1 else 'e' * 40)(path)
        self.assertTrue(self.publication(request)[0])
        self.assertEqual(reads, 2)

    def test_concurrent_tag_advance_stops_uploads(self):
        self.publication(self.api(current='e' * 40), push_error=True)

    def test_divergent_and_unadmitted_candidates_rejected(self):
        with self.assertRaisesRegex(ValueError, 'admitted preview history'):
            p.admit(self.api(current='f' * 40), SHA, '', MANIFEST)
        with self.assertRaisesRegex(ValueError, 'admitted preview history'):
            p.admit(self.api(current='a' * 40), SHA, '', MANIFEST)
        with self.assertRaisesRegex(ValueError, 'diverges'):
            p.admit(self.request, SHA, 'f' * 40, MANIFEST)
        with self.assertRaisesRegex(ValueError, 'unknown'):
            p.relation(lambda _: {'status': 'unknown'}, 'a' * 40, SHA)

    def test_withdrawal_and_source_manifest_identity(self):
        latest = {'schema': 1, 'features': [], 'removed': MANIFEST['features']}
        request = self.api(current='e' * 40, documents={'e' * 40: latest})
        with self.assertRaisesRegex(ValueError, 'withdrawn'):
            p.admit(request, SHA, '', MANIFEST)
        latest['removed'] = [{**MANIFEST['features'][0], 'restored': True}]
        self.assertTrue(p.admit(request, SHA, '', MANIFEST))
        with self.assertRaisesRegex(ValueError, 'inclusion differs'):
            p.admit(self.request, SHA, '', {'schema': 1, 'features': []})
        for document in ({'schema': 1, 'features': ['invalid']},
                         {'schema': 1, 'features': [], 'removed': 'invalid'}):
            with self.assertRaisesRegex(ValueError, 'manifest'):
                p.manifest(document)

    def test_withdrawal_between_checks_stops_publication(self):
        reads = 0
        latest = {'schema': 1, 'features': [], 'removed': MANIFEST['features']}
        def request(path):
            nonlocal reads
            if '/git/ref/' in path:
                reads += 1
            return self.api(current=SHA if reads == 1 else 'e' * 40,
                            documents={'e' * 40: latest})(path)
        with patch.object(p, 'command', return_value='') as command:
            with tempfile.TemporaryDirectory() as tmp:
                directory = Path(tmp); self.make_packages(directory)
                with self.assertRaisesRegex(ValueError, 'withdrawn'):
                    p.publish(directory, SHA, request, 123, 2)
            self.assertFalse(any(c.args[:2] == ('git', 'push') for c in command.call_args_list))

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
        self.assertIn('group: gti-preview-toolchain', workflow)
        self.assertIn('Publish validated preview without replacing a newer release', workflow)
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
