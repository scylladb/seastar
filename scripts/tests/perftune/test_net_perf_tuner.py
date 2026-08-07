import sys
import unittest
from unittest import mock

import perftune


_SAMPLE_ETHTOOL_L_TYPICAL = [
    'Channel parameters for enP63242s1:',
    'Pre-set maximums:',
    'RX:\t\tn/a',
    'TX:\t\tn/a',
    'Other:\t\tn/a',
    'Combined:\t16',
    'Current hardware settings:',
    'RX:\t\tn/a',
    'TX:\t\tn/a',
    'Other:\t\tn/a',
    'Combined:\t2',
]

_SAMPLE_ETHTOOL_L_ALL_NUMERIC = [
    'Channel parameters for eth0:',
    'Pre-set maximums:',
    'RX:\t\t8',
    'TX:\t\t4',
    'Other:\t\t1',
    'Combined:\t16',
    'Current hardware settings:',
    'RX:\t\t4',
    'TX:\t\t2',
    'Other:\t\t1',
    'Combined:\t8',
]

_SAMPLE_ETHTOOL_L_ALL_NA = [
    'Channel parameters for eth0:',
    'Pre-set maximums:',
    'RX:\t\tn/a',
    'TX:\t\tn/a',
    'Other:\t\tn/a',
    'Combined:\tn/a',
    'Current hardware settings:',
    'RX:\t\tn/a',
    'TX:\t\tn/a',
    'Other:\t\tn/a',
    'Combined:\tn/a',
]


class TestGetEthtoolLInfo(unittest.TestCase):
    _parse = staticmethod(
        perftune.NetPerfTuner._NetPerfTuner__get_ethtool_l_info)

    @mock.patch('perftune.run_ethtool')
    def test_typical_output(self, mock_ethtool):
        """
        __get_ethtool_l_info parses typical ethtool -l output where RX/TX are
        n/a and combined channels are numeric, storing None for n/a values and
        integers for numeric ones.
        """
        mock_ethtool.return_value = _SAMPLE_ETHTOOL_L_TYPICAL
        result = self._parse('eth0')

        self.assertIsNone(result.preset_maximums.rx)
        self.assertIsNone(result.preset_maximums.tx)
        self.assertIsNone(result.preset_maximums.other)
        self.assertEqual(result.preset_maximums.combined, 16)

        self.assertIsNone(result.current_hardware_settings.rx)
        self.assertIsNone(result.current_hardware_settings.tx)
        self.assertIsNone(result.current_hardware_settings.other)
        self.assertEqual(result.current_hardware_settings.combined, 2)

    @mock.patch('perftune.run_ethtool')
    def test_all_numeric_values(self, mock_ethtool):
        """
        When all channel counts are numeric (no n/a), all four fields in both
        sections are parsed as integers.
        """
        mock_ethtool.return_value = _SAMPLE_ETHTOOL_L_ALL_NUMERIC
        result = self._parse('eth0')

        self.assertEqual(result.preset_maximums.rx, 8)
        self.assertEqual(result.preset_maximums.tx, 4)
        self.assertEqual(result.preset_maximums.other, 1)
        self.assertEqual(result.preset_maximums.combined, 16)

        self.assertEqual(result.current_hardware_settings.rx, 4)
        self.assertEqual(result.current_hardware_settings.tx, 2)
        self.assertEqual(result.current_hardware_settings.other, 1)
        self.assertEqual(result.current_hardware_settings.combined, 8)

    @mock.patch('perftune.run_ethtool')
    def test_all_na(self, mock_ethtool):
        """
        When all channel counts are 'n/a', all fields in both sections are
        stored as None.
        """
        mock_ethtool.return_value = _SAMPLE_ETHTOOL_L_ALL_NA
        result = self._parse('eth0')

        for field in ('rx', 'tx', 'other', 'combined'):
            self.assertIsNone(getattr(result.preset_maximums, field))
            self.assertIsNone(getattr(result.current_hardware_settings, field))

    @mock.patch('perftune.run_ethtool')
    def test_returns_correct_types(self, mock_ethtool):
        """
        The return value is an EthtoolLChannelInfo whose two fields are
        EthtoolChannelPropertiesValues instances.
        """
        mock_ethtool.return_value = _SAMPLE_ETHTOOL_L_TYPICAL
        result = self._parse('eth0')

        self.assertIsInstance(result, perftune.EthtoolLChannelInfo)
        self.assertIsInstance(result.preset_maximums, perftune.EthtoolChannelPropertiesValues)
        self.assertIsInstance(result.current_hardware_settings, perftune.EthtoolChannelPropertiesValues)

    @mock.patch('perftune.run_ethtool')
    def test_calls_ethtool_with_correct_args(self, mock_ethtool):
        """
        __get_ethtool_l_info invokes run_ethtool with ['-l', interface_name].
        """
        mock_ethtool.return_value = _SAMPLE_ETHTOOL_L_TYPICAL
        self._parse('enp5s0')
        mock_ethtool.assert_called_once_with(['-l', 'enp5s0'])

    @mock.patch('perftune.run_ethtool')
    def test_missing_section_raises(self, mock_ethtool):
        mock_ethtool.return_value = [
            'Channel parameters for eth0:',
            'Pre-set maximums:',
            'RX:\t\tn/a',
            'TX:\t\tn/a',
            'Other:\t\tn/a',
            'Combined:\t16',
        ]
        with self.assertRaises(ValueError) as ctx:
            self._parse('eth0')
        self.assertIn('missing sections', str(ctx.exception))

    @mock.patch('perftune.run_ethtool')
    def test_missing_channel_key_raises(self, mock_ethtool):
        mock_ethtool.return_value = [
            'Channel parameters for eth0:',
            'Pre-set maximums:',
            'RX:\t\tn/a',
            'TX:\t\tn/a',
            'Current hardware settings:',
            'RX:\t\tn/a',
            'TX:\t\tn/a',
            'Other:\t\tn/a',
            'Combined:\t2',
        ]
        with self.assertRaises(ValueError) as ctx:
            self._parse('eth0')
        self.assertIn('missing channel keys', str(ctx.exception))

    @mock.patch('perftune.run_ethtool')
    def test_empty_output_raises(self, mock_ethtool):
        """
        Empty ethtool output raises a ValueError because neither expected
        section can be located.
        """
        mock_ethtool.return_value = []
        with self.assertRaises(ValueError):
            self._parse('eth0')

    @mock.patch('perftune.run_ethtool')
    def test_extra_whitespace_in_values(self, mock_ethtool):
        """
        Leading/trailing whitespace around values is stripped correctly; numeric
        values are still parsed as integers.
        """
        mock_ethtool.return_value = [
            'Channel parameters for eth0:',
            'Pre-set maximums:',
            'RX:     n/a  ',
            'TX:     n/a  ',
            'Other:  n/a  ',
            'Combined:  32  ',
            'Current hardware settings:',
            'RX:     n/a  ',
            'TX:     n/a  ',
            'Other:  n/a  ',
            'Combined:  4  ',
        ]
        result = self._parse('eth0')
        self.assertEqual(result.preset_maximums.combined, 32)
        self.assertEqual(result.current_hardware_settings.combined, 4)

    @mock.patch('perftune.run_ethtool')
    def test_sections_reversed_order(self, mock_ethtool):
        """
        __get_ethtool_l_info correctly handles output where 'Current hardware
        settings' appears before 'Pre-set maximums'.
        """
        mock_ethtool.return_value = [
            'Channel parameters for eth0:',
            'Current hardware settings:',
            'RX:\t\tn/a',
            'TX:\t\tn/a',
            'Other:\t\tn/a',
            'Combined:\t2',
            'Pre-set maximums:',
            'RX:\t\tn/a',
            'TX:\t\tn/a',
            'Other:\t\tn/a',
            'Combined:\t16',
        ]
        result = self._parse('eth0')
        self.assertEqual(result.preset_maximums.combined, 16)
        self.assertEqual(result.current_hardware_settings.combined, 2)


