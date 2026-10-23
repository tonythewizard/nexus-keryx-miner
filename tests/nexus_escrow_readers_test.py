#!/usr/bin/env python3
import json
import sys
from pathlib import Path
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import nexus_escrow_availability as availability
from nexus_dashboard import TelemetryFollower, DashboardModel


class EscrowReaderTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.folder = self.root / 'state/ai-service'
        self.folder.mkdir(parents=True)
        self.state = self.folder / 'escrow_state.json'
        self.journal = self.folder / 'escrow_state.journal'
        self.row = {'coinbase_txid': 'a' * 64, 'output_index': 0, 'amount_sompi': 100,
                    'confirm_daa': 10, 'claimed': False, 'slashed': False}

    def records(self, entries, journal='', seq=0):
        self.state.write_text(json.dumps({'entries': entries, 'journal_seq': seq}))
        self.journal.write_text(journal)
        self.state.chmod(0o600); self.journal.chmod(0o600)
        with mock.patch.object(availability, 'ROOT', self.root):
            return availability.snapshot_records()

    def test_identical_duplicates_are_one_outpoint(self):
        self.assertEqual(len(self.records([self.row, self.row])), 1)

    def test_terminal_duplicate_is_never_exposed_as_unclaimed(self):
        for flag in ('claimed', 'slashed'):
            self.assertTrue(self.records([self.row, {**self.row, flag: True}])[0][flag])

    def test_conflicting_amount_or_maturity_is_rejected(self):
        for field in ('amount_sompi', 'confirm_daa'):
            with self.assertRaises(ValueError):
                self.records([self.row, {**self.row, field: 999}])

    def test_gap_and_truncated_journal_fail_closed(self):
        for text in (json.dumps({'s': 2, 'e': self.row}) + '\n', '{'):
            with self.assertRaises(ValueError):
                self.records([self.row], text)

    def test_journal_replay_and_stale_lines(self):
        claimed = {**self.row, 'claimed': True}
        text = json.dumps({'s': 1, 'e': self.row}) + '\n' + json.dumps({'s': 2, 'e': claimed}) + '\n'
        self.assertTrue(self.records([self.row], text, seq=1)[0]['claimed'])

    def test_claim_totals_only_count_unique_ack(self):
        row = {'event': 'ESCROW_CLAIM_SUBMISSION', 'txid': 'b' * 64, 'inputs': 87, 'payout_sompi': 10}
        lines = [{**row, 'state': 'RPC_REJECTED'}, {**row, 'state': 'SUBMIT_SENT'},
                 {**row, 'state': 'RPC_ACKNOWLEDGED'}, {**row, 'state': 'RPC_ACKNOWLEDGED'}]
        (self.folder / 'claim-submissions.jsonl').write_text(''.join(json.dumps(r) + '\n' for r in lines))
        model = DashboardModel()
        for record in lines: model.ingest(json.dumps(record))
        self.assertEqual(model.claimed['transactions'], 1)
        self.assertEqual(model.claimed['outputs'], 87)
        self.assertEqual(model.claimed['payout_sompi'], 10)

    def test_conflicting_ack_totals_are_unavailable(self):
        row = {'event': 'ESCROW_CLAIM_SUBMISSION', 'state': 'RPC_ACKNOWLEDGED',
               'txid': 'b' * 64, 'inputs': 87, 'payout_sompi': 10}
        (self.folder / 'claim-submissions.jsonl').write_text(json.dumps(row) + '\n' + json.dumps({**row, 'payout_sompi': 11}) + '\n')
        model = DashboardModel()
        model.ingest(json.dumps(row)); model.ingest(json.dumps({**row, 'payout_sompi': 11}))
        self.assertIsNone(model.claimed['payout_sompi'])

    def test_new_worker_run_resets_claims_without_touching_journal(self):
        row = {'event': 'ESCROW_CLAIM_SUBMISSION', 'state': 'RPC_ACKNOWLEDGED',
               'txid': 'b' * 64, 'inputs': 87, 'payout_sompi': 10}
        journal = self.folder / 'claim-submissions.jsonl'
        journal.write_text(json.dumps(row) + '\n'); original = journal.read_bytes()
        first = self.root / 'run-one.jsonl'; second = self.root / 'run-two.jsonl'
        boot = {'event': 'STARTUP_STATE', 'state': 'BOOT', 'result': 'PASS'}
        first.write_text(json.dumps(boot) + '\n' + json.dumps(row) + '\n')
        second.write_text(json.dumps(boot) + '\n')
        for _ in range(2):  # Restarting only the dashboard retains this run's count.
            model = DashboardModel(); follower = TelemetryFollower(first)
            try: follower.bootstrap(model)
            finally: follower.close()
            self.assertEqual(model.claimed['transactions'], 1)
        model = DashboardModel(); follower = TelemetryFollower(second)
        try: follower.bootstrap(model)
        finally: follower.close()
        self.assertEqual(model.claimed['transactions'], 0)
        self.assertEqual(model.claimed['payout_sompi'], 0)
        self.assertEqual(journal.read_bytes(), original)

    def test_boot_resets_even_when_the_log_file_is_reused(self):
        model = DashboardModel()
        model.ingest(json.dumps({'event': 'ESCROW_CLAIM_SUBMISSION', 'state': 'RPC_ACKNOWLEDGED',
                                'txid': 'b' * 64, 'inputs': 87, 'payout_sompi': 10}))
        model.ingest(json.dumps({'event': 'STARTUP_STATE', 'state': 'BOOT', 'result': 'PASS'}))
        self.assertEqual(model.claimed['outputs'], 0)


if __name__ == '__main__':
    unittest.main()
