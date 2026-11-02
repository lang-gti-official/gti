#!/usr/bin/env python3
"""Source-only CI admission, artifact identity, routing and extraction regressions."""
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
import source_stdlib_ci as source
import release_artifacts as artifacts
import validation_checks as validation

REPO = 'lang-gti-official/gti'
ORIGIN = dict(sha='a' * 40, native_inputs='b' * 64, run_id=7, attempt=2,
              repository=REPO, profile=artifacts.PROFILE)


def api_fixture(sha, source_lane=False, **changes):
    run = dict(id=7, run_attempt=2, event='merge_group', head_branch='gh-readonly-queue/main/pr-1',
               head_sha=sha, path='.github/workflows/ci.yml', status='completed', conclusion='success',
               repository={'full_name': REPO}, head_repository={'full_name': REPO})
    names = ['CI gate']
    names += ([f'Source stdlib ({p})' for p in artifacts.PLATFORMS] if source_lane else
              [f'Release package ({p})' for p in artifacts.PLATFORMS] +
              [f'Integration ({p} {i}/{n})' for p, n in [('linux', 6), ('darwin', 5)] for i in range(n)])
    jobs = [dict(name=n, head_sha=sha, status='completed', conclusion='success') for n in names]
    assets = [dict(name=f'gti-nightly-{p}-2', expired=False) for p in artifacts.PLATFORMS]
    run.update(changes.get('run', {}))
    jobs = changes.get('jobs', jobs)
    assets = changes.get('assets', assets)
    def request(path):
        if '/jobs?' in path:
            assert '/attempts/2/' in path
            return {'jobs': jobs}
        if '/artifacts?' in path:
            return {'artifacts': assets, 'total_count': len(assets)}
        return {'workflow_runs': [run]}
    return request


