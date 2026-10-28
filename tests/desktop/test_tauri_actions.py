"""Desktop parity checks: isolated mocks only; never stops miners or writes identity."""
import copy
import sys
import threading
from types import SimpleNamespace
import unittest
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
import nexus_tauri_actions as actions
import nexus_ui_preferences as preferences


class DesktopActionTests(unittest.TestCase):
    def test_unknown_actions_fail_closed(self):
        for name in ('shell', 'claim', 'submit', '../restart', None, ['stop']):
            with self.subTest(name=name), self.assertRaises(ValueError):
                actions.action(name, '', True)

    def test_confirmation_required_for_sensitive_actions(self):
        for name in actions.CONFIRM:
            with self.subTest(name=name), self.assertRaises(ValueError):
                actions.action(name, '')

    def test_presentation_preferences_preserve_workloads(self):
        original=copy.deepcopy(preferences.DEFAULTS)
        with patch.object(preferences,'load',return_value=original), patch.object(preferences,'save',side_effect=preferences.validate) as save:
            actions.action('preferences',{'gpu_view':'dual'})
            self.assertEqual(save.call_args.args[0]['gpu_view'],'dual')
            self.assertEqual(save.call_args.args[0]['workloads'],{})

    def test_preferences_cannot_set_economic_fields(self):
        for field in ('payout','escrow_key','submit','claims','model'):
            with patch.object(preferences,'load',return_value=copy.deepcopy(preferences.DEFAULTS)), patch.object(preferences,'save') as save:
                with self.assertRaises(ValueError): actions.action('preferences',{field:'changed'})
                save.assert_not_called()

    def test_invalid_colors_rejected(self):
        with patch.object(preferences,'load',return_value=copy.deepcopy(preferences.DEFAULTS)), patch.object(preferences,'save',side_effect=preferences.validate):
            with self.assertRaises(ValueError): actions.action('preferences',{'text':'#050805'})

    def test_setup_uses_existing_protections(self):
        import nexus_setup
        with patch.object(nexus_setup,'configure',side_effect=ValueError('Existing payout is protected')) as configure:
            with self.assertRaisesRegex(ValueError,'protected'):actions.action('payout','example',True)
            configure.assert_called_once_with('payout','example')

    def test_controller_is_detached_and_explicit(self):
        import nexus_desktop_commands
        with patch.object(actions,'production_source',return_value=actions.SOURCE), patch.object(nexus_desktop_commands,'controller_command',return_value=['controller']) as controller, patch.object(actions.subprocess,'run') as run:
            actions.action('restart','',True)
            controller.assert_called_once_with('restart',actions.ROOT,actions.SOURCE)
            self.assertEqual(run.call_args.args[0],['controller'])

    def test_api_copy_does_not_rotate(self):
        import nexus_mcp_control
        with patch.object(nexus_mcp_control,'client_token',return_value='synthetic-key'):
            result=actions.action('copy_api_key','')
            self.assertEqual(result['clipboard'],'synthetic-key')
            self.assertNotIn('synthetic-key',result['message'])

    def test_gpu_selection_validated_before_dispatch(self):
        with patch.object(actions.subprocess,'run') as run:
            for value in ({'device':'invalid','enabled':'yes'},{'enabled':True},{}):
                with self.assertRaises(ValueError):actions.action('gpu_select',value,True)
            run.assert_not_called()

    def test_offline_mode_rejects_compute_activation(self):
        with self.assertRaises(ValueError):actions.action('gpu_mode','mining')

    def test_all_nine_pages_preserve_presenter_rows(self):
        import nexus_dashboard
        import nexus_tauri_bridge
        import nexus_presenter
        model=nexus_dashboard.DashboardModel()
        model.fixture_mode=True
        mirror=SimpleNamespace(lock=threading.Lock(),model=model,gpu='UNKNOWN')
        values=copy.deepcopy(preferences.DEFAULTS)
        with patch.object(preferences,'load',return_value=values):
            for page in range(9):
                with self.subTest(page=page):
                    actual=nexus_tauri_bridge.presentation(mirror,page,'ALL')
                    expected=nexus_presenter.build_presenter_frame(46,160,model,'UNKNOWN',page,'ALL',preferences=values)
                    self.assertEqual(actual['panels'],expected.panels)
                    self.assertNotIn('escrow_public_key',actual)
                    self.assertNotIn('setup_key_exists',actual)

    def test_presenter_preserves_all_pages_at_supported_viewport_heights(self):
        import nexus_dashboard
        import nexus_tauri_bridge
        import nexus_presenter
        model = nexus_dashboard.DashboardModel()
        model.fixture_mode = True
        mirror = SimpleNamespace(lock=threading.Lock(), model=model, gpu='UNKNOWN')
        values = copy.deepcopy(preferences.DEFAULTS)
        with patch.object(preferences, 'load', return_value=values):
            for height in (16, 32, 46):
                for page in range(9):
                    with self.subTest(height=height, page=page):
                        actual = nexus_tauri_bridge.presentation(mirror, page, 'ALL', height=height)
                        expected = nexus_presenter.build_presenter_frame(height, 160, model, 'UNKNOWN', page, 'ALL', preferences=values)
                        self.assertEqual(actual['panels'], expected.panels)
                        self.assertNotIn('escrow_public_key', actual)
                        self.assertNotIn('setup_key_exists', actual)


if __name__=='__main__':unittest.main()
