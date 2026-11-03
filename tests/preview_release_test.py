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

    def test_preview_build_smoke_and_package_contract(self):
        # The publisher repeats no focused checks: admission ran them on the
        # submitting host. It builds the products, proves the compiler runs
        # native programs with the candidate's own stdlib, and packages.
        workflow = (ROOT / '.github/workflows/preview.yml').read_text()
        self.assertIn('touch build/.cmake/api/v1/query/codemodel-v2', workflow)
        self.assertIn('python3 ../control/scripts/preview_build.py --source . --build build --parallel "${GTI_JOBS}" --products-only', workflow)
        package = workflow.split('      - name: Build the installed toolchain products')[1].split('      - name: Stage toolchain')[0]
        self.assertIn('validation_checks.py smoke --root . --build build', package)
        self.assertNotIn('preview_admission', workflow)
        self.assertNotIn('ctest --test-dir build', package)
        self.assertIn('path: source/build/Testing/preview-build.json', package)
        self.assertIn("gti-ccache-v2-release-${{ runner.os }}-${{ runner.arch }}-", workflow, 'restores the prebuilt release cache')
        self.assertNotIn('actions/cache/save@', workflow, 'llvm-prebuilt.yml owns the release cache')
        self.assertNotIn('check_ci_sanitizers', workflow)
        installed = workflow.split('      - name: Test installed toolchain')[1].split('      - name: Preserve the combined source inclusion record')[0]
        for runtime_check in ('managed_thread_runtime_test', 'atomic_runtime_test', 'process_resource_policy_test'):
            self.assertNotIn(runtime_check, installed, 'runtime behaviour is admission evidence, not packaging evidence')
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
            # Configured without a file-API query, as a local preview seed is:
            # target selection must request the model and reconfigure.
            build = root / 'build'
            (source / 'main.c').write_text('int main(void) { return 0; }\n')
            (source / 'lib.c').write_text('int value(void) { return 1; }\n')
            (source / 'CMakeLists.txt').write_text(r'''cmake_minimum_required(VERSION 3.20)
project(preview C)
enable_testing()
foreach(name IN ITEMS gti gti_lsp gti_tree_sitter owner_tests census_helper unrelated_tests)
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
         "suite=$<TARGET_FILE:sdk>" "--inventory=$<TARGET_FILE:census_helper>"
         "XDG_CACHE_HOME=${CMAKE_BINARY_DIR}/cache")
''')
            subprocess.run(['cmake', '-S', str(source), '-B', str(build), '-G', 'Ninja',
                            '-DCMAKE_BUILD_TYPE=Release'], check=True, capture_output=True)
            self.assertFalse((build / '.cmake/api/v1/reply').exists())
            targets = build_preview.build_targets(source, build, {'focused'})
            self.assertTrue((build / '.cmake/api/v1/query/codemodel-v2').is_file())
            self.assertEqual(set(targets), {'gti', 'gti_lsp', 'gti_tree_sitter', 'sdk', 'file_archive', 'owner_tests', 'census_helper'})
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
        self.assertIn("hashFiles('source/CMakeLists.txt', 'source/cmake/**', 'source/scripts/configure_ci_toolchain.sh')", workflow)
        self.assertIn('cancel-in-progress: false', workflow)
        self.assertIn('group: gti-preview-toolchain', workflow)
        self.assertIn('Publish validated preview without replacing a newer release', workflow)
        self.assertIn('needs: [select, package]', workflow)
        self.assertIn('validation_checks.py smoke --root . --build build', workflow)
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


