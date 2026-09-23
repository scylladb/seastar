import argparse
import sys
import unittest
import urllib.error
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


class _ConcreteTuner(perftune.PerfTunerBase):
    """
    Minimal concrete subclass of PerfTunerBase for testing.
    """
    def tune(self): pass
    def _get_irqs(self): return []


# ---------------------------------------------------------------------------
# PerfTunerBase.__init__ — three initialisation paths
# ---------------------------------------------------------------------------

class TestPerfTunerBaseInitWithMode(unittest.TestCase):
    @mock.patch('perftune.run_hwloc_calc', side_effect=[
        '0x000000ff',  # init: normalise cpu_mask
        '0x000000fe',  # compute_cpu_mask_for_mode (sq): cpu_mask & ~PU:0
        '0x000000fe',  # irqs_cpu_mask_for_mode -> compute_cpu_mask_for_mode again
        '0x00000001',  # irqs_cpu_mask_for_mode: cpu_mask & ~compute
    ])
    def test_init_mode_sq(self, mock_calc):
        """
        When mode='sq' is passed, __init__ uses compute_cpu_mask_for_mode to
        exclude PU:0, then derives irqs_cpu_mask as the complement; both masks
        are stored and accessible via properties.
        """
        args = _make_args(mode='sq')
        tuner = _ConcreteTuner(args)
        self.assertEqual(tuner.mode, perftune.PerfTunerBase.SupportedModes.sq)
        self.assertEqual(tuner.compute_cpu_mask, '0x000000fe')
        self.assertEqual(tuner.irqs_cpu_mask, '0x00000001')

    @mock.patch('perftune.run_hwloc_calc', return_value='0x000000ff')
    def test_init_mode_mq(self, mock_calc):
        """
        In mq mode every CPU is available for both compute and IRQ work, so
        compute_cpu_mask and irqs_cpu_mask both equal the normalised cpu_mask.
        """
        args = _make_args(mode='mq')
        tuner = _ConcreteTuner(args)
        self.assertEqual(tuner.mode, perftune.PerfTunerBase.SupportedModes.mq)
        # mq: compute == cpu_mask, irqs == cpu_mask
        self.assertEqual(tuner.compute_cpu_mask, '0x000000ff')
        self.assertEqual(tuner.irqs_cpu_mask, '0x000000ff')

    @mock.patch('perftune.run_hwloc_calc', side_effect=[
        '0x000000ff',  # init: normalise cpu_mask
        '0x000000fc',  # compute_cpu_mask_for_mode (sq_split): cpu_mask & ~core:0
        '0x000000fc',  # irqs_cpu_mask_for_mode -> compute_cpu_mask_for_mode again
        '0x00000003',  # irqs_cpu_mask_for_mode
    ])
    def test_init_mode_sq_split(self, mock_calc):
        """
        In sq_split mode core:0 (including HT siblings) is excluded from
        compute; the resulting compute and IRQ masks reflect this split.
        """
        args = _make_args(mode='sq_split')
        tuner = _ConcreteTuner(args)
        self.assertEqual(tuner.mode, perftune.PerfTunerBase.SupportedModes.sq_split)
        self.assertEqual(tuner.compute_cpu_mask, '0x000000fc')
        self.assertEqual(tuner.irqs_cpu_mask, '0x00000003')


class TestPerfTunerBaseInitWithIrqCpuMask(unittest.TestCase):
    @mock.patch('perftune.run_hwloc_calc', side_effect=[
        '0x000000ff',  # init: normalise cpu_mask
        '0x00000001',  # irqs_cpu_mask setter: normalise irq_cpu_mask
        '0x000000ff',  # irqs_cpu_mask setter: normalise cpu_mask
        '0x000000fe',  # irqs_cpu_mask setter: cpu_mask & ~irq_cpu_mask
    ])
    def test_init_irq_cpu_mask_different(self, mock_calc):
        """
        When an explicit irq_cpu_mask is provided that differs from cpu_mask,
        __init__ sets irqs_cpu_mask to that value and compute_cpu_mask to the
        remaining CPUs (cpu_mask minus irq_cpu_mask).
        """
        args = _make_args(irq_cpu_mask='0x00000001')
        tuner = _ConcreteTuner(args)
        self.assertEqual(tuner.irqs_cpu_mask, '0x00000001')
        self.assertEqual(tuner.compute_cpu_mask, '0x000000fe')

    @mock.patch('perftune.run_hwloc_calc', side_effect=[
        '0x000000ff',  # init: normalise cpu_mask
        '0x000000ff',  # irqs_cpu_mask setter: normalise irq_cpu_mask
        '0x000000ff',  # irqs_cpu_mask setter: normalise cpu_mask (same)
    ])
    def test_init_irq_cpu_mask_equals_cpu_mask(self, mock_calc):
        """
        When irq_cpu_mask equals cpu_mask, compute_cpu_mask is set to cpu_mask
        (the special-case path where IRQ and compute sets are identical).
        """
        args = _make_args(irq_cpu_mask='0x000000ff')
        tuner = _ConcreteTuner(args)
        self.assertEqual(tuner.irqs_cpu_mask, '0x000000ff')
        # When irq == cpu, compute_cpu_mask = cpu_mask
        self.assertEqual(tuner.compute_cpu_mask, '0x000000ff')


