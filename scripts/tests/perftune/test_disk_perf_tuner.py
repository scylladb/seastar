import argparse
import multiprocessing
import unittest
from unittest import mock

import perftune


def _make_args(**overrides):
    defaults = dict(
        mode=None, nics=[], tune_clock=False, get_cpu_mask=False,
        get_cpu_mask_quiet=False, get_irq_cpu_mask=False, verbose=False,
        tune=[], cpu_mask='0x000000ff', irq_cpu_mask=None, dirs=['/data'],
        devs=[], options_file=None, dump_options_file=False, dry_run=False,
        set_write_back=None, enable_arfs=None, num_rx_queues=None,
        cores_per_irq_core=16, tcp_mem_fraction=0.03,
    )
    defaults.update(overrides)
    return argparse.Namespace(**defaults)


def _make_disk_tuner(**arg_overrides):
    """
    Return a DiskPerfTuner with __init__ bypassed; only args and
    __irqs2procline / __disk2irqs / __type2diskinfo are set.
    """
    tuner = object.__new__(perftune.DiskPerfTuner)
    tuner._PerfTunerBase__args = _make_args(**arg_overrides)
    tuner._PerfTunerBase__irq_cpu_mask = '0x000000ff'
    tuner._PerfTunerBase__compute_cpu_mask = '0x000000ff'
    tuner._PerfTunerBase__mode = perftune.PerfTunerBase.SupportedModes.mq
    tuner._PerfTunerBase__is_aws_i3_nonmetal_instance = None
    tuner._PerfTunerBase__metadata_token_value = None
    tuner._PerfTunerBase__metadata_token_time = None
    return tuner


# ---------------------------------------------------------------------------
# __io_schedulers / __nomerges properties
# ---------------------------------------------------------------------------

class TestDiskPerfTunerSimpleProperties(unittest.TestCase):
    def test_io_schedulers(self):
        """
        __io_schedulers returns a list that starts with 'none' (highest
        priority scheduler) and also contains 'noop'.
        """
        tuner = _make_disk_tuner()
        schedulers = tuner._DiskPerfTuner__io_schedulers
        self.assertIn('none', schedulers)
        self.assertIn('noop', schedulers)
        self.assertEqual(schedulers[0], 'none')

    def test_nomerges(self):
        """
        __nomerges returns '2', which disables I/O request merging for lowest
        latency.
        """
        tuner = _make_disk_tuner()
        self.assertEqual(tuner._DiskPerfTuner__nomerges, '2')


# ---------------------------------------------------------------------------
# __write_cache_config property
# ---------------------------------------------------------------------------

class TestWriteCacheConfig(unittest.TestCase):
    def test_none_when_not_set(self):
        """
        When set_write_back is None (not configured), __write_cache_config
        returns None and no cache mode is applied.
        """
        tuner = _make_disk_tuner(set_write_back=None)
        self.assertIsNone(tuner._DiskPerfTuner__write_cache_config)

    def test_write_back_when_true(self):
        """
        When set_write_back is True, __write_cache_config returns 'write back'
        to enable write-back caching.
        """
        tuner = _make_disk_tuner(set_write_back=True)
        self.assertEqual(tuner._DiskPerfTuner__write_cache_config, 'write back')

    def test_write_through_when_false(self):
        """
        When set_write_back is False, __write_cache_config returns 'write
        through' to use the safer write-through mode.
        """
        tuner = _make_disk_tuner(set_write_back=False)
        self.assertEqual(tuner._DiskPerfTuner__write_cache_config, 'write through')


# ---------------------------------------------------------------------------
# __nvme_fast_path_irq_filter
# ---------------------------------------------------------------------------

class TestNvmeFastPathIrqFilter(unittest.TestCase):
    def _make(self, irqs2procline):
        tuner = _make_disk_tuner()
        tuner._DiskPerfTuner__irqs2procline = irqs2procline
        return tuner

    def test_fp_irq_within_cpu_count(self):
        """
        An NVMe queue IRQ whose queue number is within the CPU count is
        classified as a fast-path IRQ (returns True).
        """
        tuner = self._make({'10': '  10: nvme0q1'})
        with mock.patch('multiprocessing.cpu_count', return_value=8):
            self.assertTrue(
                tuner._DiskPerfTuner__nvme_fast_path_irq_filter('10'))

    def test_fp_irq_exceeds_cpu_count(self):
        """
        An NVMe queue IRQ whose queue number exceeds the CPU count is filtered
        out (returns False) as it is not a fast-path IRQ on this machine.
        """
        tuner = self._make({'10': '  10: nvme0q100'})
        with mock.patch('multiprocessing.cpu_count', return_value=8):
            self.assertFalse(
                tuner._DiskPerfTuner__nvme_fast_path_irq_filter('10'))

    def test_queue_zero_is_not_fp(self):
        """
        Queue 0 is a management queue, not fast-path.
        """
        tuner = self._make({'10': '  10: nvme0q0'})
        with mock.patch('multiprocessing.cpu_count', return_value=8):
            self.assertFalse(
                tuner._DiskPerfTuner__nvme_fast_path_irq_filter('10'))

    def test_non_nvme_irq(self):
        """
        An IRQ not associated with an NVMe queue (e.g. 'sda') returns False
        from the fast-path filter.
        """
        tuner = self._make({'10': '  10: sda'})
        with mock.patch('multiprocessing.cpu_count', return_value=8):
            self.assertFalse(
                tuner._DiskPerfTuner__nvme_fast_path_irq_filter('10'))


