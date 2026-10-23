#!/usr/bin/env python3
"""Read-only device names and vendor policy; no GPU drivers are initialized."""
import sys
from pathlib import Path
import unittest
from unittest import mock
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
import nexus_gpu_identity as identity
from nexus_gpu_registry import records,workload_options,automatic_workload
from nexus_gpu_selection import CUDA_UUID

class IdentityTests(unittest.TestCase):
    def setUp(self):identity._CACHE.clear()

    def test_specific_amd_subsystem_board(self):
        result=mock.Mock(stdout='ID_MODEL_FROM_DATABASE=Ellesmere [Radeon RX 470/480/570/580] (Radeon secondary AMD GPU Armor 8G OC)\n')
        with mock.patch.object(identity.subprocess,'run',return_value=result) as run:
            name=identity.amd_name(Path('/sys/mock-gpu'))
            self.assertEqual(name,'AMD Radeon secondary AMD GPU Armor 8G OC')
            self.assertEqual(identity.display_name({'name':name}),'secondary AMD GPU Armor 8G OC')
            self.assertEqual(identity.amd_name(Path('/sys/mock-gpu')),name)
            run.assert_called_once()

    def test_ambiguous_chip_is_never_guessed(self):
        family='Ellesmere [Radeon RX 470/480/570/580]'
        with mock.patch.object(identity.subprocess,'run',return_value=mock.Mock(stdout='ID_MODEL_FROM_DATABASE='+family)):
            self.assertEqual(identity.amd_name(Path('/sys/mock-gpu')),family)
        self.assertEqual(identity.display_name({'name':family}),family)

    def test_excluded_or_disabled_nvidia_is_not_queried(self):
        with mock.patch.object(identity.subprocess,'run') as run:
            self.assertEqual(identity.nvidia_name('GPU-excluded',True),'')
            self.assertEqual(identity.nvidia_name(CUDA_UUID,False),'')
            run.assert_not_called()

    def test_nvidia_response_uuid_must_match(self):
        with mock.patch.object(identity.subprocess,'run',return_value=mock.Mock(stdout='Wrong adapter, GPU-other')):
            self.assertEqual(identity.nvidia_name(CUDA_UUID,True),'')

    def test_vendor_workload_options(self):
        for vendor in ('AMD','Intel'):
            self.assertEqual([key for _,key in workload_options({'vendor':vendor})],['auto','nexus','nexus_bda'])
        self.assertEqual(workload_options({'vendor':'unknown'}),[('Automatic','auto')])
        self.assertEqual(automatic_workload({'vendor':'AMD'}),'nexus')
        self.assertEqual(automatic_workload({'vendor':'Intel'}),'nexus')
        self.assertEqual([key for _,key in workload_options({'vendor':'NVIDIA'})],['auto','nvidia','nexus','nexus_bda'])

    def test_display_order_never_reassigns_runtime_counters(self):
        devices=[{'id':'amd','vendor':'AMD','name':'A','runtime_lane':'rx'},
                 {'id':'nvidia','vendor':'NVIDIA','name':'N','runtime_lane':'rtx'}]
        values=records({'devices':devices})
        self.assertEqual(values[0]['tag'],'GPU1')
        self.assertEqual(values[0]['runtime_lane'],'rx')
        self.assertEqual(values[1]['runtime_lane'],'rtx')
        self.assertIsNone(records({'devices':[{'id':'intel','vendor':'Intel'}]})[0]['runtime_lane'])

if __name__=='__main__':unittest.main(verbosity=2)