class TestPerfTunerBaseInitAutoDetect(unittest.TestCase):
    @mock.patch('perftune.auto_detect_irq_mask', return_value='0x00000001')
    @mock.patch('perftune.check_sysfs_numa_topology_is_valid', return_value=True)
    @mock.patch('perftune.run_hwloc_calc', side_effect=[
        '0x000000ff',  # init: normalise cpu_mask
        '0x00000001',  # irqs_cpu_mask setter: normalise irq_cpu_mask
        '0x000000ff',  # irqs_cpu_mask setter: normalise cpu_mask
        '0x000000fe',  # irqs_cpu_mask setter: compute
    ])
    def test_autodetect_valid_topology(self, mock_calc, mock_check, mock_detect):
        """
        With a valid NUMA topology and no explicit mode or irq_cpu_mask, __init__
        calls auto_detect_irq_mask with the normalised cpu_mask and
        cores_per_irq_core, then stores the returned mask as irqs_cpu_mask.
        """
        args = _make_args()
        tuner = _ConcreteTuner(args)
        mock_check.assert_called_once()
        mock_detect.assert_called_once_with('0x000000ff', 16)
        self.assertEqual(tuner.irqs_cpu_mask, '0x00000001')

    @mock.patch('perftune.check_sysfs_numa_topology_is_valid', return_value=False)
    @mock.patch('perftune.run_hwloc_calc', return_value='0x000000ff')
    def test_autodetect_invalid_topology_raises(self, mock_calc, mock_check):
        """
        When check_sysfs_numa_topology_is_valid returns False, __init__ raises
        InvalidNUMATopologyException before calling auto_detect_irq_mask.
        """
        args = _make_args()
        with self.assertRaises(perftune.PerfTunerBase.InvalidNUMATopologyException):
            _ConcreteTuner(args)


# ---------------------------------------------------------------------------
# PerfTunerBase properties
# ---------------------------------------------------------------------------

class TestPerfTunerBaseProperties(unittest.TestCase):
    def _make_tuner(self):
        with mock.patch('perftune.run_hwloc_calc', return_value='0x000000ff'):
            return _ConcreteTuner(_make_args(mode='mq'))

    def test_cpu_mask(self):
        """
        The cpu_mask property returns the normalised CPU mask stored during
        __init__.
        """
        tuner = self._make_tuner()
        self.assertEqual(tuner.cpu_mask, '0x000000ff')

    def test_cores_per_irq_core(self):
        """
        The cores_per_irq_core property reads the value from the args namespace
        provided to __init__.
        """
        tuner = self._make_tuner()
        self.assertEqual(tuner.cores_per_irq_core, 16)

    def test_args_property(self):
        """
        The args property exposes the argparse Namespace passed to __init__.
        """
        tuner = self._make_tuner()
        self.assertIsNotNone(tuner.args)

    def test_irqs_property(self):
        """
        The irqs property delegates to the concrete _get_irqs() implementation;
        the test stub returns an empty list.
        """
        tuner = self._make_tuner()
        self.assertEqual(list(tuner.irqs), [])

    def test_mode_getter_setter(self):
        """
        Setting mode to SupportedModes.sq recomputes compute_cpu_mask (all but
        PU:0) and irqs_cpu_mask (the complement), and the mode getter reflects
        the new value.
        """
        # _make_tuner() has its own inner mock.patch so the outer side_effect
        # list only needs to cover the subsequent tuner.mode = sq assignment.
        with mock.patch('perftune.run_hwloc_calc', side_effect=[
            '0x000000fe',  # compute_cpu_mask_for_mode(sq): cpu_mask & ~PU:0
            '0x000000fe',  # compute_cpu_mask_for_mode(sq) called again inside irqs_cpu_mask_for_mode
            '0x00000001',  # irqs_cpu_mask_for_mode result
        ]):
            tuner = self._make_tuner()
            tuner.mode = perftune.PerfTunerBase.SupportedModes.sq
            self.assertEqual(tuner.mode, perftune.PerfTunerBase.SupportedModes.sq)
            self.assertEqual(tuner.compute_cpu_mask, '0x000000fe')
            self.assertEqual(tuner.irqs_cpu_mask, '0x00000001')