# ---------------------------------------------------------------------------
# __group_disks_info_by_type
# ---------------------------------------------------------------------------

class TestGroupDisksInfoByType(unittest.TestCase):
    def _make_with_disk2irqs(self, disk2irqs):
        tuner = _make_disk_tuner()
        tuner._PerfTunerBase__is_aws_i3_nonmetal_instance = False
        tuner._DiskPerfTuner__disk2irqs = disk2irqs
        tuner._DiskPerfTuner__irqs2procline = {}
        return tuner

    def test_nvme_and_non_nvme_separated(self):
        """
        __group_disks_info_by_type splits the disk→IRQ map into separate nvme
        and non_nvme entries in the returned dict.
        """
        tuner = self._make_with_disk2irqs({
            'nvme0n1': ['10', '11'],
            'sda': ['20'],
        })
        result = tuner._DiskPerfTuner__group_disks_info_by_type()
        nvme_disks, nvme_irqs = result[perftune.DiskPerfTuner.SupportedDiskTypes.nvme]
        non_nvme_disks, non_nvme_irqs = result[perftune.DiskPerfTuner.SupportedDiskTypes.non_nvme]

        self.assertIn('nvme0n1', nvme_disks)
        self.assertIn('sda', non_nvme_disks)
        self.assertIn('10', nvme_irqs)
        self.assertIn('11', nvme_irqs)
        self.assertIn('20', non_nvme_irqs)

    def test_no_disks_raises(self):
        """
        When disk2irqs is empty, __group_disks_info_by_type raises an Exception
        containing 'no disks were found'.
        """
        tuner = self._make_with_disk2irqs({})
        with self.assertRaises(Exception) as ctx:
            tuner._DiskPerfTuner__group_disks_info_by_type()
        self.assertIn('no disks were found', str(ctx.exception))

    def test_nvme_irqs_sorted(self):
        tuner = self._make_with_disk2irqs({
            'nvme0n1': ['30', '10', '20'],
        })
        result = tuner._DiskPerfTuner__group_disks_info_by_type()
        _, nvme_irqs = result[perftune.DiskPerfTuner.SupportedDiskTypes.nvme]
        self.assertEqual(nvme_irqs, sorted(nvme_irqs, key=int))

    def test_aws_i3_filters_fast_path_irqs(self):
        """
        On an AWS i3 non-metal instance, NVMe IRQs whose queue index exceeds
        the CPU count are excluded from the fast-path IRQ list.
        """
        tuner = self._make_with_disk2irqs({'nvme0n1': ['1', '2']})
        tuner._PerfTunerBase__is_aws_i3_nonmetal_instance = True
        tuner._DiskPerfTuner__irqs2procline = {
            '1': '  1: nvme0q1',
            '2': '  2: nvme0q100',
        }
        with mock.patch('multiprocessing.cpu_count', return_value=8):
            result = tuner._DiskPerfTuner__group_disks_info_by_type()
        _, nvme_irqs = result[perftune.DiskPerfTuner.SupportedDiskTypes.nvme]
        self.assertIn('1', nvme_irqs)
        self.assertNotIn('2', nvme_irqs)


# ---------------------------------------------------------------------------
# DiskPerfTuner._get_irqs
# ---------------------------------------------------------------------------

class TestDiskPerfTunerGetIrqs(unittest.TestCase):
    def test_chains_irqs_from_type2diskinfo(self):
        """
        _get_irqs() chains the IRQ lists from all disk type entries in
        __type2diskinfo into a single flat iterable.
        """
        tuner = _make_disk_tuner()
        tuner._DiskPerfTuner__type2diskinfo = {
            perftune.DiskPerfTuner.SupportedDiskTypes.nvme: (['nvme0'], ['10', '11']),
            perftune.DiskPerfTuner.SupportedDiskTypes.non_nvme: (['sda'], ['20']),
        }
        irqs = list(tuner._get_irqs())
        self.assertIn('10', irqs)
        self.assertIn('11', irqs)
        self.assertIn('20', irqs)

    def test_empty_type2diskinfo(self):
        """
        When all disk types have empty IRQ lists, _get_irqs() returns an empty
        iterator.
        """
        tuner = _make_disk_tuner()
        tuner._DiskPerfTuner__type2diskinfo = {
            perftune.DiskPerfTuner.SupportedDiskTypes.nvme: ([], []),
            perftune.DiskPerfTuner.SupportedDiskTypes.non_nvme: ([], []),
        }
        self.assertEqual(list(tuner._get_irqs()), [])


# ---------------------------------------------------------------------------
# __disks_info_by_type
# ---------------------------------------------------------------------------

class TestDisksInfoByType(unittest.TestCase):
    def test_returns_correct_type_tuple(self):
        """
        __disks_info_by_type returns the (disks, irqs) tuple for the requested
        disk type from __type2diskinfo.
        """
        tuner = _make_disk_tuner()
        tuner._DiskPerfTuner__type2diskinfo = {
            perftune.DiskPerfTuner.SupportedDiskTypes.nvme: (['nvme0n1'], ['10']),
            perftune.DiskPerfTuner.SupportedDiskTypes.non_nvme: (['sda'], ['20']),
        }
        disks, irqs = tuner._DiskPerfTuner__disks_info_by_type(
            perftune.DiskPerfTuner.SupportedDiskTypes.nvme)
        self.assertEqual(disks, ['nvme0n1'])
        self.assertEqual(irqs, ['10'])


