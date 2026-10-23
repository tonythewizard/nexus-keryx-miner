#!/usr/bin/env python3
"""Offline regression coverage for shared sessions, resilient feeds and controllers."""
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import nexus_dashboard as dashboard
from nexus_desktop_commands import controller_command


class RefinementTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name); (self.root/'logs').mkdir()

    def telemetry(self, name, claimed=False):
        path=self.root/'logs'/('nexus-ui-inference-'+name+'.jsonl')
        rows=[{'event':'STARTUP_STATE','state':'BOOT','result':'PASS'}]
        if claimed: rows.append({'event':'ESCROW_CLAIM_SUBMISSION','state':'RPC_ACKNOWLEDGED','txid':'a'*64,'inputs':87,'payout_sompi':123})
        path.write_text(''.join(json.dumps(row)+'\n' for row in rows));return path

    @mock.patch.object(dashboard,'load_wallet_status')
    def test_session_rollover_resets_counts_and_fixed_reader_stays_fixed(self, wallet):
        first=self.telemetry('one',True)
        reader=dashboard.TelemetrySessionReader(self.root); self.addCleanup(reader.close)
        fixed=dashboard.TelemetrySessionReader(self.root,first,False); self.addCleanup(fixed.close)
        self.assertEqual(reader.poll().claimed['outputs'],87)
        self.assertEqual(fixed.poll().claimed['outputs'],87)
        second=self.telemetry('two');os.utime(second,ns=(first.stat().st_mtime_ns+10,first.stat().st_mtime_ns+10))
        reader.next_scan=fixed.next_scan=0
        self.assertEqual(reader.poll().claimed['outputs'],0)
        self.assertEqual(fixed.poll().claimed['outputs'],87)

    @mock.patch.object(dashboard,'load_wallet_status')
    def test_failed_open_is_retryable_without_discarding_current_model(self, wallet):
        self.telemetry('one',True);reader=dashboard.TelemetrySessionReader(self.root); self.addCleanup(reader.close)
        before=reader.model
        with mock.patch.object(dashboard,'TelemetryFollower',side_effect=OSError('fixture')):
            with self.assertRaises(OSError):reader.poll()
        self.assertIs(reader.model,before)
        self.assertEqual(reader.poll().claimed['outputs'],87)

    @mock.patch.object(dashboard,'load_wallet_status')
    def test_failed_rollover_preserves_previous_session_then_recovers(self, wallet):
        first=self.telemetry('one',True)
        reader=dashboard.TelemetrySessionReader(self.root);self.addCleanup(reader.close)
        old=reader.poll();old_follower=reader.follower
        second=self.telemetry('two')
        os.utime(second,ns=(first.stat().st_mtime_ns+10,first.stat().st_mtime_ns+10))
        reader.next_scan=0
        with mock.patch.object(dashboard.TelemetryFollower,'bootstrap',side_effect=OSError('fixture')):
            with self.assertRaises(OSError):reader.poll()
        self.assertIs(reader.model,old)
        self.assertIs(reader.follower,old_follower)
        self.assertEqual(reader.model.claimed['outputs'],87)
        self.assertEqual(reader.poll().claimed['outputs'],0)
        self.assertEqual(reader.follower.path,second)

    @mock.patch.object(dashboard,'load_wallet_status')
    def test_atomic_same_path_replacement_resets_session(self, wallet):
        path=self.telemetry('one',True)
        reader=dashboard.TelemetrySessionReader(self.root,path,False);self.addCleanup(reader.close)
        self.assertEqual(reader.poll().claimed['outputs'],87)
        replacement=path.with_suffix('.new')
        replacement.write_text(json.dumps({'event':'STARTUP_STATE','state':'BOOT','result':'PASS'})+'\n')
        replacement.replace(path);reader.next_scan=0
        self.assertEqual(reader.poll().claimed['outputs'],0)

    def test_presentation_is_independent_and_omits_dedup_state(self):
        model=dashboard.DashboardModel();model._claim_acks['private-index']=(87,123)
        model.stats['rpc_ack']=2
        view=model.presentation_copy()
        self.assertEqual(view._claim_acks,{})
        model.stats['rpc_ack']=3
        self.assertEqual(view.stats['rpc_ack'],2)


    def test_controller_whitelist_and_persistent_temp_environment(self):
        for action in ('shutdown','restart'):
            command=controller_command(action,self.root,self.root/'source')
            self.assertIn('--property=Restart=no',command)
            self.assertIn('--setenv=TMPDIR='+str(self.root/'work/tmp'),command)
            self.assertEqual(command.count('--force'),1 if action=='shutdown' else 0)
        with self.assertRaises(ValueError):controller_command('arbitrary-command',self.root,self.root)


if __name__=='__main__':unittest.main(verbosity=2)
