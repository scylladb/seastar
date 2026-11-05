"""
Unit tests for perftune.main() — the program entry point that parses
arguments, applies sanity checks, builds tuners, and runs tuning.

Each logical section of the function gets its own TestCase class so failures
are easy to locate.

Because main() calls argp.parse_args() internally, every test patches
``perftune.argp.parse_args`` to return a pre-built Namespace rather than
reading sys.argv.
"""
import argparse
import io
import itertools
import sys
import unittest
from unittest import mock

import perftune


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _make_args(**overrides):
    """
    Build an argparse.Namespace that satisfies all main() preconditions by
    default.  Individual tests override only what they care about.
    """
    defaults = dict(
        mode=None,
        nics=['eth0'],
        tune_clock=False,
        get_cpu_mask=False,
        get_cpu_mask_quiet=False,
        get_irq_cpu_mask=False,
        verbose=False,
        tune=['net'],
        cpu_mask='0x000000ff',
        irq_cpu_mask=None,
        dirs=[],
        devs=[],
        options_file=None,
        dump_options_file=False,
        dry_run=False,
        set_write_back=None,
        enable_arfs=None,
        num_rx_queues=None,
        cores_per_irq_core=16,
        tcp_mem_fraction=0.03,
    )
    defaults.update(overrides)
    return argparse.Namespace(**defaults)


def _mock_tuner(irqs=None, compute_cpu_mask='0x000000fe', irqs_cpu_mask='0x00000001'):
    """
    Return a MagicMock that behaves like a fully-initialised PerfTuner.
    """
    t = mock.MagicMock()
    t.irqs = irqs if irqs is not None else []
    t.compute_cpu_mask = compute_cpu_mask
    t.irqs_cpu_mask = irqs_cpu_mask
    return t


def _run_main(args):
    """
    Call perftune.main() with parse_args() mocked to return *args*.
    """
    with mock.patch('perftune.argp') as mock_argp:
        mock_argp.parse_args.return_value = args
        perftune.main()


# ---------------------------------------------------------------------------
# Early-exit guards
# ---------------------------------------------------------------------------