# ---------------------------------------------------------------------------
# DiskPerfTuner.tune()
# ---------------------------------------------------------------------------

class TestDiskPerfTunerTune(unittest.TestCase):
    def _make_tuner_with_disks(self, nvme_disks, nvme_irqs, non_nvme_disks, non_nvme_irqs):
        tuner = _make_disk_tuner()
        tuner._PerfTunerBase__is_aws_i3_nonmetal_instance = False
        tuner._DiskPerfTuner__type2diskinfo = {
            perftune.DiskPerfTuner.SupportedDiskTypes.nvme: (nvme_disks, nvme_irqs),
            perftune.DiskPerfTuner.SupportedDiskTypes.non_nvme: (non_nvme_disks, non_nvme_irqs),
        }
        return tuner

    @mock.patch('perftune.distribute_irqs')
    @mock.patch('perftune.perftune_print')
    def test_tune_with_non_nvme_only(self, mock_print, mock_dist):
        """
        With only non-NVMe disks, tune() distributes their IRQs using
        irqs_cpu_mask and calls __tune_disks for them.
        """
        tuner = self._make_tuner_with_disks([], [], ['sda'], ['20'])
        with mock.patch.object(tuner, '_DiskPerfTuner__tune_disks') as mock_tune:
            tuner.tune()
        mock_dist.assert_called_once_with(['20'], '0x000000ff')
        mock_tune.assert_called_once_with(['sda'])

    @mock.patch('perftune.distribute_irqs')
    @mock.patch('perftune.perftune_print')
    def test_tune_with_nvme_only(self, mock_print, mock_dist):
        """
        With only NVMe disks, tune() distributes their IRQs using the full
        cpu_mask and calls __tune_disks for them.
        """
        tuner = self._make_tuner_with_disks(['nvme0n1'], ['10'], [], [])
        with mock.patch.object(tuner, '_DiskPerfTuner__tune_disks') as mock_tune:
            tuner.tune()
        # NVMe uses cpu_mask (not irqs_cpu_mask) and passes log_errors
        call_args = mock_dist.call_args
        self.assertEqual(call_args[0][0], ['10'])
        self.assertEqual(call_args[0][1], '0x000000ff')  # cpu_mask
        mock_tune.assert_called_once_with(['nvme0n1'])

    @mock.patch('perftune.perftune_print')
    def test_tune_no_non_nvme_prints_message(self, mock_print):
        """
        When there are no non-NVMe disks, tune() prints a 'No non-NVMe' notice.
        """
        tuner = self._make_tuner_with_disks(['nvme0n1'], ['10'], [], [])
        with mock.patch('perftune.distribute_irqs'), \
             mock.patch.object(tuner, '_DiskPerfTuner__tune_disks'):
            tuner.tune()
        printed = ' '.join(str(c) for c in mock_print.call_args_list)
        self.assertIn('No non-NVMe', printed)

    @mock.patch('perftune.perftune_print')
    def test_tune_no_nvme_prints_message(self, mock_print):
        """
        When there are no NVMe disks, tune() prints a 'No NVMe' notice.
        """
        tuner = self._make_tuner_with_disks([], [], ['sda'], ['20'])
        with mock.patch('perftune.distribute_irqs'), \
             mock.patch.object(tuner, '_DiskPerfTuner__tune_disks'):
            tuner.tune()
        printed = ' '.join(str(c) for c in mock_print.call_args_list)
        self.assertIn('No NVMe', printed)


# ---------------------------------------------------------------------------
# __learn_directory
# ---------------------------------------------------------------------------

class TestLearnDirectory(unittest.TestCase):
    @mock.patch('perftune.perftune_print')
    @mock.patch('os.path.exists', return_value=False)
    def test_nonexistent_directory_returns_empty(self, _, mock_print):
        """
        __learn_directory returns an empty list and prints a notice when the
        given directory does not exist (non-recursive call).
        """
        tuner = _make_disk_tuner()
        result = tuner._DiskPerfTuner__learn_directory('/nonexistent')
        self.assertEqual(result, [])
        mock_print.assert_called_once()

    @mock.patch('os.path.exists', return_value=False)
    def test_nonexistent_in_recur_no_print(self, _):
        """
        In recursive mode, a missing directory returns an empty list silently
        without printing any message.
        """
        tuner = _make_disk_tuner()
        result = tuner._DiskPerfTuner__learn_directory('/nonexistent', recur=True)
        self.assertEqual(result, [])

    def test_pyudev_success_returns_phys_devices(self):
        """
        When pyudev.from_device_number succeeds, __learn_directory calls __get_phys_devices.
        """
        import pyudev
        tuner = _make_disk_tuner()
        tuner._DiskPerfTuner__pyudev_ctx = mock.MagicMock()

        mock_udev = mock.MagicMock()
        mock_udev.sys_path = '/sys/devices/pci/sda'
        mock_udev.device_node = '/dev/sda'

        mock_stat = mock.MagicMock()
        mock_stat.st_dev = 2049

        with mock.patch('os.path.exists', return_value=True), \
             mock.patch('os.stat', return_value=mock_stat), \
             mock.patch.object(pyudev.Devices, 'from_device_number', return_value=mock_udev):
            result = tuner._DiskPerfTuner__learn_directory('/mnt/data')
        self.assertEqual(result, ['sda'])