class RemotePreviewTests(unittest.TestCase):
    make_packages = PreviewTests.make_packages
    api = PreviewTests.api
    def evidence(self, root, directory):
        import preview_remote as remote
        results = {}
        for i, platform in enumerate(a.PLATFORMS):
            path = root/platform; (path/'logs').mkdir(parents=True)
            run = 'run-'+str(i+1)*32
            source = {'head': SHA, 'status': '', 'snapshot_sha256': str(i+3)*64}
            steps = remote.plan(SHA, platform, '/exports/'+platform, 2, 2)
            request = {'id': run, 'source': source, 'build_type': 'Release',
                       'required_platform': 'Darwin' if platform=='darwin-arm64' else 'Linux', 'steps': steps}
            executed = [{'name': s['name'], 'exit_code': 0, 'argv': [v.replace('{source}', '/runs/'+run+'/source').replace('{build}', '/runs/'+run+'/build') for v in s['argv']]} for s in steps]
            report = {'id': run, 'source': source, 'final_source': {'head': SHA, 'status': ''},
                      'state': 'passed', 'exit_code': 0, 'steps': executed}
            for name, value in [('request.json',request),('report.json',report),('truncation.json',{'tail_only':[]})]:
                (path/name).write_text(json.dumps(value))
            names = [f'gti-preview-{platform}.tar.gz', f'gti-preview-{platform}.tar.gz.sha256']
            package = {'sha': SHA, 'platform': platform, 'profile': a.PROFILE,
                       'environment': ({'os':'ubuntu-24.04','image':None} if platform=='linux-x64' else
                                       {'os':'macos','deployment_target':'26.0','image':None}),
                       'files': {n:a.digest(directory/n) for n in names}}
            (path/'logs/package.log').write_text(remote.MARKER+json.dumps(package)+'\n')
            results[platform] = path
        return results

    def test_remote_receipts_are_preview_only_and_bound_to_real_packages(self):
        import preview_remote as remote
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); directory=root/'dist'; directory.mkdir(); self.make_packages(directory)
            results=self.evidence(root,directory)
            remote.verify(directory,SHA,results,record=True)
            self.assertEqual(p.check_packages(directory,SHA,None,None,remote_results=results),MANIFEST)
            for platform in a.PLATFORMS:
                document=json.loads((directory/f'provenance-{platform}.json').read_text())
                self.assertEqual(document['executor'],'remote-runner')
                self.assertTrue(document['run_id'].startswith('run-'))
                self.assertNotIn('attempt',document)
            with self.assertRaisesRegex(ValueError,'provenance'):
                p.check_packages(directory,SHA,123,2)
            with self.assertRaisesRegex(ValueError,'provenance'):
                a.verify(directory,SHA,123,2,p.REPOSITORY)
            with self.assertRaisesRegex(ValueError,'must not claim'):
                p.check_packages(directory,SHA,123,2,remote_results=results)
            with self.assertRaisesRegex(ValueError,'both reviewed'):
                remote.verify(directory,SHA,{'linux-x64':results['linux-x64']})
            (directory/'gti-preview-linux-x64.tar.gz').write_bytes(b'changed')
            with self.assertRaisesRegex(ValueError,'checksum'):
                remote.verify(directory,SHA,results)

    def test_production_environment_rejects_newer_linux_and_mutable_images(self):
        import preview_remote as remote
        with patch.object(remote.host,'freedesktop_os_release',return_value={'ID':'ubuntu','VERSION_ID':'26.04'}):
            with self.assertRaisesRegex(ValueError,'24.04'):remote.production_environment('linux-x64')
        with patch.object(remote.host,'freedesktop_os_release',return_value={'ID':'ubuntu','VERSION_ID':'24.04'}):
            self.assertEqual(remote.production_environment('linux-x64','sha256:'+'a'*64),
                             {'os':'ubuntu-24.04','image':'sha256:'+'a'*64})
        with patch.object(remote.host,'mac_ver',return_value=('27.0.1',(),'')):
            self.assertEqual(remote.production_environment('darwin-arm64')['deployment_target'],'26.0')
        with self.assertRaisesRegex(ValueError,'immutable'):
            remote.plan(SHA,'linux-x64','/exports',image='ubuntu:24.04')
        self.assertEqual(remote.plan(SHA,'linux-x64','/exports',image='sha256:'+'a'*64)[0]['argv'][-2:],
                         ['--container-image','sha256:'+'a'*64])

    def test_macos_deployment_target_is_scoped_to_compiler_build(self):
        import preview_remote as remote
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)/'build';build.mkdir()
            (build/'preview-editor.path').write_text('/pinned-editor/bin\n')
            def editor_stage(directory, path):path.write_text('/pinned-editor/bin\n')
            with patch.dict(os.environ, {'MACOSX_DEPLOYMENT_TARGET':'26.0'}), \
                    patch.object(remote.host,'system',return_value='Darwin'), \
                    patch.object(remote.host,'machine',return_value='arm64'), \
                    patch.object(remote.host,'mac_ver',return_value=('27.0.1',(),'')), \
                    patch.object(p,'command',side_effect=lambda *args:SHA if 'rev-parse' in args else ''), \
                    patch.object(remote,'execute') as execute, \
                    patch('configure_ci_editor.stage',side_effect=editor_stage):
                remote.step('admission',SHA,'darwin-arm64',build,Path(tmp)/'dist',4,3)
                remote.step('configure',SHA,'darwin-arm64',build,Path(tmp)/'dist',4,3)
            self.assertEqual(len(execute.call_args_list),3)
            for call in execute.call_args_list:
                self.assertNotIn('MACOSX_DEPLOYMENT_TARGET',call.kwargs['env'])
            self.assertIn('-DCMAKE_OSX_DEPLOYMENT_TARGET=26.0',execute.call_args.args[0])

    def test_installed_smokes_use_staged_absolute_executables(self):
        import preview_remote as remote
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)/'build'; build.mkdir()
            # A build-tree binary and the installed binary coexist. Bare `gti`
            # makes the CLI harness resolve the first but PATH execute the second.
            (build/'gti').write_text('build-tree binary')
            (build/'preview-editor.path').write_text('/pinned-editor/bin\n')
            stage = build/'preview-stage'
            (stage/'bin').mkdir(parents=True)
            for name in ('gti', 'gti_lsp'):
                (stage/'bin'/name).write_text('staged binary')
            with patch.object(remote.host,'system',return_value='Darwin'), \
                    patch.object(remote.host,'machine',return_value='arm64'), \
                    patch.object(remote.host,'mac_ver',return_value=('27.0.1',(),'')), \
                    patch.object(p,'command',side_effect=lambda *args:SHA if 'rev-parse' in args else ''), \
                    patch.object(remote,'execute') as execute:
                remote.step('installed',SHA,'darwin-arm64',build,Path(tmp)/'dist',4,3)
            smokes = [call for call in execute.call_args_list if call.args[0][0] == 'python3']
            self.assertEqual(len(smokes), 6)
            for call in smokes:
                name = 'gti_lsp' if Path(call.args[0][1]).name == 'lsp_smoke_test.py' else 'gti'
                self.assertEqual(Path(call.args[0][2]), (stage/'bin'/name).resolve())
                self.assertTrue(Path(call.args[0][2]).is_absolute())

    def test_remote_receipts_bind_production_os_and_container_identity(self):
        import preview_remote as remote
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);directory=root/'dist';directory.mkdir();self.make_packages(directory)
            results=self.evidence(root,directory);location=results['linux-x64'];log=location/'logs/package.log'
            original=json.loads(log.read_text()[len(remote.MARKER):]);changed=copy.deepcopy(original)
            changed['environment']['os']='ubuntu-26.04';log.write_text(remote.MARKER+json.dumps(changed)+'\n')
            with self.assertRaisesRegex(ValueError,'identity'):remote.receipt(location,SHA,'linux-x64')
            original['environment']['image']='sha256:'+'b'*64;log.write_text(remote.MARKER+json.dumps(original)+'\n')
            with self.assertRaisesRegex(ValueError,'identity'):remote.receipt(location,SHA,'linux-x64')

    def test_remote_receipts_reject_skipped_changed_and_misattributed_work(self):
        import preview_remote as remote
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); directory=root/'dist'; directory.mkdir(); self.make_packages(directory)
            results=self.evidence(root,directory); location=results['linux-x64']
            report=json.loads((location/'report.json').read_text());request=json.loads((location/'request.json').read_text())
            bad=[]
            for key,value in [('state','failed'),('exit_code',1),('steps',report['steps'][:-1])]:
                d=copy.deepcopy(report);d[key]=value;bad.append(d)
            d=copy.deepcopy(report);d['source']['head']='f'*40;bad.append(d)
            d=copy.deepcopy(report);d['final_source']['status']=' M source';bad.append(d)
            d=copy.deepcopy(report);d['steps'][2]['exit_code']=8;bad.append(d)
            d=copy.deepcopy(report);d['steps'][2]['argv'][-1]='99';bad.append(d)
            for document in bad:
                (location/'report.json').write_text(json.dumps(document))
                with self.assertRaises(ValueError):remote.receipt(location,SHA,'linux-x64')
            (location/'report.json').write_text(json.dumps(report))
            for mutate in [lambda d:d.update(required_platform='Darwin'),lambda d:d.update(build_type='Debug'),
                           lambda d:d['steps'][2].update(argv=['true'])]:
                document=copy.deepcopy(request);mutate(document)
                (location/'request.json').write_text(json.dumps(document))
                with self.assertRaises(ValueError):remote.receipt(location,SHA,'linux-x64')
            (location/'request.json').write_text(json.dumps(request))
            (location/'truncation.json').write_text(json.dumps({'tail_only':['logs/admission.log']}))
            with self.assertRaisesRegex(ValueError,'truncated'):remote.receipt(location,SHA,'linux-x64')

    def test_remote_publication_keeps_order_and_exact_tag_lease(self):
        import preview_remote as remote
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); directory=root/'dist'; directory.mkdir();self.make_packages(directory)
            results=self.evidence(root,directory);remote.verify(directory,SHA,results,record=True)
            with patch.object(p,'command',side_effect=lambda *args:'d'*40+'\trefs/tags/preview' if args[1]=='ls-remote' else '') as command:
                self.assertFalse(p.publish(directory,SHA,self.api(current='d'*40),None,None,remote_results=results))
                self.assertFalse(any(c.args[:2]==('git','push') for c in command.call_args_list))
            calls=[]
            def command(*args):
                calls.append(args)
                if args[1]=='ls-remote':return 'a'*40+'\trefs/tags/preview'
                return ''
            with patch.object(p,'command',side_effect=command),patch.object(p.subprocess,'run') as run:
                run.return_value.returncode=0
                self.assertTrue(p.publish(directory,SHA,self.api(),None,None,remote_results=results))
            push=next(c for c in calls if c[:2]==('git','push'))
            self.assertIn('--force-with-lease=refs/tags/preview:'+'a'*40,push)
            self.assertTrue(str(calls[-1][-1]).endswith('/BUILD_ID'))
            self.assertIn('No GitHub Actions build attestation',(root/'preview-notes.txt').read_text())

    def test_remote_admission_requires_current_candidate_and_exact_inclusion(self):
        import preview_remote as remote
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); directory=root/'dist'; directory.mkdir();self.make_packages(directory)
            results=self.evidence(root,directory);remote.verify(directory,SHA,results,record=True)
            with patch('agent_workflow.Workflow') as workflow, patch('preview_workflow.Preview') as preview:
                state={'head':SHA,'manifest':MANIFEST}
                preview.return_value.load.return_value=state
                remote.admit(directory,SHA,results,root,'candidate')
                preview.return_value.current.assert_called_once_with(state)
                self.assertEqual(state['verified']['head'],SHA)
                self.assertEqual(set(state['verified']['execution']['platforms']),set(a.PLATFORMS))
                preview.return_value.publish.assert_called_once_with('candidate')
                preview.return_value.reset_mock()
                for change in ({'head':'e'*40},{'manifest':{'schema':1,'features':[]}}):
                    preview.return_value.load.return_value={'head':SHA,'manifest':MANIFEST,**change}
                    with self.assertRaisesRegex(ValueError,'prepared candidate'):
                        remote.admit(directory,SHA,results,root,'candidate')
                preview.return_value.save.assert_not_called()
                preview.return_value.publish.assert_not_called()
                preview.return_value.current.side_effect=RuntimeError('preview advanced')
                with self.assertRaisesRegex(RuntimeError,'preview advanced'):
                    remote.admit(directory,SHA,results,root,'candidate')
                preview.return_value.publish.assert_not_called()


if __name__ == '__main__':
    unittest.main()