class TestMainEarlyExitGuards(unittest.TestCase):

    def test_no_tune_modes_exits_with_error(self):
        """
        main() exits with 'At least one tune mode MUST be given' when args.tune
        is empty. A mode/irq_cpu_mask conflict also causes an exit.
        """
        args = _make_args(tune=[])
        with self.assertRaises(SystemExit) as ctx:
            _run_main(args)
        self.assertIn("At least one tune mode MUST be given", str(ctx.exception))


        args = _make_args(mode='mq', irq_cpu_mask='0x00000001')
        with self.assertRaises(SystemExit) as ctx:
            _run_main(args)
        self.assertIn("Provide either tune mode or IRQs CPU mask - not both", str(ctx.exception))

    def test_mode_alone_does_not_trigger_conflict_guard(self):
        """
        Providing only a mode (no irq_cpu_mask) does not trigger the
        mode-vs-irq_cpu_mask conflict guard; main() proceeds normally.
        """
        args = _make_args(mode='mq')
        with mock.patch('perftune.NetPerfTuner') as m, \
             mock.patch('perftune.restart_irqbalance'):
            m.return_value = _mock_tuner()
            _run_main(args)  # must not raise

    def test_irq_cpu_mask_alone_does_not_trigger_conflict_guard(self):
        """
        Providing only irq_cpu_mask=None (no mode) does not trigger the conflict
        guard; main() proceeds without error.
        """
        args = _make_args(irq_cpu_mask=None)
        with mock.patch('perftune.NetPerfTuner') as m, \
             mock.patch('perftune.restart_irqbalance'):
            m.return_value = _mock_tuner()
            _run_main(args)  # must not raise

    # --- cores_per_irq_core minimum ---

    def test_cores_per_irq_core_below_minimum_exits(self):
        """
        When cores_per_irq_core is below the minimum allowed value, main()
        exits with a message containing 'irq_core_auto_detection_ratio' and the
        minimum value.
        """
        min_val = perftune.PerfTunerBase.min_cores_per_irq_core()
        args = _make_args(cores_per_irq_core=min_val - 1)
        with self.assertRaises(SystemExit) as ctx:
            _run_main(args)
        self.assertIn("irq_core_auto_detection_ratio", str(ctx.exception))
        self.assertIn(str(min_val), str(ctx.exception))

    def test_cores_per_irq_core_at_minimum_does_not_exit(self):
        """
        A cores_per_irq_core value exactly at the minimum is accepted; main()
        proceeds without error.
        """
        args = _make_args(cores_per_irq_core=perftune.PerfTunerBase.min_cores_per_irq_core())
        with mock.patch('perftune.NetPerfTuner') as m, \
             mock.patch('perftune.restart_irqbalance'):
            m.return_value = _mock_tuner()
            _run_main(args)  # must not raise

    def test_cores_per_irq_core_above_minimum_does_not_exit(self):
        """
        A cores_per_irq_core value above the minimum is accepted; main()
        proceeds without error.
        """
        args = _make_args(cores_per_irq_core=perftune.PerfTunerBase.min_cores_per_irq_core() + 10)
        with mock.patch('perftune.NetPerfTuner') as m, \
             mock.patch('perftune.restart_irqbalance'):
            m.return_value = _mock_tuner()
            _run_main(args)  # must not raise

    # --- irq_cpu_mask must be a subset of cpu_mask ---

    @mock.patch('perftune.run_hwloc_calc', side_effect=['0x000000ff', '0x000000fe'])
    def test_irq_cpu_mask_not_subset_exits(self, _mock_calc):
        """
        When hwloc-calc reports that irq_cpu_mask is not a subset of cpu_mask,
        main() exits with a message about 'must be a subset of CPU mask'.
        """
        # run_hwloc_calc([cpu_mask]) → '0x000000ff'
        # run_hwloc_calc([cpu_mask, irq_cpu_mask]) → '0x000000fe'  (differ → not a subset)
        args = _make_args(irq_cpu_mask='0x00000100')
        with self.assertRaises(SystemExit) as ctx:
            _run_main(args)
        self.assertIn("IRQ CPU mask", str(ctx.exception))
        self.assertIn("must be a subset of CPU mask", str(ctx.exception))

    @mock.patch('perftune.run_hwloc_calc', return_value='0x000000ff')
    def test_irq_cpu_mask_is_subset_does_not_exit(self, _mock_calc):
        """
        When irq_cpu_mask is a proper subset of cpu_mask (both hwloc-calc calls
        return the same value), main() does not exit.
        """
        # Both hwloc-calc calls return the same value → subset check passes
        args = _make_args(irq_cpu_mask='0x000000ff')
        with mock.patch('perftune.NetPerfTuner') as m, \
             mock.patch('perftune.restart_irqbalance'):
            m.return_value = _mock_tuner()
            _run_main(args)  # must not raise

    def test_no_irq_cpu_mask_skips_subset_check(self):
        """
        When irq_cpu_mask is None the subset check must not call run_hwloc_calc.
        """
        args = _make_args(irq_cpu_mask=None)
        with mock.patch('perftune.run_hwloc_calc') as mock_calc, \
             mock.patch('perftune.NetPerfTuner') as m, \
             mock.patch('perftune.restart_irqbalance'):
            m.return_value = _mock_tuner()
            _run_main(args)
        mock_calc.assert_not_called()


# ---------------------------------------------------------------------------
# Default-value population
# ---------------------------------------------------------------------------