# ---------------------------------------------------------------------------
# __tune_write_back_cache
# ---------------------------------------------------------------------------

class TestTuneWriteBackCache(unittest.TestCase):
    def test_no_op_when_config_none(self):
        """
        When __write_cache_config is None (set_write_back not configured),
        __tune_write_back_cache returns True without writing anything.
        """
        tuner = _make_disk_tuner(set_write_back=None)
        tuner._DiskPerfTuner__write_back_cache_tuned_devs = set()
        result = tuner._DiskPerfTuner__tune_write_back_cache('/dev/sda')
        self.assertTrue(result)


# ---------------------------------------------------------------------------
# __tune_disk / __tune_disks
# ---------------------------------------------------------------------------

class TestTuneDisk(unittest.TestCase):
    def _make_with_empty_sets(self):
        tuner = _make_disk_tuner()
        tuner._DiskPerfTuner__io_scheduler_tuned_devs = set()
        tuner._DiskPerfTuner__nomerges_tuned_devs = set()
        tuner._DiskPerfTuner__write_back_cache_tuned_devs = set()
        return tuner

    @mock.patch('perftune.perftune_print')
    def test_tune_disk_no_scheduler_supported(self, mock_print):
        """
        When no supported I/O scheduler is available, __tune_disk prints
        'Not setting I/O Scheduler' and skips scheduler configuration.
        """
        tuner = self._make_with_empty_sets()
        with mock.patch.object(tuner, '_DiskPerfTuner__get_io_scheduler', return_value=None), \
             mock.patch.object(tuner, '_DiskPerfTuner__tune_nomerges', return_value=True), \
             mock.patch.object(tuner, '_DiskPerfTuner__tune_write_back_cache', return_value=True):
            tuner._DiskPerfTuner__tune_disk('sda')
        printed = ' '.join(str(c) for c in mock_print.call_args_list)
        self.assertIn('Not setting I/O Scheduler', printed)

    @mock.patch('perftune.perftune_print')
    def test_tune_disk_scheduler_feature_missing(self, mock_print):
        """
        When the scheduler sysfs feature file is not found, __tune_disk prints
        'feature not present' for the scheduler.
        """
        tuner = self._make_with_empty_sets()
        with mock.patch.object(tuner, '_DiskPerfTuner__get_io_scheduler', return_value='none'), \
             mock.patch.object(tuner, '_DiskPerfTuner__tune_io_scheduler', return_value=False), \
             mock.patch.object(tuner, '_DiskPerfTuner__tune_nomerges', return_value=True), \
             mock.patch.object(tuner, '_DiskPerfTuner__tune_write_back_cache', return_value=True):
            tuner._DiskPerfTuner__tune_disk('sda')
        printed = ' '.join(str(c) for c in mock_print.call_args_list)
        self.assertIn('feature not present', printed)

    @mock.patch('perftune.perftune_print')
    def test_tune_disk_nomerges_feature_missing(self, mock_print):
        """
        When __tune_nomerges returns False, __tune_disk prints a diagnostic
        mentioning 'nomerges'.
        """
        tuner = self._make_with_empty_sets()
        with mock.patch.object(tuner, '_DiskPerfTuner__get_io_scheduler', return_value='none'), \
             mock.patch.object(tuner, '_DiskPerfTuner__tune_io_scheduler', return_value=True), \
             mock.patch.object(tuner, '_DiskPerfTuner__tune_nomerges', return_value=False), \
             mock.patch.object(tuner, '_DiskPerfTuner__tune_write_back_cache', return_value=True):
            tuner._DiskPerfTuner__tune_disk('sda')
        printed = ' '.join(str(c) for c in mock_print.call_args_list)
        self.assertIn("nomerges", printed)

    @mock.patch('perftune.perftune_print')
    def test_tune_disk_write_cache_missing(self, mock_print):
        """
        When __tune_write_back_cache returns False, __tune_disk prints a
        message mentioning 'write_cache'.
        """
        tuner = self._make_with_empty_sets()
        with mock.patch.object(tuner, '_DiskPerfTuner__get_io_scheduler', return_value='none'), \
             mock.patch.object(tuner, '_DiskPerfTuner__tune_io_scheduler', return_value=True), \
             mock.patch.object(tuner, '_DiskPerfTuner__tune_nomerges', return_value=True), \
             mock.patch.object(tuner, '_DiskPerfTuner__tune_write_back_cache', return_value=False):
            tuner._DiskPerfTuner__tune_disk('sda')
        printed = ' '.join(str(c) for c in mock_print.call_args_list)
        self.assertIn('write_cache', printed)

    def test_tune_disks_iterates_all(self):
        """
        __tune_disks calls __tune_disk for every device in the provided list,
        in order.
        """
        tuner = self._make_with_empty_sets()
        tune_calls = []
        with mock.patch.object(tuner, '_DiskPerfTuner__tune_disk',
                               side_effect=lambda d: tune_calls.append(d)):
            tuner._DiskPerfTuner__tune_disks(['sda', 'nvme0n1'])
        self.assertEqual(tune_calls, ['sda', 'nvme0n1'])


# ---------------------------------------------------------------------------
# __get_io_scheduler
# ---------------------------------------------------------------------------

