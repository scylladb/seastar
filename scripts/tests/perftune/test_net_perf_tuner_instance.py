import sys
import unittest
from unittest import mock

import perftune


def _make_net_tuner(irqs2procline=None):
    """
    Return a NetPerfTuner instance with __init__ bypassed.
    """
    tuner = object.__new__(perftune.NetPerfTuner)
    tuner._NetPerfTuner__irqs2procline = irqs2procline or {}
    return tuner


# ---------------------------------------------------------------------------
# __get_bond_ifaces (static)
# ---------------------------------------------------------------------------

class TestGetBondIfaces(unittest.TestCase):
    _fn = staticmethod(
        perftune.NetPerfTuner._NetPerfTuner__get_bond_ifaces)

    @mock.patch('os.path.exists', return_value=False)
    def test_no_bonding_masters_file(self, _):
        """
        __get_bond_ifaces returns an empty dict when /sys/class/net/bonding_masters
        does not exist, meaning no bond interfaces are present.
        """
        self.assertEqual(self._fn(), {})

    @mock.patch('builtins.open', mock.mock_open(read_data='bond0 bond1\n'))
    @mock.patch('os.path.exists', return_value=True)
    def test_two_bonds(self, _):
        """
        When bonding_masters contains two names, __get_bond_ifaces returns a
        dict mapping both names to True.
        """
        result = self._fn()
        self.assertEqual(result, {'bond0': True, 'bond1': True})

    @mock.patch('builtins.open', mock.mock_open(read_data=''))
    @mock.patch('os.path.exists', return_value=True)
    def test_empty_file(self, _):
        """
        An empty bonding_masters file produces an empty dict: no bond
        interfaces are configured.
        """
        self.assertEqual(self._fn(), {})


# ---------------------------------------------------------------------------
# __get_vlan_ifaces (static)
# ---------------------------------------------------------------------------

class TestGetVlanIfaces(unittest.TestCase):
    _fn = staticmethod(
        perftune.NetPerfTuner._NetPerfTuner__get_vlan_ifaces)

    @mock.patch('glob.glob', return_value=[
        '/proc/net/vlan/eth0.100',
        '/proc/net/vlan/eth0.200',
        '/proc/net/vlan/config',
    ])
    def test_vlan_interfaces_excluded_config(self, _):
        """
        __get_vlan_ifaces builds a dict from /proc/net/vlan/* entries, omitting
        the 'config' file which is not an interface name.
        """
        result = self._fn()
        self.assertIn('eth0.100', result)
        self.assertIn('eth0.200', result)
        self.assertNotIn('config', result)

    @mock.patch('glob.glob', return_value=['/proc/net/vlan/config'])
    def test_only_config_returns_empty(self, _):
        """
        When the only file in /proc/net/vlan/ is 'config', __get_vlan_ifaces
        returns an empty dict.
        """
        self.assertEqual(self._fn(), {})

    @mock.patch('glob.glob', return_value=[])
    def test_no_vlans(self, _):
        """
        With no entries in /proc/net/vlan/, __get_vlan_ifaces returns an empty
        dict.
        """
        self.assertEqual(self._fn(), {})


# ---------------------------------------------------------------------------
# __intel_irq_to_queue_idx
# ---------------------------------------------------------------------------

class TestIntelIrqToQueueIdx(unittest.TestCase):
    def _make(self, irqs2procline):
        tuner = _make_net_tuner(irqs2procline)
        return perftune.NetPerfTuner._NetPerfTuner__intel_irq_to_queue_idx.__get__(tuner)

    def test_fast_path_irq(self):
        """
        For an Intel-style TxRx-N IRQ name, __intel_irq_to_queue_idx extracts
        and returns the queue index N.
        """
        fn = self._make({'42': '  42:  eth0-TxRx-3'})
        self.assertEqual(fn('42'), 3)

    def test_fdir_irq_sorted_last(self):
        """
        IRQs whose procline contains 'fdir' are classified as non-fast-path and
        return sys.maxsize so they sort after the fast-path IRQs.
        """
        fn = self._make({'42': '  42:  eth0:fdir-TxRx-0'})
        self.assertEqual(fn('42'), sys.maxsize)

    def test_unknown_irq(self):
        """
        An IRQ whose name doesn't match any Intel fast-path pattern returns
        sys.maxsize, placing it at the end of the sorted IRQ list.
        """
        fn = self._make({'42': '  42:  some-other-irq'})
        self.assertEqual(fn('42'), sys.maxsize)

    def test_queue_index_zero(self):
        """
        A TxRx-0 IRQ is a valid fast-path queue at index 0; the functor returns
        0 for it.
        """
        fn = self._make({'10': '  10:  eth0-TxRx-0'})
        self.assertEqual(fn('10'), 0)