class TestMainDefaults(unittest.TestCase):

    def _run(self, args):
        with mock.patch('perftune.NetPerfTuner') as m, \
             mock.patch('perftune.restart_irqbalance'):
            m.return_value = _mock_tuner()
            _run_main(args)

    def test_empty_nics_list_defaults_to_eth0(self):
        """
        When args.nics is an empty list, main() sets it to ['eth0'] before
        creating any tuner.
        """
        args = _make_args(nics=[])
        self._run(args)
        self.assertEqual(args.nics, ['eth0'])

    def test_nics_already_set_are_not_overridden(self):
        """
        Explicitly configured NICs are not replaced by the 'eth0' default.
        """
        args = _make_args(nics=['ens3', 'ens4'])
        self._run(args)
        self.assertEqual(args.nics, ['ens3', 'ens4'])

    @mock.patch('perftune.run_hwloc_calc', return_value='0x000000ff')
    def test_none_cpu_mask_is_populated_via_hwloc_calc_all(self, mock_calc):
        """
        When args.cpu_mask is None, main() calls run_hwloc_calc(['all']) and
        stores the result before creating tuners.
        """
        args = _make_args(cpu_mask=None)
        with mock.patch('perftune.NetPerfTuner') as m, \
             mock.patch('perftune.restart_irqbalance'):
            m.return_value = _mock_tuner()
            _run_main(args)
        mock_calc.assert_any_call(['all'])
        self.assertEqual(args.cpu_mask, '0x000000ff')

    def test_cpu_mask_already_set_is_not_overridden(self):
        """
        An explicit cpu_mask is not overwritten by the hwloc-calc default.
        """
        args = _make_args(cpu_mask='0x0000000f')
        self._run(args)
        self.assertEqual(args.cpu_mask, '0x0000000f')


# ---------------------------------------------------------------------------
# --dump-options-file path
# ---------------------------------------------------------------------------

class TestMainDumpOptionsFile(unittest.TestCase):

    @mock.patch('perftune.dump_config')
    def test_dump_options_file_true_calls_dump_config_and_exits_zero(self, mock_dump):
        """
        When dump_options_file=True, main() calls dump_config() once and then
        exits with code 0.
        """
        args = _make_args(dump_options_file=True)
        with self.assertRaises(SystemExit) as ctx:
            _run_main(args)
        mock_dump.assert_called_once_with(args)
        self.assertEqual(ctx.exception.code, 0)

    @mock.patch('perftune.dump_config')
    def test_dump_options_file_false_does_not_call_dump_config(self, mock_dump):
        """
        When dump_options_file=False, dump_config() is not called and main()
        proceeds to tuning.
        """
        args = _make_args(dump_options_file=False)
        with mock.patch('perftune.NetPerfTuner') as m, \
             mock.patch('perftune.restart_irqbalance'):
            m.return_value = _mock_tuner()
            _run_main(args)
        mock_dump.assert_not_called()


# ---------------------------------------------------------------------------
# Tuner instantiation
# ---------------------------------------------------------------------------

class TestMainTunerCreation(unittest.TestCase):

    @mock.patch('perftune.SystemPerfTuner')
    @mock.patch('perftune.NetPerfTuner')
    @mock.patch('perftune.DiskPerfTuner')
    def test_all_three_tune_modes_instantiate_all_tuners(self, mock_disk, mock_net, mock_sys):
        """
        tune=['disks', 'net', 'system'] causes all three tuner classes
        (DiskPerfTuner, NetPerfTuner, SystemPerfTuner) to be instantiated with
        the args namespace.
        """
        for m in (mock_disk, mock_net, mock_sys):
            m.return_value = _mock_tuner()
        args = _make_args(tune=['disks', 'net', 'system'])
        with mock.patch('perftune.restart_irqbalance'):
            _run_main(args)
        mock_disk.assert_called_once_with(args)
        mock_net.assert_called_once_with(args)
        mock_sys.assert_called_once_with(args)

    @mock.patch('perftune.SystemPerfTuner')
    @mock.patch('perftune.NetPerfTuner')
    @mock.patch('perftune.DiskPerfTuner')
    def test_only_disks_in_tune_creates_only_disk_tuner(self, mock_disk, mock_net, mock_sys):
        """
        tune=['disks'] creates only DiskPerfTuner; NetPerfTuner and
        SystemPerfTuner are not instantiated.
        """
        mock_disk.return_value = _mock_tuner()
        args = _make_args(tune=['disks'])
        with mock.patch('perftune.restart_irqbalance'):
            _run_main(args)
        mock_disk.assert_called_once_with(args)
        mock_net.assert_not_called()
        mock_sys.assert_not_called()

    @mock.patch('perftune.SystemPerfTuner')
    @mock.patch('perftune.NetPerfTuner')
    @mock.patch('perftune.DiskPerfTuner')
    def test_only_net_in_tune_creates_only_net_tuner(self, mock_disk, mock_net, mock_sys):
        """
        tune=['net'] creates only NetPerfTuner; the other two tuners are not
        instantiated.
        """
        mock_net.return_value = _mock_tuner()
        args = _make_args(tune=['net'])
        with mock.patch('perftune.restart_irqbalance'):
            _run_main(args)
        mock_net.assert_called_once_with(args)
        mock_disk.assert_not_called()
        mock_sys.assert_not_called()

    @mock.patch('perftune.SystemPerfTuner')
    @mock.patch('perftune.NetPerfTuner')
    @mock.patch('perftune.DiskPerfTuner')
    def test_only_system_in_tune_creates_only_system_tuner(self, mock_disk, mock_net, mock_sys):
        """
        tune=['system'] creates only SystemPerfTuner; the other two tuners are
        not instantiated.
        """
        mock_sys.return_value = _mock_tuner()
        args = _make_args(tune=['system'])
        with mock.patch('perftune.restart_irqbalance'):
            _run_main(args)
        mock_sys.assert_called_once_with(args)
        mock_disk.assert_not_called()
        mock_net.assert_not_called()