class TestGetIoScheduler(unittest.TestCase):
    def _make(self):
        tuner = _make_disk_tuner()
        tuner._DiskPerfTuner__io_scheduler_tuned_devs = set()
        return tuner

    @mock.patch('perftune.readlines', return_value=['none [mq-deadline] kyber'])
    def test_returns_highest_priority_match(self, _):
        """
        __get_io_scheduler returns the first scheduler from __io_schedulers that
        appears in the current scheduler sysfs file.
        """
        tuner = self._make()
        with mock.patch.object(tuner, '_DiskPerfTuner__get_feature_file',
                               return_value=('/sys/.../scheduler', '/dev/sda')):
            result = tuner._DiskPerfTuner__get_io_scheduler('/dev/sda')
        self.assertEqual(result, 'none')

    @mock.patch('perftune.readlines', return_value=['mq-deadline kyber'])
    def test_returns_none_when_no_supported_scheduler(self, _):
        """
        When none of the preferred schedulers appear in the sysfs file,
        __get_io_scheduler returns None.
        """
        tuner = self._make()
        with mock.patch.object(tuner, '_DiskPerfTuner__get_feature_file',
                               return_value=('/sys/.../scheduler', '/dev/sda')):
            result = tuner._DiskPerfTuner__get_io_scheduler('/dev/sda')
        self.assertIsNone(result)

    @mock.patch('perftune.readlines', return_value=[])
    def test_returns_none_when_no_lines(self, _):
        """
        An empty or missing scheduler file (readlines returns []) causes
        __get_io_scheduler to return None.
        """
        tuner = self._make()
        with mock.patch.object(tuner, '_DiskPerfTuner__get_feature_file',
                               return_value=(None, None)):
            result = tuner._DiskPerfTuner__get_io_scheduler('/dev/sda')
        self.assertIsNone(result)


# ---------------------------------------------------------------------------
# __get_feature_file — None-guard path
# ---------------------------------------------------------------------------

class TestGetFeatureFile(unittest.TestCase):
    def test_returns_none_none_when_dev_node_is_none(self):
        """
        When dev_node is None, __get_feature_file immediately returns (None, None)
        without attempting any device lookup.
        """
        tuner = _make_disk_tuner()
        result = tuner._DiskPerfTuner__get_feature_file(None, lambda p: p)
        self.assertEqual(result, (None, None))

    def test_returns_none_none_when_path_creator_is_none(self):
        """
        When the path_creator argument is None, __get_feature_file returns
        (None, None).
        """
        tuner = _make_disk_tuner()
        result = tuner._DiskPerfTuner__get_feature_file('/dev/sda', None)
        self.assertEqual(result, (None, None))


# ---------------------------------------------------------------------------
# __tune_one_feature
# ---------------------------------------------------------------------------

class TestTuneOneFeature(unittest.TestCase):
    def _make(self):
        tuner = _make_disk_tuner()
        tuner._DiskPerfTuner__io_scheduler_tuned_devs = set()
        tuner._DiskPerfTuner__nomerges_tuned_devs = set()
        tuner._DiskPerfTuner__write_back_cache_tuned_devs = set()
        return tuner

    def test_returns_false_when_no_feature_file(self):
        """
        When __get_feature_file returns (None, None), __tune_one_feature returns
        False indicating the feature is not available.
        """
        tuner = self._make()
        with mock.patch.object(tuner, '_DiskPerfTuner__get_feature_file',
                               return_value=(None, None)):
            result = tuner._DiskPerfTuner__tune_one_feature(
                '/dev/sda', lambda p: p, 'none', tuner._DiskPerfTuner__io_scheduler_tuned_devs)
        self.assertFalse(result)

    @mock.patch('perftune.fwriteln_and_log')
    def test_returns_true_and_writes_feature(self, mock_fw):
        """
        When a feature file path is found, __tune_one_feature writes the value
        via fwriteln_and_log and returns True.
        """
        tuner = self._make()
        with mock.patch.object(tuner, '_DiskPerfTuner__get_feature_file',
                               return_value=('/sys/sda/scheduler', '/dev/sda')):
            result = tuner._DiskPerfTuner__tune_one_feature(
                '/dev/sda', lambda p: p, 'none', tuner._DiskPerfTuner__io_scheduler_tuned_devs)
        self.assertTrue(result)
        mock_fw.assert_called_once_with('/sys/sda/scheduler', 'none')

    @mock.patch('perftune.fwriteln_and_log')
    def test_second_call_same_dev_does_not_rewrite(self, mock_fw):
        """
        __tune_one_feature tracks already-tuned devices; a second call for the
        same device is a no-op and fwriteln_and_log is called only once total.
        """
        tuner = self._make()
        with mock.patch.object(tuner, '_DiskPerfTuner__get_feature_file',
                               return_value=('/sys/sda/scheduler', '/dev/sda')):
            tuner._DiskPerfTuner__tune_one_feature(
                '/dev/sda', lambda p: p, 'none', tuner._DiskPerfTuner__io_scheduler_tuned_devs)
            tuner._DiskPerfTuner__tune_one_feature(
                '/dev/sda', lambda p: p, 'none', tuner._DiskPerfTuner__io_scheduler_tuned_devs)
        # Second call should not re-write
        mock_fw.assert_called_once()


# ---------------------------------------------------------------------------
# __tune_io_scheduler / __tune_nomerges / __tune_write_back_cache (1793, 1796, 1803)
# ---------------------------------------------------------------------------

