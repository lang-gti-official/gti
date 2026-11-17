"""Mounted-storage selection and pinned runner/worktree identities."""
import copy
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import agent_storage as storage
import agent_workflow as workflow
import remote_runner as runner


class StorageTests(unittest.TestCase):
    def test_mounted_external_and_internal_fallback(self):
        target = Path('/Volumes/External/Documents/External-projects')
        with mock.patch.object(storage.platform, 'system', return_value='Darwin'), \
             mock.patch.object(storage.platform, 'node', return_value='Lukes-Mac-mini.local'), \
             mock.patch.object(Path, 'resolve', return_value=target), \
             mock.patch.object(Path, 'is_mount', return_value=True), \
             mock.patch.object(Path, 'is_dir', return_value=True), \
             mock.patch.object(Path, 'stat', side_effect=[mock.Mock(st_dev=2), mock.Mock(st_dev=1)]), \
             mock.patch.object(storage.os, 'access', return_value=True):
            self.assertEqual(storage.external_projects(), target)
        for failure in (FileNotFoundError, PermissionError, RuntimeError):
            with mock.patch.object(storage.platform, 'system', return_value='Darwin'), \
                 mock.patch.object(storage.platform, 'node', return_value='Mac-mini'), \
                 mock.patch.object(Path, 'resolve', side_effect=failure):
                self.assertIsNone(storage.external_projects())
        with mock.patch.object(storage, 'external_projects', return_value=None):
            self.assertEqual(storage.worktree_path('task', Path('/repo/gti')), Path('/repo/gti-worktrees/task'))
            self.assertEqual(storage.runner_root('/private/tmp/gti-remote-runner'), '/private/tmp/gti-remote-runner')
        with mock.patch.object(storage, 'external_projects', return_value=target):
            self.assertEqual(storage.worktree_path('task', Path('/repo/gti')), target/'gti/worktrees/task')

    def test_non_external_unmounted_unwritable_and_other_hosts(self):
        for target, mounted, writable in [(Path('/Users/x/Documents/External-projects'), True, True),
                                          (Path('/Volumes/External/projects'), False, True),
                                          (Path('/Volumes/External/projects'), True, False)]:
            with mock.patch.object(storage.platform, 'system', return_value='Darwin'), \
                 mock.patch.object(storage.platform, 'node', return_value='Mac-mini'), \
                 mock.patch.object(Path, 'resolve', return_value=target), \
                 mock.patch.object(Path, 'is_mount', return_value=mounted), \
                 mock.patch.object(Path, 'is_dir', return_value=True), \
                 mock.patch.object(Path, 'stat', side_effect=[mock.Mock(st_dev=2), mock.Mock(st_dev=1)]), \
                 mock.patch.object(storage.os, 'access', return_value=writable):
                self.assertIsNone(storage.external_projects())
        with mock.patch.object(storage.platform, 'system', return_value='Linux'):
            self.assertIsNone(storage.external_projects())
        with mock.patch.object(storage.platform, 'node', return_value='MacBook.local'):
            self.assertIsNone(storage.external_projects())

    def test_disconnected_pinned_path_does_not_create_fallback(self):
        with mock.patch.object(Path, 'is_mount', return_value=False), mock.patch.object(Path, 'mkdir') as mkdir:
            with self.assertRaises(storage.StorageError):
                runner.root_directory('/Volumes/External/gti/gti-remote-runner', create=True)
            mkdir.assert_not_called()

    def test_remote_selection_keeps_original_window_and_does_not_mutate_config(self):
        config = copy.deepcopy(runner.HOSTS['mini'])
        with mock.patch.object(runner, 'connection', return_value={'root':'/Volumes/External/gti/gti-remote-runner'}):
            chosen = runner.new_run_connection(config)
        self.assertEqual(chosen['window_root'], config['root'])
        self.assertNotEqual(chosen['root'], config['root'])
        self.assertNotIn('window_root', config)
        with mock.patch.object(runner, 'connection') as connect:
            runner.new_run_connection(dict(config, prefer_external_storage=False))
            connect.assert_not_called()

    def test_recorded_run_stays_pinned_even_after_preference_changes(self):
        with tempfile.TemporaryDirectory() as temporary:
            local = Path(temporary)
            config = dict(runner.HOSTS['mini'], root='/Volumes/External/gti/gti-remote-runner',
                          window_root='/private/tmp/gti-remote-runner')
            run_id = 'run-' + 'a'*32
            runner.record_connection(local, run_id, 'mini', config)
            args = runner.parser().parse_args(['status', run_id])
            name, chosen = runner.select_connection(args, {'default':'ubuntu-8','hosts':runner.HOSTS}, local, run_id)
            self.assertEqual(name, 'mini')
            self.assertEqual(chosen, config)

    def test_optional_worktree_and_explicit_root(self):
        with mock.patch.object(sys, 'argv', ['agent_workflow.py','start','task','--title','Task']), \
             mock.patch.object(workflow, 'Workflow') as cls, contextlib.redirect_stdout(io.StringIO()):
            cls.return_value.lock.return_value = contextlib.nullcontext()
            cls.return_value.start.return_value = {}
            self.assertEqual(workflow.main(), 0)
            self.assertIsNone(cls.return_value.start.call_args.args[2])
        args = runner.parser().parse_args(['--root','/private/tmp/gti-remote-runner','status'])
        _, config = runner.select_connection(args, {'default':'mini','hosts':runner.HOSTS}, Path('/unused'))
        self.assertFalse(config['prefer_external_storage'])

    def test_saved_mini_default_upgrades_but_custom_root_is_preserved(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'settings.json'
            for root, preferred in [(runner.DEFAULT_ROOT, True), ('/custom/gti-remote-runner', False)]:
                config = dict(runner.HOSTS['mini'], root=root)
                del config['prefer_external_storage']
                path.write_text(json.dumps({'schema':runner.SETTINGS_SCHEMA, 'default':'mini', 'hosts':{'mini':config}}))
                loaded = runner.load_settings(path)['hosts']['mini']
                self.assertEqual(loaded.get('prefer_external_storage', False), preferred)

    def test_status_includes_both_roots_with_shared_window(self):
        config = dict(runner.HOSTS['mini'])
        def connection(c, action):
            if action == 'select-root': return {'root':'/Volumes/External/gti/gti-remote-runner'}
            return {'runs':[{'root':c['root']}], 'pruned':[], 'window':{'build':[]}}
        with mock.patch.object(runner,'connection',side_effect=connection):
            status = runner.runner_status(config)
        self.assertEqual(len(status['runs']),2)
        self.assertEqual(len(status['roots']),2)

if __name__ == '__main__':
    unittest.main()