# ---------------------------------------------------------------------------
# __mlx_irq_to_queue_idx
# ---------------------------------------------------------------------------

class TestMlxIrqToQueueIdx(unittest.TestCase):
    def _make(self, irqs2procline):
        tuner = _make_net_tuner(irqs2procline)
        return perftune.NetPerfTuner._NetPerfTuner__mlx_irq_to_queue_idx.__get__(tuner)

    def test_mlx5_comp(self):
        """
        For a Mellanox mlx5_comp<N>@... IRQ name, the functor extracts and
        returns the completion queue index N.
        """
        fn = self._make({'5': '  5:  mlx5_comp23@pci:0'})
        self.assertEqual(fn('5'), 23)

    def test_mlx4(self):
        """
        For an mlx4-<N>@... IRQ name, the functor returns the numeric queue
        index N.
        """
        fn = self._make({'6': '  6:  mlx4-6@pci:0'})
        self.assertEqual(fn('6'), 6)

    def test_unknown(self):
        """
        An IRQ whose procline doesn't match any Mellanox pattern returns
        sys.maxsize.
        """
        fn = self._make({'7': '  7:  eth0-TxRx-0'})
        self.assertEqual(fn('7'), sys.maxsize)

    def test_mlx5_index_zero(self):
        """
        mlx5_comp0 is a valid fast-path queue at index 0; the functor returns
        0.
        """
        fn = self._make({'1': '  1:  mlx5_comp0@pci:0'})
        self.assertEqual(fn('1'), 0)


# ---------------------------------------------------------------------------
# __mana_irq_to_queue_idx
# ---------------------------------------------------------------------------

class TestManaIrqToQueueIdx(unittest.TestCase):
    def _make(self, irqs2procline):
        tuner = _make_net_tuner(irqs2procline)
        return perftune.NetPerfTuner._NetPerfTuner__mana_irq_to_queue_idx.__get__(tuner)

    def test_mana_fp_queue(self):
        """
        A mana_q<N>@... IRQ represents a fast-path MANA queue; the functor
        returns the queue index N.
        """
        fn = self._make({'10': '  10:  mana_q7@pci:0'})
        self.assertEqual(fn('10'), 7)

    def test_mana_hwc_sorted_last(self):
        """
        The mana_hwc (Hardware Channel) IRQ is a control IRQ, not fast-path;
        the functor returns sys.maxsize to place it last.
        """
        fn = self._make({'11': '  11:  mana_hwc@pci:0'})
        self.assertEqual(fn('11'), sys.maxsize)

    def test_unknown(self):
        """
        An IRQ whose procline doesn't match any MANA pattern returns
        sys.maxsize.
        """
        fn = self._make({'12': '  12:  eth0-TxRx-0'})
        self.assertEqual(fn('12'), sys.maxsize)


# ---------------------------------------------------------------------------
# __virtio_irq_to_queue_idx
# ---------------------------------------------------------------------------

class TestVirtioIrqToQueueIdx(unittest.TestCase):
    def _make(self, irqs2procline):
        tuner = _make_net_tuner(irqs2procline)
        return perftune.NetPerfTuner._NetPerfTuner__virtio_irq_to_queue_idx.__get__(tuner)

    def test_virtio_input(self):
        """
        A virtio<N>-input.<Q> IRQ represents receive queue Q; the functor
        returns Q.
        """
        fn = self._make({'20': '  20:  virtio2-input.5'})
        self.assertEqual(fn('20'), 5)

    def test_virtio_output(self):
        """
        A virtio<N>-output.<Q> IRQ represents transmit queue Q; the functor
        returns Q.
        """
        fn = self._make({'21': '  21:  virtio0-output.2'})
        self.assertEqual(fn('21'), 2)

    def test_virtio_input_queue_zero(self):
        """
        Queue index 0 is valid for virtio input IRQs; the functor returns 0.
        """
        fn = self._make({'22': '  22:  virtio1-input.0'})
        self.assertEqual(fn('22'), 0)

    def test_unknown(self):
        """
        An IRQ whose procline doesn't match any virtio pattern returns
        sys.maxsize.
        """
        fn = self._make({'23': '  23:  eth0-TxRx-0'})
        self.assertEqual(fn('23'), sys.maxsize)