class TestTuneFeatureMethods(unittest.TestCase):
    def _make(self):
        tuner = _make_disk_tuner()
        tuner._DiskPerfTuner__io_scheduler_tuned_devs = set()
        tuner._DiskPerfTuner__nomerges_tuned_devs = set()
        tuner._DiskPerfTuner__write_back_cache_tuned_devs = set()
        return tuner

    def test_tune_io_scheduler_calls_tune_one_feature(self):
        """
        __tune_io_scheduler delegates to __tune_one_feature and returns its
        result.
        """
        tuner = self._make()
        with mock.patch.object(tuner, '_DiskPerfTuner__tune_one_feature',
                               return_value=True) as mock_tune:
            result = tuner._DiskPerfTuner__tune_io_scheduler('/dev/sda', 'none')
        self.assertTrue(result)
        mock_tune.assert_called_once()

    def test_tune_nomerges_calls_tune_one_feature(self):
        """
        __tune_nomerges delegates to __tune_one_feature and returns its result.
        """
        tuner = self._make()
        with mock.patch.object(tuner, '_DiskPerfTuner__tune_one_feature',
                               return_value=True) as mock_tune:
            result = tuner._DiskPerfTuner__tune_nomerges('/dev/sda')
        self.assertTrue(result)
        mock_tune.assert_called_once()

    def test_tune_write_back_cache_when_config_set(self):
        """
        When a write-cache config is set, __tune_write_back_cache calls
        __tune_one_feature to write the mode and returns True.
        """
        tuner = _make_disk_tuner(set_write_back=True)
        tuner._DiskPerfTuner__write_back_cache_tuned_devs = set()
        with mock.patch.object(tuner, '_DiskPerfTuner__tune_one_feature',
                               return_value=True) as mock_tune:
            result = tuner._DiskPerfTuner__tune_write_back_cache('/dev/sda')
        self.assertTrue(result)
        mock_tune.assert_called_once()


# ---------------------------------------------------------------------------
# __learn_directories
# ---------------------------------------------------------------------------

class TestLearnDirectories(unittest.TestCase):
    def test_empty_dirs_returns_empty_dict(self):
        """
        __learn_directories with an empty dirs list returns an empty dict
        without performing any I/O.
        """
        tuner = _make_disk_tuner(dirs=[])
        result = tuner._DiskPerfTuner__learn_directories()
        self.assertEqual(result, {})

    @mock.patch('os.path.exists', return_value=False)
    @mock.patch('perftune.perftune_print')
    def test_nonexistent_dir_returns_empty_list(self, _, __):
        """
        A directory that does not exist produces an empty list in the result
        dict for that directory key.
        """
        tuner = _make_disk_tuner(dirs=['/mnt/data'])
        result = tuner._DiskPerfTuner__learn_directories()
        self.assertEqual(result['/mnt/data'], [])


# ---------------------------------------------------------------------------
# __get_phys_devices
# ---------------------------------------------------------------------------

class TestGetPhysDevices(unittest.TestCase):
    def _make(self):
        tuner = _make_disk_tuner()
        tuner._DiskPerfTuner__pyudev_ctx = mock.MagicMock()
        return tuner

    def test_non_virtual_returns_device_name(self):
        """
        A non-virtual block device (path not under /virtual/) is returned
        directly as a single-element list.
        """
        tuner = self._make()
        mock_udev = mock.MagicMock()
        mock_udev.sys_path = '/sys/devices/pci0000:00/0000:00:1f.2/block/sda'
        mock_udev.device_node = '/dev/sda'
        result = tuner._DiskPerfTuner__get_phys_devices(mock_udev)
        self.assertEqual(result, ['sda'])

    def test_virtual_device_with_no_slaves(self):
        """
        Virtual device with empty slaves list is treated as regular device.
        """
        tuner = self._make()
        mock_udev = mock.MagicMock()
        mock_udev.sys_path = '/sys/devices/virtual/block/dm-0'
        mock_udev.device_node = '/dev/dm-0'
        with mock.patch('os.listdir', return_value=[]):
            result = tuner._DiskPerfTuner__get_phys_devices(mock_udev)
        self.assertEqual(result, ['dm-0'])

    def test_virtual_device_with_slaves_recurses(self):
        """
        Virtual device with slaves recurses into each slave.
        """
        import pyudev
        tuner = self._make()
        mock_udev = mock.MagicMock()
        mock_udev.sys_path = '/sys/devices/virtual/block/dm-0'
        mock_udev.device_node = '/dev/dm-0'

        slave_udev = mock.MagicMock()
        slave_udev.sys_path = '/sys/devices/pci/sda'
        slave_udev.device_node = '/dev/sda'

        with mock.patch('os.listdir', return_value=['sda']), \
             mock.patch.object(pyudev.Devices, 'from_device_file', return_value=slave_udev):
            result = tuner._DiskPerfTuner__get_phys_devices(mock_udev)
        self.assertEqual(result, ['sda'])


# ---------------------------------------------------------------------------
# DiskPerfTuner.__learn_irqs
# ---------------------------------------------------------------------------

