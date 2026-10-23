#!/usr/bin/env python3
"""Force restart controller fixtures; all process/service actions are mocked."""
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import nexus_restart_workers as restart


class RestartTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.state = self.root/'state'; self.state.mkdir()
        self.updates = self.root/'updates'; self.updates.mkdir()
        self.home = self.root/'home'
        self.unit = 'nexus-fixture-worker.service'
        self.fragment = self.root/'systemd/transient'/self.unit
        self.fragment.parent.mkdir(parents=True)
        self.contents = '[Service]\nExecStart=/fixture/nexus live --enable-ai-response --enable-submit\nRestart=no\n'
        self.fragment.write_text(self.contents)
        process = self.root/'proc/12'; process.mkdir(parents=True)
        (process/'cgroup').write_text('0::/user.slice/'+self.unit+'\n')
        self.worker = dict(pid=12, kind='nexus-live-cuda-worker', starttime='123')
        self.new = dict(pid=13, kind='nexus-live-cuda-worker', starttime='124')
        self.calls = []

    def tearDown(self): self.tmp.cleanup()

    def path(self, *parts):
        if parts and str(parts[0]) == '/proc': return self.root/'proc'/Path(*parts[1:])
        return Path(*parts)

    def service_run(self, args, timeout=10, env=None):
        self.calls.append(args)
        if 'FragmentPath' in ' '.join(args): return str(self.fragment)
        if '--force' in args:
            restart.atomic(self.updates/'user-stop.json', {'request_id': env.get('NEXUS_STOP_REQUEST_ID') if env else None})
        return ''

    def patches(self, identities):
        fake_path=mock.Mock(side_effect=self.path); fake_path.home.return_value=self.home
        return (mock.patch.object(restart,'STATE',self.state),
                mock.patch.object(restart,'UPDATE_STATE',self.updates),
                mock.patch.object(restart,'Path',fake_path),
                mock.patch.object(restart,'native_workers',side_effect=identities),
                mock.patch.object(restart,'digest',return_value='a'*64),
                mock.patch.object(restart,'properties',return_value={'LoadState':'loaded','ActiveState':'inactive'}),
                mock.patch.object(restart,'run',side_effect=self.service_run),
                mock.patch.object(restart.subprocess,'run',return_value=mock.Mock(returncode=1)))

    def test_transient_restart_preserves_exact_unit_and_worker(self):
        patches=self.patches([[self.worker],[self.worker],[],[],[self.new]])
        for patch in patches:patch.start()
        try:restart.restart()
        finally:
            for patch in reversed(patches):patch.stop()
        self.assertEqual((self.home/'.config/systemd/user'/self.unit).read_text(),self.contents)
        self.assertEqual(json.loads((self.state/'status.json').read_text())['status'],'START_REQUESTED')
        self.assertFalse((self.updates/'user-stop.json').exists())
        self.assertIn(['systemctl','--user','start',self.unit],self.calls)

    def test_multiple_workers_refuse_any_stop(self):
        with mock.patch.object(restart,'native_workers',return_value=[self.worker,self.new]), mock.patch.object(restart,'run') as run:
            with self.assertRaises(ValueError):restart.restart()
        run.assert_not_called()

    def test_newer_manual_stop_blocks_restart(self):
        original=self.service_run
        def run(args, timeout=10, env=None):
            result=original(args,timeout,env)
            if '--force' in args:restart.atomic(self.updates/'user-stop.json',{'request_id':'newer-user-stop'})
            return result
        patches=self.patches([[self.worker],[self.worker],[]])
        for patch in patches:patch.start()
        try:
            with mock.patch.object(restart,'run',side_effect=run),self.assertRaises(ValueError):restart.restart()
        finally:
            for patch in reversed(patches):patch.stop()
        self.assertNotIn(['systemctl','--user','start',self.unit],self.calls)

    def test_stopped_identity_resolves_only_the_declared_recovery_variable(self):
        import hashlib
        source=self.root/'source';scripts=source/'scripts';scripts.mkdir(parents=True)
        binary=self.root/'artifacts/worker';binary.parent.mkdir();binary.write_bytes(b'fixture identity')
        expected=hashlib.sha256(binary.read_bytes()).hexdigest()
        launcher=scripts/'nexusmine-next.fish'
        launcher.write_text('set -l recovery '+str(self.root)+'\nset -l worker "$recovery/artifacts/worker"\nset -l expected_sha256 '+expected+'\n')
        command='{ path=/usr/bin/fish ; argv[]=/usr/bin/fish '+str(launcher)+' live ; ignore_errors=no ; }'
        def run(args,**kwargs):
            return command if 'ExecStart' in args else ''
        with mock.patch.object(restart,'ROOT',self.root),mock.patch.object(restart,'SOURCE',source), \
             mock.patch.object(restart,'properties',return_value={'LoadState':'loaded','ActiveState':'failed'}), \
             mock.patch.object(restart,'run',side_effect=run),mock.patch.object(restart,'native_workers',return_value=[]):
            self.assertEqual(restart.stopped_service_identity(self.unit),expected)
            binary.write_bytes(b'changed')
            with self.assertRaisesRegex(ValueError,'hash mismatch'):
                restart.stopped_service_identity(self.unit)

    def test_failed_worker_recovers_without_stopping_the_desktop(self):
        self.fragment=self.root/self.unit;self.fragment.write_text(self.contents)
        patches=self.patches([[],[],[],[],[self.new]])
        for patch in patches:patch.start()
        try:
            with mock.patch.object(restart,'stopped_service_identity',return_value='a'*64) as checked:
                restart.restart()
                checked.assert_called_once_with('nexus-v002-production.service')
        finally:
            for patch in reversed(patches):patch.stop()
        self.assertFalse(any('--force' in args for args in self.calls))
        self.assertIn(['systemctl','--user','start','nexus-v002-production.service'],self.calls)

    def test_manual_shutdown_during_stopped_recovery_wins(self):
        self.fragment=self.root/self.unit;self.fragment.write_text(self.contents)
        patches=self.patches([[],[],[]])
        for patch in patches:patch.start()
        def validate(unit):
            restart.atomic(self.updates/'user-stop.json',{'request_id':'newer-manual-stop'})
            return 'a'*64
        try:
            with mock.patch.object(restart,'stopped_service_identity',side_effect=validate),self.assertRaisesRegex(ValueError,'newer manual shutdown'):
                restart.restart()
        finally:
            for patch in reversed(patches):patch.stop()
        self.assertFalse(any('start' in args for args in self.calls))

    def test_optional_helper_failure_does_not_prevent_worker_restart(self):
        patches=self.patches([[self.worker],[self.worker],[],[],[self.new]])
        for patch in patches:patch.start()
        try:
            with mock.patch.object(restart.subprocess,'run',return_value=mock.Mock(returncode=0)), \
                 mock.patch.object(restart,'reset_if_failed',side_effect=lambda unit: None if unit==self.unit else (_ for _ in ()).throw(ValueError('missing helper'))):
                restart.restart()
        finally:
            for patch in reversed(patches):patch.stop()
        result=json.loads((self.state/'status.json').read_text())
        self.assertEqual(result['status'],'START_REQUESTED')
        self.assertEqual(result['helper_errors'],list(restart.HELPERS))

    def test_failed_worker_with_bad_pin_never_starts(self):
        self.fragment=self.root/self.unit;self.fragment.write_text(self.contents)
        patches=self.patches([[]])
        for patch in patches:patch.start()
        try:
            with mock.patch.object(restart,'stopped_service_identity',side_effect=ValueError('hash mismatch')),self.assertRaises(ValueError):
                restart.restart()
        finally:
            for patch in reversed(patches):patch.stop()
        self.assertFalse(any('start' in args or '--force' in args for args in self.calls))

    def test_worker_appearing_before_start_is_not_duplicated(self):
        patches=self.patches([[self.worker],[self.worker],[],[self.new]])
        for patch in patches:patch.start()
        try:
            with self.assertRaisesRegex(ValueError,'appeared before service start'):
                restart.restart()
        finally:
            for patch in reversed(patches):patch.stop()
        self.assertNotIn(['systemctl','--user','start',self.unit],self.calls)

    def test_unloaded_optional_unit_is_not_reset(self):
        with mock.patch.object(restart,'properties',return_value={'LoadState':'not-found'}),mock.patch.object(restart,'run') as run:
            with self.assertRaises(ValueError):restart.reset_if_failed('optional.service')
        run.assert_not_called()

    def test_healthy_unit_needs_no_reset_failed(self):
        with mock.patch.object(restart,'properties',return_value={'LoadState':'loaded','ActiveState':'inactive'}),mock.patch.object(restart,'run') as run:
            restart.reset_if_failed(self.unit)
        run.assert_not_called()


if __name__=='__main__':unittest.main(verbosity=2)