class TestRxQueueIndex(unittest.TestCase):
    _fn = staticmethod(
        perftune.NetPerfTuner._NetPerfTuner__rx_queue_index)

    def test_rx_zero(self):
        """
        __rx_queue_index extracts 0 from an rx-0 queue path.
        """
        self.assertEqual(self._fn('/sys/class/net/eth0/queues/rx-0/rps_cpus'), 0)

    def test_rx_higher_index(self):
        """
        __rx_queue_index extracts 15 from an rx-15 queue path.
        """
        self.assertEqual(self._fn('/sys/class/net/eth0/queues/rx-15/rps_cpus'), 15)

    def test_rps_flow_cnt_path(self):
        """
        The function works for rps_flow_cnt paths as well as rps_cpus paths,
        returning the queue index from 'rx-<N>'.
        """
        self.assertEqual(self._fn('/sys/class/net/eth0/queues/rx-3/rps_flow_cnt'), 3)

    def test_tx_path_returns_maxsize(self):
        """
        A TX queue path (tx-0/xps_cpus) does not carry an RX queue index;
        __rx_queue_index returns sys.maxsize.
        """
        self.assertEqual(self._fn('/sys/class/net/eth0/queues/tx-0/xps_cpus'), sys.maxsize)

    def test_unrelated_path_returns_maxsize(self):
        """
        A path unrelated to RX queues (e.g. /proc/interrupts) returns
        sys.maxsize.
        """
        self.assertEqual(self._fn('/proc/interrupts'), sys.maxsize)

    def test_sorting_order(self):
        """
        Sorting paths by __rx_queue_index places them in numeric queue order
        (0, 1, 2, 10), not lexicographic order.
        """
        paths = [
            '/sys/class/net/eth0/queues/rx-2/rps_cpus',
            '/sys/class/net/eth0/queues/rx-0/rps_cpus',
            '/sys/class/net/eth0/queues/rx-10/rps_cpus',
            '/sys/class/net/eth0/queues/rx-1/rps_cpus',
        ]
        self.assertEqual(sorted(paths, key=self._fn), [
            '/sys/class/net/eth0/queues/rx-0/rps_cpus',
            '/sys/class/net/eth0/queues/rx-1/rps_cpus',
            '/sys/class/net/eth0/queues/rx-2/rps_cpus',
            '/sys/class/net/eth0/queues/rx-10/rps_cpus',
        ])

    def test_non_matching_paths_sort_last(self):
        """
        Non-matching paths (returning sys.maxsize) sort after all RX queue
        paths.
        """
        paths = [
            '/sys/class/net/eth0/queues/tx-0/xps_cpus',
            '/sys/class/net/eth0/queues/rx-0/rps_cpus',
        ]
        self.assertEqual(sorted(paths, key=self._fn)[0],
                         '/sys/class/net/eth0/queues/rx-0/rps_cpus')