class TestDiskLearnIrqs(unittest.TestCase):
    @mock.patch('perftune.learn_all_irqs_one', return_value=['42'])
    def test_learns_irqs_for_devs(self, mock_learn):
        """
        __learn_irqs maps each device specified via --dev to its IRQ list by
        calling learn_all_irqs_one with the device's sysfs path.
        """
        import pyudev
        tuner = _make_disk_tuner(devs=['sda'])
        tuner._DiskPerfTuner__dir2disks = {}
        tuner._DiskPerfTuner__irqs2procline = {}
        tuner._DiskPerfTuner__pyudev_ctx = mock.MagicMock()

        mock_udev = mock.MagicMock()
        mock_udev.sys_path = '/sys/devices/pci0000:00/0000:00:1f.2/ata2/host1/target1:0:0/1:0:0:0/block/sda'

        with mock.patch.object(pyudev.Devices, 'from_device_file', return_value=mock_udev):
            result = tuner._DiskPerfTuner__learn_irqs()
        self.assertIn('sda', result)
        self.assertEqual(result['sda'], ['42'])

    @mock.patch('perftune.learn_all_irqs_one', return_value=[])
    def test_virtual_nvme_device_uses_device_subpath(self, mock_learn):
        """
        Virtual NVMe path: __learn_irqs builds dev_sys_path via nvme device name match.
        """
        import pyudev
        tuner = _make_disk_tuner(devs=['nvme0n1'])
        tuner._DiskPerfTuner__dir2disks = {}
        tuner._DiskPerfTuner__irqs2procline = {}
        tuner._DiskPerfTuner__pyudev_ctx = mock.MagicMock()

        mock_udev = mock.MagicMock()
        mock_udev.sys_path = '/sys/devices/virtual/nvme-subsystem/nvme-subsys0/nvme0n1'

        with mock.patch.object(pyudev.Devices, 'from_device_file', return_value=mock_udev):
            result = tuner._DiskPerfTuner__learn_irqs()
        self.assertIn('nvme0n1', result)

    def test_iscsi_device_gets_empty_irqs(self):
        import pyudev
        tuner = _make_disk_tuner(devs=['sda'])
        tuner._DiskPerfTuner__dir2disks = {}
        tuner._DiskPerfTuner__irqs2procline = {}
        tuner._DiskPerfTuner__pyudev_ctx = mock.MagicMock()

        mock_udev = mock.MagicMock()
        mock_udev.sys_path = '/sys/devices/platform/host0/session0/target0:0:0/0:0:0:0/block/sda'

        with mock.patch.object(pyudev.Devices, 'from_device_file', return_value=mock_udev):
            result = tuner._DiskPerfTuner__learn_irqs()
        self.assertEqual(result['sda'], [])

    def test_skip_already_processed_device(self):
        """
        Devices already added to disk2irqs via --dir are skipped when processing --dev.
        """
        import pyudev
        tuner = _make_disk_tuner(devs=['sda'])
        tuner._DiskPerfTuner__dir2disks = {'/mnt': ['sda']}  # sda already via dirs
        tuner._DiskPerfTuner__irqs2procline = {}
        tuner._DiskPerfTuner__pyudev_ctx = mock.MagicMock()

        mock_udev = mock.MagicMock()
        mock_udev.sys_path = '/sys/devices/platform/host0/session0/block/sda'

        with mock.patch.object(pyudev.Devices, 'from_device_file', return_value=mock_udev):
            result = tuner._DiskPerfTuner__learn_irqs()
        # sda processed once via dirs, devs entry is skipped
        self.assertEqual(result['sda'], [])


# ---------------------------------------------------------------------------
# __learn_directory — exception / df fallback path
# ---------------------------------------------------------------------------

class TestLearnDirectoryException(unittest.TestCase):
    @mock.patch('perftune.perftune_print')
    @mock.patch('perftune.run_read_only_command',
                return_value='Filesystem 1024-blocks\n/dev/sda1 1024\n')
    @mock.patch('os.path.exists', return_value=True)
    def test_raises_when_pyudev_fails_and_dev_path_found(self, _, mock_cmd, mock_print):
        """
        When pyudev fails and df -P returns a /dev/ path, raise Logic error.
        """
        import pyudev
        tuner = _make_disk_tuner()
        tuner._DiskPerfTuner__pyudev_ctx = mock.MagicMock()
        with mock.patch.object(pyudev.Devices, 'from_device_number',
                               side_effect=Exception("no dev")):
            with self.assertRaises(Exception) as ctx:
                tuner._DiskPerfTuner__learn_directory('/mnt/data')
        self.assertIn('Logic error', str(ctx.exception))

    @mock.patch('perftune.perftune_print')
    @mock.patch('perftune.run_read_only_command',
                return_value='Filesystem 1024-blocks\n/other/path 1024\n')
    def test_recurses_when_non_dev_filesystem(self, mock_cmd, mock_print):
        """
        When df returns a non-/dev/ path, recurse; if recursive dir missing, return [].
        """
        import pyudev
        tuner = _make_disk_tuner()
        tuner._DiskPerfTuner__pyudev_ctx = mock.MagicMock()

        exists_calls = [0]

        def exists_side(p):
            exists_calls[0] += 1
            return exists_calls[0] == 1  # True only for the first call

        with mock.patch('os.path.exists', side_effect=exists_side), \
             mock.patch.object(pyudev.Devices, 'from_device_number',
                               side_effect=Exception("no dev")):
            result = tuner._DiskPerfTuner__learn_directory('/mnt/data')
        self.assertEqual(result, [])


# ---------------------------------------------------------------------------
# __get_feature_file — main execution path
# ---------------------------------------------------------------------------

