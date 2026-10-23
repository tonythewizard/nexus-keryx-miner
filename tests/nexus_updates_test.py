#!/usr/bin/env python3
import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest import mock
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import nexus_updates as updates
import nexus_dashboard as dashboard

class UpdateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name); self.state = self.root / 'state/updates'
        self.state.mkdir(parents=True)
        for name, value in [('ROOT', self.root), ('STATE', self.state)]:
            patch = mock.patch.object(updates, name, value); patch.start(); self.addCleanup(patch.stop)
        self.commit = 'a'*40
        self.manifest = dict(schema_version=1, git_commit=self.commit, version='0.0.2', platform='linux-x86_64', llama_abi=3,
                             stock_llama_sha256=updates.LLAMA_SHA, validation=dict(cuda_integration='PASS', rust_parity='PASS', escrow_claim_mock='PASS'),
                             worker=dict(asset='worker', sha256='b'*64), oracle=dict(asset='oracle', sha256='c'*64))
    def test_default_off_and_toggle_persistence(self):
        self.assertFalse(updates.configuration()['enabled'])
        updates.atomic(self.state/'config.json', {'enabled': False})
        self.assertFalse(updates.status()['enabled'])
    def test_disabled_never_calls_github_or_activation(self):
        updates.atomic(self.state/'config.json', {'enabled': False})
        with mock.patch.object(updates,'github') as github, mock.patch.object(updates,'apply') as apply:
            updates.check(); github.assert_not_called(); apply.assert_not_called()
        self.assertEqual(updates.status()['status'],'DISABLED')
    def test_invalid_configuration_fail_closed(self):
        updates.atomic(self.state/'config.json', {'enabled': 'false'})
        with self.assertRaises(ValueError): updates.configuration()
    def test_personal_private_repository_required(self):
        good=dict(owner=dict(login='tonythewizard',type='User'),private=True,visibility='private')
        with mock.patch.object(updates,'github',side_effect=[dict(login='tonythewizard'),good]): self.assertEqual(updates.repository_info(),good)
        for changes in (dict(private=False),dict(visibility='internal'),dict(owner=dict(login='org',type='Organization'))):
            with mock.patch.object(updates,'github',side_effect=[dict(login='tonythewizard'),{**good,**changes}]):
                with self.assertRaises(ValueError): updates.repository_info()
    def test_exact_manifest_and_tests_required(self):
        updates.validate_manifest(self.manifest,self.commit)
        for key,value in [('git_commit','d'*40),('llama_abi',4),('platform','linux-arm64'),('stock_llama_sha256','e'*64),('validation',{})]:
            with self.assertRaises(ValueError): updates.validate_manifest({**self.manifest,key:value},self.commit)
    def test_tauri_desktop_asset_digest_validated(self):
        manifest={**self.manifest,'desktop':dict(asset='nexus-desktop',sha256='d'*64)}
        updates.validate_manifest(manifest,self.commit)
        for value in ({'asset':'../desktop','sha256':'d'*64},{'asset':'desktop','sha256':'bad'}):
            with self.assertRaises(ValueError):updates.validate_manifest({**manifest,'desktop':value},self.commit)

    def test_legacy_release_cannot_stop_worker_for_tauri_activation(self):
        with mock.patch.object(updates,'verified_release',return_value=self.manifest), mock.patch.object(updates,'run') as run:
            with self.assertRaisesRegex(ValueError,'Tauri desktop'):updates.apply(self.root/'release')
            run.assert_not_called()

    def test_tauri_staged_binary_is_rehashed(self):
        directory=self.root/'updates/releases'/self.commit;directory.mkdir(parents=True)
        for role in ('worker','oracle','desktop'):
            (directory/role).write_bytes(role.encode())
            self.manifest[role]=dict(asset=role,sha256=updates.sha(directory/role))
        updates.atomic(directory/'manifest.json',self.manifest)
        updates.atomic(directory/'verified.json',dict(manifest_sha256=updates.sha(directory/'manifest.json')))
        updates.verified_release(directory)
        (directory/'desktop').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError,'desktop changed'):updates.verified_release(directory)

    def archive(self, name, kind=tarfile.REGTYPE):
        path=self.root/'source.tar'; info=tarfile.TarInfo(name);info.type=kind
        data=b'x'; info.size=1 if kind==tarfile.REGTYPE else 0
        with tarfile.open(path,'w') as f:f.addfile(info,io.BytesIO(data) if info.size else None)
        return path
    def test_archive_path_secrets_and_links_rejected(self):
        for name,kind in [('repo/../escape',tarfile.REGTYPE),('/escape',tarfile.REGTYPE),('repo/link',tarfile.SYMTYPE),('repo/model.gguf',tarfile.REGTYPE),('repo/escrow.key',tarfile.REGTYPE)]:
            with self.assertRaises(ValueError): updates.extract_source(self.archive(name,kind),self.root/'out')
    def test_archive_valid_source(self):
        updates.extract_source(self.archive('repo/VERSION'),self.root/'out')
        self.assertEqual((self.root/'out/VERSION').read_text(),'x')
    def test_staged_hashes_rechecked(self):
        directory=self.root/'updates/releases'/self.commit;directory.mkdir(parents=True)
        for role in ('worker','oracle'):
            (directory/role).write_bytes(role.encode());self.manifest[role]['sha256']=updates.sha(directory/role)
        updates.atomic(directory/'manifest.json',self.manifest)
        updates.atomic(directory/'verified.json',dict(manifest_sha256=updates.sha(directory/'manifest.json')))
        updates.verified_release(directory)
        (directory/'worker').write_bytes(b'tampered')
        with self.assertRaises(ValueError):updates.verified_release(directory)
    def test_dashboard_disabled_without_status(self):
        updates.atomic(self.state/'config.json',{'enabled':False})
        model=dashboard.DashboardModel()
        with mock.patch.object(dashboard.subprocess,'run',return_value=mock.Mock(stdout='inactive')):
            dashboard.refresh_development_status(model,self.root)
        self.assertFalse(model.updates['enabled'])
    def test_dashboard_update_controls_are_in_setup(self):
        m=dashboard.DashboardModel();m.updates=dict(status='UP_TO_DATE',enabled=True)
        frame='\n'.join(dashboard.build_matrix_frame(42,140,m,'','READ ONLY',7).lines())
        self.assertIn('Automatic updates',frame);self.assertIn('UP TO DATE',frame)
    def test_nominal_progress_shown_without_claim_readiness_promise(self):
        m=dashboard.DashboardModel();m.escrow_available=dict(status="AVAILABLE",nominal_outputs=54,outputs=54,amount_sompi=1)
        frame="\n".join(dashboard.build_matrix_frame(42,140,m,"","READ ONLY",3).lines())
        self.assertIn("54 / 87",frame);self.assertIn("Inputs missing",frame);self.assertIn("33",frame)
    def test_checker_runs_inside_runtime_without_immediate_activation(self):
        loop=updates.EmbeddedUpdater()
        def checked(activate):
            self.assertFalse(activate);loop.stop_event.set()
        with mock.patch.object(updates,'check',side_effect=checked) as check, mock.patch.object(updates,'status',return_value={'status':'LOCAL_AHEAD'}):
            loop.loop();check.assert_called_once_with(activate=False)
    def test_closed_checker_does_not_run(self):
        loop=updates.EmbeddedUpdater();loop.close()
        with mock.patch.object(updates,'check') as check:loop.loop();check.assert_not_called()
    def test_no_recovery_without_transaction(self):
        with mock.patch.object(updates,'run') as run:updates.recover();run.assert_not_called()
    def test_recovery_restores_unit_and_shortcuts(self):
        import nexus_agent_api
        home=self.root/'home';(home/'.local/bin').mkdir(parents=True)
        units={updates.SERVICE:'[Service]\nExecStart=original\n'}
        updates.atomic(self.state/'activation.json',dict(units=units,links=dict(nexus='/original/nexus'),installed=None))
        unit=home/'.config/systemd/user'/ (updates.SERVICE+'.d')/'release.conf';unit.parent.mkdir(parents=True);unit.write_text('candidate')
        with mock.patch.object(Path,'home',return_value=home), mock.patch.object(nexus_agent_api,'worker_identity',return_value=[]), mock.patch.object(updates,'run') as run:
            updates.recover()
        self.assertEqual(unit.read_text(),units[updates.SERVICE]);self.assertEqual((home/'.local/bin/nexus').readlink(),Path('/original/nexus'))
        self.assertFalse((self.state/'activation.json').exists())
        self.assertIn(mock.call(['systemctl','--user','start',updates.SERVICE]),run.call_args_list)
    def test_recovery_refuses_duplicate_worker(self):
        import nexus_agent_api
        updates.atomic(self.state/'activation.json',dict(units={},links={},installed=None))
        with mock.patch.object(nexus_agent_api,'worker_identity',return_value=[{'pid':1}]), mock.patch.object(updates,'run') as run:
            with self.assertRaises(ValueError):updates.recover()
        self.assertTrue((self.state/'activation.json').exists())
        self.assertEqual(run.call_count,1)
    def test_manual_shutdown_prevents_rollback_restart(self):
        import nexus_agent_api
        updates.atomic(self.state/'user-stop.json',dict(requested_unix=1))
        updates.atomic(self.state/'activation.json',dict(units={},links={},installed=None))
        with mock.patch.object(nexus_agent_api,'worker_identity',return_value=[]), mock.patch.object(updates,'run') as run:
            updates.recover()
        self.assertNotIn(mock.call(['systemctl','--user','start',updates.SERVICE]),run.call_args_list)
        with mock.patch.object(updates,'github') as github:updates.check();github.assert_not_called()
        self.assertEqual(updates.status()['status'],'STOPPED')
    def test_notification_and_wait_for_validated_release(self):
        def github(path):
            if '/commits/' in path:return dict(sha=self.commit,commit=dict(committer=dict(date='2099-01-01T00:00:00Z')))
            if '/releases?' in path:return []
            raise AssertionError(path)
        with mock.patch.object(updates,'repository_info',return_value={'default_branch':'main'}), mock.patch.object(updates,'github',side_effect=github), mock.patch.object(updates,'SOURCE',self.root/'no-git'), mock.patch.object(updates.subprocess,'run',return_value=mock.Mock(returncode=0)) as notify, mock.patch('nexus_notifications.notifications_enabled',return_value=True), mock.patch.object(updates,'apply') as apply:
            updates.check();updates.check()
        notify.assert_called_once();apply.assert_not_called()
        self.assertEqual(updates.status()['status'],'WAITING')

if __name__=='__main__':unittest.main()