# ---------------------------------------------------------------------------
# Output modes: --get-cpu-mask / --get-cpu-mask-quiet / --get-irq-cpu-mask
# ---------------------------------------------------------------------------

class TestMainOutputModes(unittest.TestCase):

    @mock.patch('perftune.perftune_print')
    @mock.patch('perftune.NetPerfTuner')
    def test_get_cpu_mask_prints_compute_mask_of_first_tuner(self, mock_cls, mock_print):
        """
        When get_cpu_mask=True, main() prints the compute_cpu_mask of the first
        created tuner and does not proceed to tuning.
        """
        mock_cls.return_value = _mock_tuner(compute_cpu_mask='0x000000fe')
        _run_main(_make_args(get_cpu_mask=True))
        mock_print.assert_called_with('0x000000fe')

    @mock.patch('perftune.perftune_print')
    @mock.patch('perftune.NetPerfTuner')
    def test_get_cpu_mask_quiet_prints_compute_mask_of_first_tuner(self, mock_cls, mock_print):
        """
        get_cpu_mask_quiet=True has the same output behaviour as get_cpu_mask:
        it prints the compute mask without performing tuning.
        """
        mock_cls.return_value = _mock_tuner(compute_cpu_mask='0x000000fe')
        _run_main(_make_args(get_cpu_mask_quiet=True))
        mock_print.assert_called_with('0x000000fe')

    @mock.patch('perftune.perftune_print')
    @mock.patch('perftune.NetPerfTuner')
    def test_get_irq_cpu_mask_prints_irqs_cpu_mask_of_first_tuner(self, mock_cls, mock_print):
        """
        When get_irq_cpu_mask=True, main() prints the irqs_cpu_mask of the
        first tuner.
        """
        mock_cls.return_value = _mock_tuner(irqs_cpu_mask='0x00000001')
        _run_main(_make_args(get_irq_cpu_mask=True))
        mock_print.assert_called_with('0x00000001')

    @mock.patch('perftune.restart_irqbalance')
    @mock.patch('perftune.NetPerfTuner')
    def test_get_cpu_mask_does_not_call_restart_irqbalance(self, mock_cls, mock_restart):
        """
        In get_cpu_mask mode, main() does not restart irqbalance because no
        actual tuning is performed.
        """
        mock_cls.return_value = _mock_tuner()
        with mock.patch('perftune.perftune_print'):
            _run_main(_make_args(get_cpu_mask=True))
        mock_restart.assert_not_called()

    @mock.patch('perftune.restart_irqbalance')
    @mock.patch('perftune.NetPerfTuner')
    def test_get_irq_cpu_mask_does_not_call_restart_irqbalance(self, mock_cls, mock_restart):
        """
        In get_irq_cpu_mask mode, restart_irqbalance is not called.
        """
        mock_cls.return_value = _mock_tuner()
        with mock.patch('perftune.perftune_print'):
            _run_main(_make_args(get_irq_cpu_mask=True))
        mock_restart.assert_not_called()


# ---------------------------------------------------------------------------
# Normal tuning flow: restart_irqbalance + tuner.tune()
# ---------------------------------------------------------------------------

