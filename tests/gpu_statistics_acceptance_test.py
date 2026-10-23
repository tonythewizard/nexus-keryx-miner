"""One focused CPU acceptance: producer-clock sums, attribution and live-log replay."""
import json
import os
from pathlib import Path
import sys
import time
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import nexus_dashboard as dashboard
import nexus_gpu_metrics as metrics
from nexus_presenter import build_presenter_frame
import nexus_dashboard_test as fixtures


class StatisticsAcceptance(unittest.TestCase):
    def test_statistics_and_per_device_block_pipeline(self):
        model=dashboard.DashboardModel()
        model.gpu_devices={'amd_devices':[{'id':'pci:0000:00:00.0','vram_bytes':8*2**30,
            'driver':'amdgpu','mode':'diagnostics','capabilities_status':'N/A',
            'metrics':{'power_w':50,'temperature_c':70,'vram_used_bytes':2*2**30}}],
            'selection':{'nvidia_pom':True,'amd_pom':True}}
        end=time.monotonic_ns();base=end-60*10**9
        for second in range(61):
            model.ingest(json.dumps(dict(event='AMD_LANE_STATE', attempts=second*100,
                completed=second*100, discarded=0, verified=0, busy=True,
                preparing=False, paused=False, failed=False)),observed_ns=end)
            snap=fixtures.DashboardTests.native_snapshot(base+second*10**9)
            snap['elapsed_ns']=second*10**9
            snap['counters'].update(eligible_attempts=second*1000,
                gpu_work_units_launched=second*1000,gpu_work_units_completed=second*1000,
                pom_steps_completed=second*1000*256)
            model.ingest(json.dumps(snap),observed_ns=end)
        with patch('time.monotonic_ns',return_value=end+1):
            for window in (1,10,60):
                rate=metrics.fresh_rates(model,window)
                self.assertEqual((rate['rtx'],rate['rx'],rate['total']),(1000,100,1100))
            data=metrics.summary(model,'80, 1024, 8192, 60, 100, 1500')
            self.assertEqual(data['board_power_total_w'],150)
            self.assertEqual(data['vram_used_total_bytes'],3*2**30)
            self.assertEqual(data['fresh_completed_total'],66000)
            self.assertIsNone(data['combined_raw_hashrate'])
            self.assertIsNone(data['combined_stale_fraction'])
            for char,source in [('a',''),('b',' source=AMD_VULKAN')]:
                h=char*64
                for _ in range(2):model.ingest(f'LIVE_SUBMIT_RESULT{source} state=RPC_ACKNOWLEDGED hash={h}')
                model.ingest(f'LIVE_CHAIN_STATUS state=CHAIN_CONFIRMED hash={h} selected_chain=true blue_score=100 selected_tip_blue_score=106')
            for key in ('submit_sent','rpc_ack','chain_observed','chain_confirmed'):
                self.assertEqual(model.stats[key],2)
                self.assertEqual(model.gpu_block_stats['rtx'][key],1)
                self.assertEqual(model.gpu_block_stats['rx'][key],1)
            h='c'*64
            model.ingest(f'LIVE_CHAIN_STATUS state=CHAIN_OBSERVED hash={h} selected_chain=false')
            self.assertEqual(model.gpu_block_unknown['chain_observed'],1)
            model.ingest(f'LIVE_SUBMIT_RESULT source=AMD_VULKAN state=RPC_ACKNOWLEDGED hash={h}')
            self.assertEqual(model.gpu_block_unknown['chain_observed'],0)
            self.assertEqual(model.gpu_block_stats['rx']['chain_observed'],2)
            frame=build_presenter_frame(46,160,model,'80, 1024, 8192, 60, 100, 1500',0)
            self.assertEqual(len(frame.panels),9)
            self.assertEqual(sum(p['title']=='GPU FLEET / AI' for p in frame.panels),1)
            self.assertTrue(any(p['title']=='REPORTED POWER / TOTAL' for p in frame.panels))
            self.assertIn('1.100 kH/s',str(frame.panels[0]['free']))
            gpu=build_presenter_frame(46,160,model,'UNKNOWN',1)
            self.assertEqual(len(gpu.panels),3)
            snap=fixtures.DashboardTests.native_snapshot(end+10**9)
            snap['elapsed_ns']=61*10**9
            snap['counters'].update(eligible_attempts=61000,gpu_work_units_launched=61000,
                gpu_work_units_completed=61000,pom_steps_completed=61000*256)
            model.ingest(json.dumps(snap))
        with patch('time.monotonic_ns',return_value=end+10**9+1):
            self.assertIsNone(metrics.fresh_rates(model,10)['total'])
        root=Path(os.environ.get("NEXUS_DATA_DIR", str(Path.home() / ".local/share/nexus-keryx-miner")))
        path=max((root/'logs').glob('nexus-ui-inference-*.log'),key=lambda p:p.stat().st_mtime)
        live=dashboard.DashboardModel()
        live.gpu_devices=model.gpu_devices
        with path.open() as f:
            if path.stat().st_size>128*2**20:f.seek(path.stat().st_size-64*2**20);f.readline()
            for line in f:
                if line.endswith('\n'):live.ingest(line)
        for key in live.gpu_block_unknown:
            self.assertEqual(live.stats[key],sum(s[key] for s in live.gpu_block_stats.values())+live.gpu_block_unknown[key])
        for tab in (0,1):self.assertTrue(build_presenter_frame(46,160,live,'UNKNOWN',tab).panels)
        out=root/'work/amd-inference/gpu-statistics-acceptance.json'
        out.write_text(json.dumps({'result':'PASS','log':str(path),'metrics':metrics.summary(live,'UNKNOWN'),
            'per_gpu_blocks':live.gpu_block_stats,'unknown':live.gpu_block_unknown,'all_blocks':live.stats},indent=2))


if __name__=='__main__':unittest.main(verbosity=2)
