import sys
from pathlib import Path
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import nexus_agent_api as api
import nexus_dashboard as dashboard


class DiagnosticsTest(unittest.TestCase):
    def test_diagnostics_preserves_session_claims(self):
        agent = api.AgentAPI.__new__(api.AgentAPI)
        agent.lock = threading.RLock()
        agent.node_probe_cache = (time.monotonic(), {"synced": "true"})
        model = dashboard.DashboardModel()
        model.claimed = {"outputs": 7, "transactions": 1, "payout_sompi": 123, "status": "SESSION · ACK"}
        agent.diagnostic_reader = Mock()
        agent.diagnostic_reader.poll.return_value = model
        agent.availability_poller = SimpleNamespace(pump=lambda: {"status": "MOCK"})
        with patch.object(api, "run", return_value="ActiveState=inactive\nMainPID=0"), \
             patch.object(api, "worker_identity", return_value=[]), \
             patch.object(api, "recent_snapshot", return_value={}), \
             patch.object(dashboard, "gpu_sample", return_value="MOCK"):
            result = agent.diagnose()
        self.assertEqual(result["escrow_claimed"], model.claimed)
        agent.diagnostic_reader.poll.assert_called_once()


if __name__ == "__main__":
    unittest.main()