class TestMainNormalFlow(unittest.TestCase):

    @mock.patch('perftune.restart_irqbalance')
    @mock.patch('perftune.NetPerfTuner')
    def test_tune_calls_restart_irqbalance(self, mock_cls, mock_restart):
        """
        During normal tuning, main() calls restart_irqbalance exactly once
        (after all tuners have been created).
        """
        mock_cls.return_value = _mock_tuner(irqs=['10', '11'])
        _run_main(_make_args())
        mock_restart.assert_called_once()

    @mock.patch('perftune.restart_irqbalance')
    @mock.patch('perftune.NetPerfTuner')
    def test_restart_irqbalance_receives_tuner_irqs(self, mock_cls, mock_restart):
        """
        restart_irqbalance is called with the IRQ list collected from the
        tuner(s).
        """
        mock_cls.return_value = _mock_tuner(irqs=['10', '11'])
        _run_main(_make_args())
        irqs_passed = list(mock_restart.call_args[0][0])
        self.assertEqual(sorted(irqs_passed), ['10', '11'])

    @mock.patch('perftune.restart_irqbalance')
    @mock.patch('perftune.NetPerfTuner')
    def test_tune_method_called_on_tuner(self, mock_cls, mock_restart):
        """
        main() calls tune() on the created tuner to apply the actual tuning.
        """
        tuner = _mock_tuner()
        mock_cls.return_value = tuner
        _run_main(_make_args())
        tuner.tune.assert_called_once()

    @mock.patch('perftune.restart_irqbalance')
    @mock.patch('perftune.SystemPerfTuner')
    @mock.patch('perftune.NetPerfTuner')
    @mock.patch('perftune.DiskPerfTuner')
    def test_all_tuner_tune_methods_called(self, mock_disk, mock_net, mock_sys, mock_restart):
        """
        When all three tune modes are active, tune() is called on all three
        tuner instances.
        """
        disk_tuner = _mock_tuner(irqs=['5'])
        net_tuner = _mock_tuner(irqs=['10'])
        sys_tuner = _mock_tuner(irqs=['20'])
        mock_disk.return_value = disk_tuner
        mock_net.return_value = net_tuner
        mock_sys.return_value = sys_tuner
        _run_main(_make_args(tune=['disks', 'net', 'system']))
        disk_tuner.tune.assert_called_once()
        net_tuner.tune.assert_called_once()
        sys_tuner.tune.assert_called_once()

    @mock.patch('perftune.restart_irqbalance')
    @mock.patch('perftune.SystemPerfTuner')
    @mock.patch('perftune.NetPerfTuner')
    @mock.patch('perftune.DiskPerfTuner')
    def test_restart_irqbalance_receives_irqs_from_all_tuners(self, mock_disk, mock_net, mock_sys, mock_restart):
        """
        restart_irqbalance receives the union of IRQs reported by all active
        tuners (disks, net, system).
        """
        mock_disk.return_value = _mock_tuner(irqs=['5'])
        mock_net.return_value = _mock_tuner(irqs=['10'])
        mock_sys.return_value = _mock_tuner(irqs=['20'])
        _run_main(_make_args(tune=['disks', 'net', 'system']))
        mock_restart.assert_called_once()
        irqs_passed = set(mock_restart.call_args[0][0])
        self.assertEqual(irqs_passed, {'5', '10', '20'})


# ---------------------------------------------------------------------------
# Exception handling
# ---------------------------------------------------------------------------