# ---------------------------------------------------------------------------
# __get_driver_name
# ---------------------------------------------------------------------------

class TestGetDriverName(unittest.TestCase):
    @mock.patch('perftune.run_ethtool', return_value=[
        'driver: mlx5_core', 'version: 5.0', 'firmware-version: 20.28'])
    def test_returns_driver(self, _):
        """
        __get_driver_name parses ethtool -i output and returns the driver name
        from the 'driver: <name>' line.
        """
        tuner = _make_net_tuner()
        self.assertEqual(tuner._NetPerfTuner__get_driver_name('eth0'), 'mlx5_core')

    @mock.patch('perftune.run_ethtool', return_value=['version: 5.0'])
    def test_no_driver_line_returns_empty(self, _):
        """
        When the ethtool output contains no 'driver:' line, __get_driver_name
        returns an empty string.
        """
        tuner = _make_net_tuner()
        self.assertEqual(tuner._NetPerfTuner__get_driver_name('eth0'), '')

    @mock.patch('perftune.run_ethtool', return_value=[
        'driver: ixgbe', 'driver: ixgbe-duplicate'])
    def test_multiple_driver_lines_raises(self, _):
        """
        When ethtool output contains more than one 'driver:' line,
        __get_driver_name raises an exception reporting 'More than one'.
        """
        tuner = _make_net_tuner()
        with self.assertRaises(Exception) as ctx:
            tuner._NetPerfTuner__get_driver_name('eth0')
        self.assertIn('More than one', str(ctx.exception))


# ---------------------------------------------------------------------------
# __max_rx_queue_count
# ---------------------------------------------------------------------------

class TestMaxRxQueueCount(unittest.TestCase):
    @mock.patch('perftune.run_ethtool', return_value=['driver: ixgbe'])
    def test_ixgbe_limit(self, _):
        """
        The ixgbe driver supports at most 16 RX queues; __max_rx_queue_count
        returns 16 for this driver.
        """
        self.assertEqual(_make_net_tuner()._NetPerfTuner__max_rx_queue_count('eth0'), 16)

    @mock.patch('perftune.run_ethtool', return_value=['driver: i40e'])
    def test_i40e_limit(self, _):
        """
        The i40e driver supports up to 64 RX queues; __max_rx_queue_count
        returns 64 for this driver.
        """
        self.assertEqual(_make_net_tuner()._NetPerfTuner__max_rx_queue_count('eth0'), 64)

    @mock.patch('perftune.run_ethtool', return_value=['driver: ixgbevf'])
    def test_ixgbevf_limit(self, _):
        """
        The ixgbevf (VF) driver is limited to 4 RX queues;
        __max_rx_queue_count returns 4.
        """
        self.assertEqual(_make_net_tuner()._NetPerfTuner__max_rx_queue_count('eth0'), 4)

    @mock.patch('perftune.run_ethtool', return_value=['driver: i40evf'])
    def test_i40evf_limit(self, _):
        """
        The i40evf (VF) driver supports up to 16 RX queues;
        __max_rx_queue_count returns 16.
        """
        self.assertEqual(_make_net_tuner()._NetPerfTuner__max_rx_queue_count('eth0'), 16)

    @mock.patch('perftune.run_ethtool', return_value=['driver: ena'])
    def test_unknown_driver_returns_maxsize(self, _):
        """
        For an unrecognised driver, __max_rx_queue_count returns sys.maxsize,
        imposing no artificial queue-count limit.
        """
        self.assertEqual(_make_net_tuner()._NetPerfTuner__max_rx_queue_count('eth0'), sys.maxsize)