# ---------------------------------------------------------------------------
# irqs_cpu_mask setter edge cases
# ---------------------------------------------------------------------------

class TestIrqsCpuMaskSetter(unittest.TestCase):
    def _make_base_tuner(self):
        with mock.patch('perftune.run_hwloc_calc', return_value='0x000000ff'):
            return _ConcreteTuner(_make_args(mode='mq'))

    @mock.patch('perftune.run_hwloc_calc', return_value='0x0')
    def test_zero_irq_mask_raises(self, _):
        """
        Setting irqs_cpu_mask to a zero mask raises CPUMaskIsZeroException
        because there would be no CPUs to handle IRQs.
        """
        tuner = self._make_base_tuner()
        with self.assertRaises(perftune.PerfTunerBase.CPUMaskIsZeroException):
            tuner.irqs_cpu_mask = '0x0'

    @mock.patch('perftune.run_hwloc_calc', side_effect=[
        '0x00000001',  # normalise irq_cpu_mask
        '0x000000ff',  # normalise cpu_mask
        '0x0',         # cpu_mask & ~irq -> zero compute
    ])
    def test_zero_compute_mask_raises(self, _):
        """
        When the resulting compute mask (cpu_mask minus irq_mask) is zero,
        setting irqs_cpu_mask raises CPUMaskIsZeroException.
        """
        tuner = self._make_base_tuner()
        with self.assertRaises(perftune.PerfTunerBase.CPUMaskIsZeroException):
            tuner.irqs_cpu_mask = '0x00000001'


# ---------------------------------------------------------------------------
# PerfTunerBase.is_aws_i3_non_metal_instance / __check_host_type
# ---------------------------------------------------------------------------

class TestCheckHostType(unittest.TestCase):
    def _make_tuner(self):
        with mock.patch('perftune.run_hwloc_calc', return_value='0x000000ff'):
            return _ConcreteTuner(_make_args(mode='mq'))

    @mock.patch('urllib.request.urlopen')
    def test_aws_i3_non_metal(self, mock_urlopen):
        """
        When the EC2 metadata endpoint reports an i3.* non-metal instance type,
        is_aws_i3_non_metal_instance returns True.
        """
        cm = mock.MagicMock()
        cm.__enter__.return_value.read.return_value = b'i3.4xlarge'
        mock_urlopen.return_value = cm
        tuner = self._make_tuner()
        self.assertTrue(tuner.is_aws_i3_non_metal_instance)

    @mock.patch('urllib.request.urlopen')
    def test_aws_i3_metal(self, mock_urlopen):
        """
        An i3.metal instance type causes is_aws_i3_non_metal_instance to return
        False (metal instances are excluded).
        """
        cm = mock.MagicMock()
        cm.__enter__.return_value.read.return_value = b'i3.metal'
        mock_urlopen.return_value = cm
        tuner = self._make_tuner()
        self.assertFalse(tuner.is_aws_i3_non_metal_instance)

    @mock.patch('urllib.request.urlopen')
    def test_non_aws_url_error(self, mock_urlopen):
        """
        A URLError (e.g. not running on AWS) causes is_aws_i3_non_metal_instance
        to return False without propagating the exception.
        """
        mock_urlopen.side_effect = urllib.error.URLError("unreachable")
        tuner = self._make_tuner()
        self.assertFalse(tuner.is_aws_i3_non_metal_instance)

    @mock.patch('urllib.request.urlopen')
    def test_non_aws_timeout(self, mock_urlopen):
        """
        A TimeoutError when contacting the metadata endpoint results in False
        being returned by is_aws_i3_non_metal_instance.
        """
        mock_urlopen.side_effect = TimeoutError()
        tuner = self._make_tuner()
        self.assertFalse(tuner.is_aws_i3_non_metal_instance)

    @mock.patch('urllib.request.urlopen')
    def test_cached_after_first_call(self, mock_urlopen):
        """
        The result of the metadata lookup is cached; the second access to
        is_aws_i3_non_metal_instance does not call urlopen again.
        """
        mock_urlopen.side_effect = urllib.error.URLError("unreachable")
        tuner = self._make_tuner()
        _ = tuner.is_aws_i3_non_metal_instance
        _ = tuner.is_aws_i3_non_metal_instance
        # urlopen called only once — value is cached
        self.assertEqual(mock_urlopen.call_count, 1)


