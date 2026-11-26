import argparse
import unittest
from unittest import mock

import perftune


def _make_args(**overrides):
    defaults = dict(
        mode=None, nics=[], tune_clock=False, get_cpu_mask=False,
        get_cpu_mask_quiet=False, get_irq_cpu_mask=False, verbose=False,
        tune=[], cpu_mask='0x000000ff', irq_cpu_mask=None, dirs=[], devs=[],
        options_file=None, dump_options_file=False, dry_run=False,
        set_write_back=None, enable_arfs=None, num_rx_queues=None,
        cores_per_irq_core=16, tcp_mem_fraction=0.03,
    )
    defaults.update(overrides)
    return argparse.Namespace(**defaults)


def _make_system_tuner(**arg_overrides):
    """
    Construct SystemPerfTuner with all external I/O mocked.
    """
    with mock.patch('perftune.run_hwloc_calc', return_value='0x000000ff'), \
         mock.patch('perftune.run_read_only_command', return_value='none\n'), \
         mock.patch('platform.machine', return_value='x86_64'):
        return perftune.SystemPerfTuner(_make_args(mode='mq', **arg_overrides))


class TestSystemPerfTunerInit(unittest.TestCase):
    def test_creates_clocksource_manager(self):
        """
        SystemPerfTuner.__init__ creates and stores a ClocksourceManager
        instance as _clocksource_manager.
        """
        tuner = _make_system_tuner()
        self.assertIsInstance(tuner._clocksource_manager, perftune.ClocksourceManager)

    def test_cpu_mask_accessible(self):
        """
        After construction, the cpu_mask property returns the normalised CPU
        mask configured in the args namespace.
        """
        tuner = _make_system_tuner()
        self.assertEqual(tuner.cpu_mask, '0x000000ff')

    def test_mode_is_mq(self):
        """
        The tuner is initialised with mode='mq', so the mode property returns
        SupportedModes.mq.
        """
        tuner = _make_system_tuner()
        self.assertEqual(tuner.mode, perftune.PerfTunerBase.SupportedModes.mq)


class TestSystemPerfTunerGetIrqs(unittest.TestCase):
    def test_get_irqs_returns_empty(self):
        """
        SystemPerfTuner._get_irqs() always returns an empty iterator because
        system tuning does not assign specific IRQs.
        """
        tuner = _make_system_tuner()
        self.assertEqual(list(tuner._get_irqs()), [])


class TestSystemPerfTunerTune(unittest.TestCase):
    def test_tune_clock_false_does_nothing(self):
        """
        When tune_clock=False, tune() does not touch the clocksource manager
        at all — setting_available is never called.
        """
        tuner = _make_system_tuner(tune_clock=False)
        with mock.patch.object(tuner._clocksource_manager,
                               'setting_available') as mock_avail:
            tuner.tune()
            mock_avail.assert_not_called()

    @mock.patch('perftune.perftune_print')
    def test_tune_clock_setting_unavailable(self, mock_print):
        """
        When tune_clock=True but setting_available() returns False, tune()
        prints a 'not available' message and skips clocksource enforcement.
        """
        tuner = _make_system_tuner(tune_clock=True)
        with mock.patch.object(tuner._clocksource_manager,
                               'setting_available', return_value=False):
            tuner.tune()
            mock_print.assert_called_once()
            self.assertIn('not available', mock_print.call_args[0][0])

    @mock.patch('perftune.perftune_print')
    def test_tune_clock_preferred_not_available(self, mock_print):
        """
        When clocksource tuning is available but the preferred source is absent,
        tune() calls recommendation_if_unavailable() and prints its message.
        """
        tuner = _make_system_tuner(tune_clock=True)
        with mock.patch.object(tuner._clocksource_manager,
                               'setting_available', return_value=True), \
             mock.patch.object(tuner._clocksource_manager,
                               'preferred_clocksource_available', return_value=False), \
             mock.patch.object(tuner._clocksource_manager,
                               'recommendation_if_unavailable',
                               return_value='upgrade kernel'):
            tuner.tune()
            mock_print.assert_called_once_with('upgrade kernel')

    def test_tune_clock_enforces_when_available(self):
        """
        When both setting_available() and preferred_clocksource_available() are
        True, tune() calls enforce_preferred_clocksource() to apply the setting.
        """
        tuner = _make_system_tuner(tune_clock=True)
        with mock.patch.object(tuner._clocksource_manager,
                               'setting_available', return_value=True), \
             mock.patch.object(tuner._clocksource_manager,
                               'preferred_clocksource_available', return_value=True), \
             mock.patch.object(tuner._clocksource_manager,
                               'enforce_preferred_clocksource') as mock_enforce:
            tuner.tune()
            mock_enforce.assert_called_once()