class TestGetFeatureFileMainPath(unittest.TestCase):
    def _make(self):
        tuner = _make_disk_tuner()
        tuner._DiskPerfTuner__io_scheduler_tuned_devs = set()
        return tuner

    @mock.patch('os.path.exists', return_value=True)
    def test_returns_feature_when_exists(self, _):
        import pyudev
        tuner = self._make()
        mock_udev = mock.MagicMock()
        mock_udev.sys_path = '/sys/devices/pci/sda'
        with mock.patch.object(pyudev.Devices, 'from_device_file', return_value=mock_udev), \
             mock.patch('pyudev.Context', return_value=mock.MagicMock()):
            result_file, result_node = tuner._DiskPerfTuner__get_feature_file(
                '/dev/sda', lambda p: p + '/queue/scheduler')
        self.assertEqual(result_file, '/sys/devices/pci/sda/queue/scheduler')
        self.assertEqual(result_node, '/dev/sda')

    @mock.patch('os.path.exists', return_value=False)
    def test_no_parent_returns_none_none(self, _):
        import pyudev
        tuner = self._make()
        mock_udev = mock.MagicMock()
        mock_udev.sys_path = '/sys/devices/pci/sda'
        mock_udev.parent = None
        with mock.patch.object(pyudev.Devices, 'from_device_file', return_value=mock_udev), \
             mock.patch('pyudev.Context', return_value=mock.MagicMock()):
            result = tuner._DiskPerfTuner__get_feature_file(
                '/dev/sda', lambda p: p + '/queue/scheduler')
        self.assertEqual(result, (None, None))

    @mock.patch('os.path.exists', side_effect=[False, True])
    def test_traverses_to_parent(self, _):
        import pyudev
        tuner = self._make()
        mock_parent = mock.MagicMock()
        mock_parent.sys_path = '/sys/devices/pci'
        mock_parent.device_node = '/dev/sda'
        mock_udev = mock.MagicMock()
        mock_udev.sys_path = '/sys/devices/pci/sda1'
        mock_udev.parent = mock_parent

        with mock.patch.object(pyudev.Devices, 'from_device_file',
                               side_effect=[mock_udev, mock_parent]), \
             mock.patch('pyudev.Context', return_value=mock.MagicMock()):
            result_file, result_node = tuner._DiskPerfTuner__get_feature_file(
                '/dev/sda1', lambda p: p + '/queue/scheduler')
        self.assertEqual(result_file, '/sys/devices/pci/queue/scheduler')


# ---------------------------------------------------------------------------
# DiskPerfTuner.__init__
# ---------------------------------------------------------------------------

class TestDiskPerfTunerInit(unittest.TestCase):
    def test_raises_when_no_dirs_or_devs(self):
        """
        DiskPerfTuner.__init__ raises an Exception mentioning 'disks' when
        neither --dir nor --dev arguments are provided.
        """
        with mock.patch('perftune.run_hwloc_calc', return_value='0x000000ff'):
            with self.assertRaises(Exception) as ctx:
                perftune.DiskPerfTuner(_make_args(mode='mq', dirs=[], devs=[]))
        self.assertIn("disks", str(ctx.exception))

    @mock.patch('perftune.get_irqs2procline_map', return_value={})
    @mock.patch('perftune.learn_all_irqs_one', return_value=[])
    @mock.patch('os.path.exists', return_value=False)
    @mock.patch('perftune.perftune_print')
    def test_init_with_dirs(self, mock_print, mock_exists, mock_learn, mock_irqmap):
        """
        When a --dir argument is given, __init__ proceeds without raising; the
        pyudev context is initialised.
        """
        import pyudev
        with mock.patch('perftune.run_hwloc_calc', return_value='0x000000ff'), \
             mock.patch('pyudev.Context', return_value=mock.MagicMock()):
            try:
                tuner = perftune.DiskPerfTuner(
                    _make_args(mode='mq', dirs=['/mnt/data'], devs=[]))
            except Exception as e:
                if 'no disks were found' in str(e):
                    return  # Expected when learn_directories returns no disks
                raise
        self.assertIsNotNone(tuner._DiskPerfTuner__pyudev_ctx)

    def test_init_completes_with_devs(self):
        """
        DiskPerfTuner.__init__ initialises the io_scheduler, nomerges and write_back_cache
        tuned-device sets.
        """
        import pyudev
        with mock.patch('perftune.run_hwloc_calc', return_value='0x000000ff'), \
             mock.patch('pyudev.Context', return_value=mock.MagicMock()), \
             mock.patch('perftune.get_irqs2procline_map', return_value={}), \
             mock.patch.object(perftune.DiskPerfTuner,
                               '_DiskPerfTuner__learn_directories', return_value={}), \
             mock.patch.object(perftune.DiskPerfTuner,
                               '_DiskPerfTuner__learn_irqs', return_value={'sda': []}), \
             mock.patch.object(perftune.DiskPerfTuner,
                               '_DiskPerfTuner__group_disks_info_by_type',
                               return_value={'non-nvme': (['sda'], [])}):
            tuner = perftune.DiskPerfTuner(_make_args(mode='mq', devs=['sda']))
        self.assertIsInstance(tuner._DiskPerfTuner__io_scheduler_tuned_devs, set)
        self.assertIsInstance(tuner._DiskPerfTuner__nomerges_tuned_devs, set)
        self.assertIsInstance(tuner._DiskPerfTuner__write_back_cache_tuned_devs, set)