class SourceStdlibCI(unittest.TestCase):
    def test_only_library_source_and_reviewed_companions_qualify(self):
        paths = ['stdlib/std/algorithm.gti', 'tests/generic_algorithm_test.py',
                 'tests/fixtures/remove_if.gti', 'examples/100-remove-if.gti',
                 'changes/remove-if.feature.md', 'docs/language/algorithms.md']
        self.assertTrue(source.eligible(paths))
        self.assertEqual(validation.ci_mode({}, paths), 'source-stdlib')
        self.assertEqual(validation.focused_scope(paths), 'source-stdlib')
        for path in ['CMakeLists.txt', 'cmake/Any.cmake', 'tests/CMakeLists.txt', 'tests/ownership.toml',
                     'runtime/src/thread.cpp', 'src/compiler/parse.cpp', 'tests/test.cpp', 'VERSION',
                     'stdlib/prelude.gti', 'stdlib/module-map.json', 'scripts/source_stdlib_ci.py',
                     '.github/workflows/ci.yml', 'unknown.gti']:
            with self.subTest(path=path):
                self.assertEqual(validation.ci_mode({}, paths + [path]), 'full')
        self.assertFalse(source.eligible(['docs/index.md']))
        self.assertFalse(source.eligible(['tests/foo_test.py']))

    def test_draft_component_and_rollup_contracts_remain(self):
        paths = ['stdlib/std/algorithm.gti', '.gti-validation/batches/rollup-20261102-1.json']
        self.assertEqual(validation.ci_mode({}, paths), 'source-stdlib')
        self.assertEqual(validation.ci_mode({'pull_request': {'draft': True}}, paths), 'draft')
        self.assertEqual(validation.ci_mode({'pull_request': {'body': '<!-- gti-validation-component: {} -->'}}, paths), 'component')

    def test_source_gate_requires_successful_matrix_and_lint(self):
        rows = {key: {'result': 'success'} for key in validation.LINT_JOBS | {'source-stdlib'}}
        for event in ['pull_request', 'merge_group']:
            validation.admission('source-stdlib', rows, event)
            for state in ['skipped', 'failure', 'cancelled']:
                with self.assertRaises(ValueError):
                    validation.admission('source-stdlib', {**rows, 'source-stdlib': {'result': state}}, event)
        with self.assertRaises(ValueError):
            validation.admission('full', rows, 'merge_group')

    def test_owners_consumers_and_regressions_are_mandatory(self):
        names = source.test_entries(['stdlib/std/algorithm.gti', 'tests/generic_algorithm_cluster_test.py'])
        for name in ['compiler_pipeline', 'stdlib_surface', 'stdlib_surface_manifest',
                     'generic_algorithm_cluster', 'generic_algorithm_errors', 'practical_examples']:
            self.assertIn(name, names)
        with patch.object(source, 'git', return_value='examples/unregistered/new.gti\0'), self.assertRaisesRegex(ValueError, 'nested example coverage'):
            source.test_entries(['stdlib/std/algorithm.gti', 'examples/unregistered/new.gti'])
        with self.assertRaisesRegex(ValueError, 'not registered'):
            source.test_entries(['stdlib/std/algorithm.gti', 'tests/unregistered_new_test.py'])
        with self.assertRaisesRegex(ValueError, 'no registered consumer'):
            source.test_entries(['stdlib/std/algorithm.gti', 'tests/fixtures/unregistered_new.gti'])

    def test_test_inventory_missing_and_disabled_fail(self):
        inventory = {'tests': [{'name': 'compiler_pipeline'}, {'name': 'runtime_case'}]}
        self.assertEqual(source.selected_tests(['compiler_pipeline', '^runtime_.*$'], inventory, []),
                         {'compiler_pipeline', 'runtime_case'})
        with self.assertRaises(ValueError):
            source.selected_tests(['not_registered'], inventory, [])
        inventory['tests'][1]['properties'] = [{'name': 'DISABLED', 'value': True}]
        with self.assertRaises(ValueError):
            source.selected_tests(['runtime_case'], inventory, [])

    def test_results_require_execution_not_skips(self):
        with tempfile.TemporaryDirectory() as temporary:
            report = Path(temporary) / 'results.xml'
            for body in ['<testcase name="case"><skipped/></testcase>', '<testcase name="case"><failure/></testcase>',
                         '<testcase name="other"/>', '<testcase name="case"/><testcase name="case"/>']:
                report.write_text('<testsuite>' + body + '</testsuite>')
                with self.assertRaises(ValueError):
                    source.preview_examples.validate_results(report, {'case'})
            report.write_text('<testsuite><testcase name="case"/></testsuite>')
            source.preview_examples.validate_results(report, {'case'})

    def test_source_release_is_not_a_full_compiler_build(self):
        request = api_fixture(ORIGIN['sha'], source_lane=True)
        self.assertEqual(artifacts.release_evidence(REPO, ORIGIN['sha'], request)['run_id'], '7')
        with self.assertRaises(ValueError):
            artifacts.release_evidence(REPO, ORIGIN['sha'], request, full_only=True)
        for options in [dict(run={'event': 'pull_request'}), dict(run={'conclusion': 'failure'}),
                        dict(jobs=[]), dict(assets=[]), dict(run={'head_sha': 'c' * 40}),
                        dict(run={'head_repository': {'full_name': 'fork/gti'}})]:
            with self.subTest(options=options), self.assertRaises(ValueError):
                artifacts.release_evidence(REPO, ORIGIN['sha'], api_fixture(ORIGIN['sha'], True, **options))

    def artifact(self, directory, platform='darwin-arm64'):
        archive = directory / f'gti-nightly-{platform}.tar.gz'
        archive.write_bytes(b'validated native compiler archive')
        (directory / (archive.name + '.sha256')).write_text(artifacts.digest(archive) + '  ' + archive.name + '\n')
        artifacts.record(directory, platform, dict(GITHUB_SHA=ORIGIN['sha'], GITHUB_REPOSITORY=REPO,
                         GITHUB_RUN_ID='7', GITHUB_RUN_ATTEMPT='2'))
        return archive

    def test_artifact_requires_exact_source_attempt_platform_and_bytes(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            archive = self.artifact(directory)
            self.assertEqual(source.verify_platform(directory, ORIGIN, 'darwin-arm64'), archive)
            for field, value in [('sha', 'c' * 40), ('run_id', 8), ('attempt', 1),
                                 ('profile', 'debug'), ('repository', 'fork/gti')]:
                with self.subTest(field=field), self.assertRaises(ValueError):
                    source.verify_platform(directory, {**ORIGIN, field: value}, 'darwin-arm64')
            archive.write_bytes(b'tampered')
            with self.assertRaises(ValueError):
                source.verify_platform(directory, ORIGIN, 'darwin-arm64')

    def test_derived_archive_cannot_seed_native_reuse(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            self.artifact(directory)
            path = directory / 'provenance-darwin-arm64.json'
            data = json.loads(path.read_text()); data['compiler_origin'] = ORIGIN
            path.write_text(json.dumps(data))
            with self.assertRaisesRegex(ValueError, 'directly'):
                source.verify_platform(directory, ORIGIN, 'darwin-arm64')

    def test_reused_release_provenance_preserves_compiler_origin(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            for platform in artifacts.PLATFORMS:
                archive = self.artifact(directory, platform)
                with tarfile.open(archive, 'w:gz') as stream:
                    for name, data in [('share/gti/COMPILER_ORIGIN.json', json.dumps(ORIGIN).encode()),
                                       ('share/gti/BUILD_ID', b'c' * 40)]:
                        member = tarfile.TarInfo(name); member.size = len(data)
                        stream.addfile(member, io.BytesIO(data))
                artifacts.record(directory, platform, dict(GITHUB_SHA='c' * 40, GITHUB_REPOSITORY=REPO,
                                 GITHUB_RUN_ID='9', GITHUB_RUN_ATTEMPT='1'), compiler_origin=ORIGIN)
            artifacts.verify(directory, 'c' * 40, 9, 1, REPO)
            path = directory / 'provenance-darwin-arm64.json'
            data = json.loads(path.read_text()); data['compiler_origin']['sha'] = 'd' * 40
            path.write_text(json.dumps(data))
            with self.assertRaisesRegex(ValueError, 'embedded|disagree'):
                artifacts.verify(directory, 'c' * 40, 9, 1, REPO)

    def test_extraction_rejects_escape_and_external_links(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            for index, (name, link) in enumerate([('../escape', None), ('lib/link', '/tmp/escape')]):
                archive = directory / f'{index}.tar.gz'
                with tarfile.open(archive, 'w:gz') as stream:
                    item = tarfile.TarInfo(name)
                    if link:
                        item.type = tarfile.SYMTYPE; item.linkname = link
                        stream.addfile(item)
                    else:
                        item.size = 1; stream.addfile(item, io.BytesIO(b'x'))
                with self.assertRaises(tarfile.FilterError):
                    source.unpack(archive, directory / f'stage{index}')
            self.assertFalse((directory.parent / 'escape').exists())

    def test_native_identity_ignores_only_source_test_documentation_metadata(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            subprocess.run(['git', 'init', '-q', str(root)], check=True)
            def commit(path, text):
                p = root / path; p.parent.mkdir(parents=True, exist_ok=True); p.write_text(text)
                subprocess.run(['git', '-C', str(root), 'add', '.'], check=True)
                subprocess.run(['git', '-C', str(root), '-c', 'user.name=Test', '-c', 'user.email=test@example.org',
                                'commit', '-qm', path], check=True)
                return source.git('rev-parse', 'HEAD', root=root)
            first = commit('src/compiler.cpp', 'native')
            initial = source.native_identity(first, root)
            for path in ['stdlib/std/algorithm.gti', 'tests/fixture.gti', 'docs/test.md', 'changes/test.feature.md']:
                self.assertEqual(source.native_identity(commit(path, 'updated'), root), initial)
            for path in ['src/compiler.cpp', 'runtime/header.h', 'CMakeLists.txt', 'scripts/configure_ci_toolchain.sh',
                         '.github/workflows/ci.yml', 'unknown-native-input.txt']:
                with self.subTest(path=path):
                    self.assertNotEqual(source.native_identity(commit(path, 'changed'), root), initial)

    def test_baseline_requires_matching_native_ancestor_and_full_artifacts(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            subprocess.run(['git', 'init', '-q', str(root)], check=True)
            def commit(path, text):
                p = root / path; p.parent.mkdir(parents=True, exist_ok=True); p.write_text(text)
                subprocess.run(['git', '-C', str(root), 'add', '.'], check=True)
                subprocess.run(['git', '-C', str(root), '-c', 'user.name=Test', '-c', 'user.email=test@example.org',
                                'commit', '-qm', path], check=True)
                return source.git('rev-parse', 'HEAD', root=root)
            native = commit('src/compiler.cpp', 'unchanged optimized compiler')
            head = commit('stdlib/std/algorithm.gti', 'new library source')
            origin = source.baseline(REPO, head, api_fixture(native), root)
            self.assertEqual(origin['sha'], native)
            self.assertEqual(origin['native_inputs'], source.native_identity(head, root))
            for api in [api_fixture(native, source_lane=True), api_fixture(native, assets=[]),
                        api_fixture(native, run={'event': 'pull_request'})]:
                with self.assertRaises(ValueError):
                    source.baseline(REPO, head, api, root)
            changed = commit('src/compiler.cpp', 'different compiler')
            with self.assertRaises(ValueError):
                source.baseline(REPO, changed, api_fixture(native), root)
            subprocess.run(['git', '-C', str(root), 'checkout', '-q', '--detach', native], check=True)
            sibling = commit('stdlib/std/unrelated.gti', 'unrelated history')
            with self.assertRaises(ValueError):
                source.baseline(REPO, head, api_fixture(sibling), root)

    def test_planning_falls_back_when_artifacts_or_routing_unavailable(self):
        with patch.object(source, 'changed', return_value=['stdlib/std/algorithm.gti']), \
             patch.object(source, 'git', return_value='c' * 40), \
             patch.object(source, 'baseline', side_effect=ValueError('expired')), \
             patch.object(source, 'test_entries', return_value=['compiler_pipeline']):
            self.assertEqual(source.plan('a' * 40, {}, REPO)['mode'], 'full')

    def test_prepare_replaces_deleted_sources_and_preserves_native_bytes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            old = root / 'old'
            (old / 'share/gti/stdlib/std').mkdir(parents=True)
            (old / 'bin').mkdir()
            (old / 'bin/gti').write_bytes(b'unchanged native binary')
            (old / 'share/gti/BUILD_ID').write_text(ORIGIN['sha'])
            (old / 'share/gti/stdlib/std/deleted.gti').write_text('old')
            artifact = root / 'artifact'; artifact.mkdir()
            archive = artifact / 'gti-nightly-darwin-arm64.tar.gz'
            with tarfile.open(archive, 'w:gz') as stream:
                stream.add(old, arcname='.')
            (artifact / (archive.name + '.sha256')).write_text(artifacts.digest(archive) + '  ' + archive.name + '\n')
            artifacts.record(artifact, 'darwin-arm64', dict(GITHUB_SHA=ORIGIN['sha'], GITHUB_REPOSITORY=REPO,
                             GITHUB_RUN_ID='7', GITHUB_RUN_ATTEMPT='2'))
            (root / 'stdlib/std').mkdir(parents=True)
            (root / 'stdlib/std/new.gti').write_text('new source')
            plan = dict(head='c' * 40, base='d' * 40, compiler_origin=ORIGIN)
            with patch.object(source, 'ensure_checkout'), \
                 patch.object(source, 'git', return_value=plan['head']), \
                 patch.object(source, 'changed', return_value=['stdlib/std/new.gti']), \
                 patch.object(source, 'native_identity', return_value=ORIGIN['native_inputs']), \
                 patch.object(source, 'request', api_fixture(ORIGIN['sha'])):
                source.prepare(plan, 'darwin-arm64', artifact, root / 'stage', root)
            stage = root / 'stage'
            with patch.object(source, 'ensure_checkout'), patch.object(source, 'git', return_value=plan['head']):
                source.seal(plan, stage, 'darwin-arm64', root)
                (stage / 'bin/gti').write_bytes(b'changed native binary')
                with self.assertRaisesRegex(ValueError, 'native payload changed'):
                    source.seal(plan, stage, 'darwin-arm64', root)
                (stage / 'bin/gti').write_bytes(b'unchanged native binary')
                (stage / 'share/gti/stdlib/std/new.gti').write_text('stale library')
                with self.assertRaisesRegex(ValueError, 'packaged library differs'):
                    source.seal(plan, stage, 'darwin-arm64', root)
                (stage / 'share/gti/stdlib/std/new.gti').write_text('new source')
            self.assertEqual((stage / 'bin/gti').read_bytes(), b'unchanged native binary')
            self.assertFalse((stage / 'share/gti/stdlib/std/deleted.gti').exists())
            self.assertEqual((stage / 'share/gti/stdlib/std/new.gti').read_text(), 'new source')
            self.assertEqual(json.loads((stage / 'share/gti/COMPILER_ORIGIN.json').read_text()), ORIGIN)

    def test_consumer_graph_refuses_native_sources_and_ignores_output_directories(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            replies = build / '.cmake/api/v1/reply'; replies.mkdir(parents=True)
            (replies / 'index-fixture.json').write_text(json.dumps({'reply': {'codemodel-v2': {'jsonFile': 'model.json'}}}))
            (replies / 'model.json').write_text(json.dumps({'paths': {'source': str(ROOT)}, 'configurations': [
                {'targets': [{'name': 'consumer', 'id': 'consumer-id', 'jsonFile': 'consumer.json'}]}]}))
            document = {'name': 'consumer', 'sources': [{'path': 'tests/consumer.cpp'}],
                        'artifacts': [{'path': 'tests/consumer'}]}
            target = replies / 'consumer.json'; target.write_text(json.dumps(document))
            (build / 'CTestTestfile.cmake').write_text('add_test("case" "' + str(build / 'tests/consumer') +
                '" "--output-dir=' + str(build / 'some-output-directory') + '")\n')
            self.assertEqual(source.consumer_targets(build, '^case$'), ['consumer'])
            document['sources'] = [{'path': 'src/compiler/native.cpp'}]
            target.write_text(json.dumps(document))
            with self.assertRaisesRegex(ValueError, 'rebuild native'):
                source.consumer_targets(build, '^case$')

    def test_workflow_source_lane_and_full_lane_are_separate(self):
        text = (ROOT / '.github/workflows/ci.yml').read_text()
        lane = text.split('\n  source-stdlib:\n')[1].split('\n  package:\n')[0]
        for required in ['source_stdlib_ci.py prepare', 'source_stdlib_ci.py test', 'compiler_origin.run_id',
                         'compiler_origin.attempt', '--compiler-origin', 'github.event_name == \'merge_group\'']:
            self.assertIn(required, lane)
        self.assertNotIn('cmake --install', lane)
        self.assertNotIn('continue-on-error', lane)
        self.assertIn('GTI_COMPILER_TEST_ISOLATE_CASES ON', (ROOT / 'cmake/ReuseToolchain.cmake').read_text())
        self.assertIn("needs.plan.outputs.mode == 'full'", text.split('\n  package:\n')[1])


if __name__ == '__main__':
    unittest.main()