class TestMainExceptionHandling(unittest.TestCase):

    # --- CPUMaskIsZeroException ---

    @mock.patch('perftune.perftune_print')
    @mock.patch('perftune.NetPerfTuner')
    def test_cpu_mask_zero_with_get_cpu_mask_quiet_prints_0x0(self, mock_cls, mock_print):
        """
        When a CPUMaskIsZeroException is raised and get_cpu_mask_quiet=True,
        main() prints '0x0' instead of exiting with an error.
        """
        mock_cls.side_effect = perftune.PerfTunerBase.CPUMaskIsZeroException("zero mask")
        _run_main(_make_args(get_cpu_mask_quiet=True))  # must NOT raise SystemExit
        mock_print.assert_called_with('0x0')

    @mock.patch('perftune.NetPerfTuner')
    def test_cpu_mask_zero_without_quiet_exits_with_descriptive_message(self, mock_cls):
        """
        A CPUMaskIsZeroException without quiet mode causes main() to exit with
        the exception message and a 'can't be tuned' note.
        """
        mock_cls.side_effect = perftune.PerfTunerBase.CPUMaskIsZeroException("zero mask")
        with self.assertRaises(SystemExit) as ctx:
            _run_main(_make_args(get_cpu_mask_quiet=False))
        self.assertIn("zero mask", str(ctx.exception))
        self.assertIn("can't be tuned until the issue is fixed", str(ctx.exception))

    @mock.patch('perftune.perftune_print')
    @mock.patch('perftune.NetPerfTuner')
    def test_cpu_mask_zero_quiet_true_takes_priority_over_get_cpu_mask(self, mock_cls, mock_print):
        """
        get_cpu_mask_quiet=True always prints '0x0', even when get_cpu_mask is also True.
        """
        mock_cls.side_effect = perftune.PerfTunerBase.CPUMaskIsZeroException("zero")
        _run_main(_make_args(get_cpu_mask=True, get_cpu_mask_quiet=True))  # must NOT raise
        mock_print.assert_called_with('0x0')

    @mock.patch('perftune.NetPerfTuner')
    def test_cpu_mask_zero_get_cpu_mask_true_but_quiet_false_exits(self, mock_cls):
        """
        When get_cpu_mask=True but get_cpu_mask_quiet=False, a
        CPUMaskIsZeroException still triggers a SystemExit.
        """
        mock_cls.side_effect = perftune.PerfTunerBase.CPUMaskIsZeroException("zero")
        with self.assertRaises(SystemExit):
            _run_main(_make_args(get_cpu_mask=True, get_cpu_mask_quiet=False))

    # --- InvalidNUMATopologyException ---

    @mock.patch('sys.stderr', new_callable=io.StringIO)
    @mock.patch('perftune.NetPerfTuner')
    def test_invalid_numa_topology_exits_with_code_3(self, mock_cls, mock_stderr):
        """
        An InvalidNUMATopologyException causes main() to exit with code 3,
        indicating a topology problem the user must fix.
        """
        mock_cls.side_effect = perftune.PerfTunerBase.InvalidNUMATopologyException("bad NUMA")
        with self.assertRaises(SystemExit) as ctx:
            _run_main(_make_args())
        self.assertEqual(ctx.exception.code, 3)

    @mock.patch('sys.stderr', new_callable=io.StringIO)
    @mock.patch('perftune.NetPerfTuner')
    def test_invalid_numa_topology_writes_message_to_stderr(self, mock_cls, mock_stderr):
        """
        The InvalidNUMATopologyException message and 'can't be tuned' text are
        written to stderr before exiting.
        """
        mock_cls.side_effect = perftune.PerfTunerBase.InvalidNUMATopologyException("bad NUMA")
        with self.assertRaises(SystemExit):
            _run_main(_make_args())
        output = mock_stderr.getvalue()
        self.assertIn("bad NUMA", output)
        self.assertIn("can't be tuned until the issue is fixed", output)

    # --- generic Exception ---

    @mock.patch('perftune.NetPerfTuner')
    def test_generic_exception_exits_with_message_containing_original_error(self, mock_cls):
        """
        A generic RuntimeError from tuner construction causes main() to exit
        with a message containing the original error text.
        """
        mock_cls.side_effect = RuntimeError("something went wrong")
        with self.assertRaises(SystemExit) as ctx:
            _run_main(_make_args())
        self.assertIn("something went wrong", str(ctx.exception))
        self.assertIn("can't be tuned until the issue is fixed", str(ctx.exception))

    @mock.patch('perftune.restart_irqbalance')
    @mock.patch('perftune.NetPerfTuner')
    def test_generic_exception_raised_during_tune_exits(self, mock_cls, mock_restart):
        """
        An exception raised inside tuner.tune() is caught by main() and causes
        a SystemExit with the error message.
        """
        tuner = _mock_tuner()
        tuner.tune.side_effect = ValueError("tune failure")
        mock_cls.return_value = tuner
        with self.assertRaises(SystemExit) as ctx:
            _run_main(_make_args())
        self.assertIn("tune failure", str(ctx.exception))


if __name__ == '__main__':
    unittest.main()