# ---------------------------------------------------------------------------
# __metadata_token — near-expiry refresh path
# ---------------------------------------------------------------------------

class TestMetadataTokenRefresh(unittest.TestCase):
    def _make_tuner(self):
        with mock.patch('perftune.run_hwloc_calc', return_value='0x000000ff'):
            return _ConcreteTuner(_make_args(mode='mq'))

    @mock.patch('urllib.request.urlopen')
    def test_token_refreshed_when_near_expiry(self, mock_urlopen):
        """
        When the cached metadata token was acquired more than the TTL ago,
        accessing __metadata_token fetches a fresh token via urlopen.
        """
        import datetime
        cm = mock.MagicMock()
        cm.__enter__.return_value.read.return_value = b'test-token'
        mock_urlopen.return_value = cm

        tuner = self._make_tuner()
        # Simulate an existing token that is nearly expired
        tuner._PerfTunerBase__metadata_token_value = 'old-token'
        tuner._PerfTunerBase__metadata_token_time = (
            datetime.datetime.now() - datetime.timedelta(seconds=21600))

        # Accessing the property via __check_host_type indirectly
        token = tuner._PerfTunerBase__metadata_token
        self.assertEqual(token, 'test-token')

    @mock.patch('urllib.request.urlopen')
    def test_token_reused_when_still_valid(self, mock_urlopen):
        """
        A recently acquired token is returned directly from the cache without
        calling urlopen again.
        """
        import datetime
        tuner = self._make_tuner()
        tuner._PerfTunerBase__metadata_token_value = 'valid-token'
        tuner._PerfTunerBase__metadata_token_time = datetime.datetime.now()

        token = tuner._PerfTunerBase__metadata_token
        self.assertEqual(token, 'valid-token')
        mock_urlopen.assert_not_called()


# ---------------------------------------------------------------------------
# __check_host_type — unexpected exception path
# ---------------------------------------------------------------------------

class TestCheckHostTypeUnexpectedException(unittest.TestCase):
    def _make_tuner(self):
        with mock.patch('perftune.run_hwloc_calc', return_value='0x000000ff'):
            return _ConcreteTuner(_make_args(mode='mq'))

    @mock.patch('urllib.request.urlopen', side_effect=RuntimeError("unexpected"))
    def test_unexpected_exception_sets_false(self, _):
        """
        An unexpected exception (not URLError or TimeoutError) from urlopen is
        also handled gracefully: is_aws_i3_non_metal_instance returns False.
        """
        tuner = self._make_tuner()
        self.assertFalse(tuner.is_aws_i3_non_metal_instance)


# ---------------------------------------------------------------------------
# compute_cpu_mask_for_mode / irqs_cpu_mask_for_mode edge cases
# ---------------------------------------------------------------------------

class TestStaticMaskMethods(unittest.TestCase):
    def test_irqs_cpu_mask_zero_raises(self):
        """
        irqs_cpu_mask_for_mode raises CPUMaskIsZeroException when IRQ mask resolves to zero.
        """
        with mock.patch('perftune.run_hwloc_calc', side_effect=['0x000000fe', '0x0']):
            with self.assertRaises(perftune.PerfTunerBase.CPUMaskIsZeroException):
                perftune.PerfTunerBase.irqs_cpu_mask_for_mode(
                    perftune.PerfTunerBase.SupportedModes.sq, '0x000000ff')

    def test_abstract_tune_is_callable_as_super(self):
        """
        PerfTunerBase.tune() no-op body can be invoked via super().
        """
        with mock.patch('perftune.run_hwloc_calc', return_value='0x000000ff'):
            tuner = _ConcreteTuner(_make_args(mode='mq'))
        perftune.PerfTunerBase.tune(tuner)  # no-op base implementation

    def test_abstract_get_irqs_is_callable_as_super(self):
        """
        PerfTunerBase._get_irqs() no-op body can be invoked via super().
        """
        with mock.patch('perftune.run_hwloc_calc', return_value='0x000000ff'):
            tuner = _ConcreteTuner(_make_args(mode='mq'))
        result = perftune.PerfTunerBase._get_irqs(tuner)  # no-op base implementation
        self.assertIsNone(result)
