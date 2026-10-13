import argparse
import unittest
from unittest import mock

import perftune


def _make_args(**overrides):
    defaults = dict(
        mode=None, nics=[], tune_clock=False, get_cpu_mask=False,
        get_cpu_mask_quiet=False, get_irq_cpu_mask=False, verbose=False,
        tune=[], cpu_mask=None, irq_cpu_mask=None, dirs=[], devs=[],
        options_file=None, dump_options_file=False, dry_run=False,
        set_write_back=None, enable_arfs=None, num_rx_queues=None,
        cores_per_irq_core=16, tcp_mem_fraction=0.03,
    )
    defaults.update(overrides)
    return argparse.Namespace(**defaults)


class TestClocksourceManager(unittest.TestCase):
    @mock.patch('perftune.run_read_only_command', return_value='kvm\n')
    def test_get_arch_kvm(self, mock_cmd):
        """
        When systemd-detect-virt returns 'kvm', ClocksourceManager._arch is
        set to 'kvm'.
        """
        mgr = perftune.ClocksourceManager(_make_args())
        self.assertEqual(mgr._arch, 'kvm')

    @mock.patch('perftune.run_read_only_command', return_value='amazon\n')
    def test_get_arch_amazon(self, mock_cmd):
        """
        'amazon' (AWS Nitro/KVM) is normalised to 'kvm' by
        ClocksourceManager.
        """
        mgr = perftune.ClocksourceManager(_make_args())
        self.assertEqual(mgr._arch, 'kvm')

    @mock.patch('perftune.run_read_only_command', return_value='google\n')
    def test_get_arch_google(self, mock_cmd):
        """
        'google' (GCE KVM) is normalised to 'kvm' by ClocksourceManager.
        """
        mgr = perftune.ClocksourceManager(_make_args())
        self.assertEqual(mgr._arch, 'kvm')

    @mock.patch('perftune.run_read_only_command', return_value='none\n')
    @mock.patch('platform.machine', return_value='x86_64')
    def test_get_arch_physical(self, mock_machine, mock_cmd):
        """
        When systemd-detect-virt returns 'none' (bare-metal), _arch is set to
        the platform machine string (e.g. 'x86_64').
        """
        mgr = perftune.ClocksourceManager(_make_args())
        self.assertEqual(mgr._arch, 'x86_64')

    @mock.patch('perftune.run_read_only_command', side_effect=Exception("no systemd"))
    @mock.patch('platform.machine', return_value='aarch64')
    def test_get_arch_no_systemd(self, mock_machine, mock_cmd):
        """
        If systemd-detect-virt is unavailable (raises an exception), _arch
        falls back to platform.machine().
        """
        mgr = perftune.ClocksourceManager(_make_args())
        self.assertEqual(mgr._arch, 'aarch64')

    @mock.patch('perftune.run_read_only_command', return_value='none\n')
    @mock.patch('platform.machine', return_value='x86_64')
    def test_preferred_x86(self, mock_machine, mock_cmd):
        """
        On x86_64 bare-metal the preferred clocksource is 'tsc'.
        """
        mgr = perftune.ClocksourceManager(_make_args())
        self.assertEqual(mgr.preferred(), 'tsc')

    @mock.patch('perftune.run_read_only_command', return_value='kvm\n')
    def test_preferred_kvm(self, mock_cmd):
        """
        On KVM the preferred clocksource is 'kvm-clock'.
        """
        mgr = perftune.ClocksourceManager(_make_args())
        self.assertEqual(mgr.preferred(), 'kvm-clock')

    @mock.patch('perftune.run_read_only_command', return_value='none\n')
    @mock.patch('platform.machine', return_value='x86_64')
    def test_setting_available_x86(self, mock_machine, mock_cmd):
        """
        setting_available() returns True on x86_64 because clocksource tuning
        is supported.
        """
        mgr = perftune.ClocksourceManager(_make_args())
        self.assertTrue(mgr.setting_available())

    @mock.patch('perftune.run_read_only_command', side_effect=Exception("no systemd"))
    @mock.patch('platform.machine', return_value='aarch64')
    def test_setting_not_available(self, mock_machine, mock_cmd):
        """
        setting_available() returns False on aarch64 (or other non-x86 bare
        metal), where clocksource tuning is not applicable.
        """
        mgr = perftune.ClocksourceManager(_make_args())
        self.assertFalse(mgr.setting_available())

    @mock.patch('builtins.open', mock.mock_open(read_data='tsc hpet acpi_pm\n'))
    @mock.patch('perftune.run_read_only_command', return_value='none\n')
    @mock.patch('platform.machine', return_value='x86_64')
    def test_preferred_clocksource_available(self, mock_machine, mock_cmd):
        """
        preferred_clocksource_available() returns True when the preferred
        source name appears in the available_clocksource sysfs file.
        """
        mgr = perftune.ClocksourceManager(_make_args())
        self.assertTrue(mgr.preferred_clocksource_available())

    @mock.patch('builtins.open', mock.mock_open(read_data='hpet acpi_pm\n'))
    @mock.patch('perftune.run_read_only_command', return_value='none\n')
    @mock.patch('platform.machine', return_value='x86_64')
    def test_preferred_clocksource_not_available(self, mock_machine, mock_cmd):
        """
        preferred_clocksource_available() returns False when the preferred
        source is absent from the available_clocksource list.
        """
        mgr = perftune.ClocksourceManager(_make_args())
        self.assertFalse(mgr.preferred_clocksource_available())

    @mock.patch('perftune.fwriteln')
    @mock.patch('perftune.run_read_only_command', return_value='none\n')
    @mock.patch('platform.machine', return_value='x86_64')
    def test_enforce_preferred(self, mock_machine, mock_cmd, mock_fwriteln):
        """
        enforce_preferred_clocksource() writes the preferred clocksource name
        to the current_clocksource sysfs path via fwriteln.
        """
        mgr = perftune.ClocksourceManager(_make_args())
        mgr.enforce_preferred_clocksource()
        mock_fwriteln.assert_called_once()
        call_args = mock_fwriteln.call_args
        self.assertIn('tsc', call_args[0][1])

    @mock.patch('perftune.run_read_only_command', return_value='none\n')
    @mock.patch('platform.machine', return_value='x86_64')
    def test_recommendation_if_unavailable(self, mock_machine, mock_cmd):
        """
        recommendation_if_unavailable() returns a string containing the
        preferred clocksource name that can be displayed to the user.
        """
        mgr = perftune.ClocksourceManager(_make_args())
        msg = mgr.recommendation_if_unavailable()
        self.assertIn('tsc', msg)

    @mock.patch('builtins.open', mock.mock_open(read_data='tsc\n'))
    @mock.patch('perftune.run_read_only_command', return_value='none\n')
    @mock.patch('platform.machine', return_value='x86_64')
    def test_current_clocksource(self, mock_machine, mock_cmd):
        """
        _current_clocksource() reads and returns the contents of the
        current_clocksource sysfs file.
        """
        mgr = perftune.ClocksourceManager(_make_args())
        self.assertEqual(mgr._current_clocksource(), 'tsc')
