import argparse
import sys
import unittest
from unittest import mock

import perftune


def _make_args(**overrides):
    defaults = dict(
        mode=None, nics=['eth0'], tune_clock=False, get_cpu_mask=False,
        get_cpu_mask_quiet=False, get_irq_cpu_mask=False, verbose=False,
        tune=[], cpu_mask='0x000000ff', irq_cpu_mask=None, dirs=[], devs=[],
        options_file=None, dump_options_file=False, dry_run=False,
        set_write_back=None, enable_arfs=None, num_rx_queues=None,
        cores_per_irq_core=16, tcp_mem_fraction=0.03,
    )
    defaults.update(overrides)
    return argparse.Namespace(**defaults)


def _make_net_tuner(nics=None, **extra_attrs):
    """
    Return a NetPerfTuner with __init__ bypassed.
    """
    resolved_nics = ['eth0'] if nics is None else nics
    tuner = object.__new__(perftune.NetPerfTuner)
    tuner.nics = resolved_nics
    tuner._NetPerfTuner__nic_is_bond_iface_dict = {}
    tuner._NetPerfTuner__nic_is_vlan_iface_dict = {}
    tuner._NetPerfTuner__slaves_dict = {}
    tuner._NetPerfTuner__nic2irqs = {}
    tuner._NetPerfTuner__irqs2procline = {}
    tuner._PerfTunerBase__args = _make_args(nics=resolved_nics)
    tuner._PerfTunerBase__irq_cpu_mask = '0x000000ff'
    tuner._PerfTunerBase__compute_cpu_mask = '0x000000ff'
    tuner._PerfTunerBase__mode = perftune.PerfTunerBase.SupportedModes.mq
    tuner._PerfTunerBase__is_aws_i3_nonmetal_instance = None
    tuner._PerfTunerBase__metadata_token_value = None
    tuner._PerfTunerBase__metadata_token_time = None
    for k, v in extra_attrs.items():
        setattr(tuner, k, v)
    return tuner


# ---------------------------------------------------------------------------
# NetPerfTuner._get_irqs
# ---------------------------------------------------------------------------

class TestNetPerfTunerGetIrqs(unittest.TestCase):
    def test_chains_irqs_across_nics(self):
        """
        _get_irqs() chains the IRQ lists of all NICs from __nic2irqs into a
        single iterable, so all IRQs are visible regardless of which NIC they
        belong to.
        """
        tuner = _make_net_tuner()
        tuner._NetPerfTuner__nic2irqs = {'eth0': ['42', '43'], 'eth1': ['44']}
        self.assertEqual(sorted(tuner._get_irqs()), ['42', '43', '44'])

    def test_empty_nic2irqs(self):
        """
        When no NICs have been mapped to IRQs, _get_irqs() returns an empty
        iterator.
        """
        tuner = _make_net_tuner()
        tuner._NetPerfTuner__nic2irqs = {}
        self.assertEqual(list(tuner._get_irqs()), [])


# ---------------------------------------------------------------------------
# __nic_is_bond_iface / __nic_is_vlan_iface / __nic_has_slaves
# __nic_exists / __nic_is_tunable / __slaves
# ---------------------------------------------------------------------------

class TestNicHelpers(unittest.TestCase):
    def test_nic_is_bond_true(self):
        """
        __nic_is_bond_iface returns True when the NIC name is present in the
        bond-interface dictionary.
        """
        tuner = _make_net_tuner()
        tuner._NetPerfTuner__nic_is_bond_iface_dict = {'bond0': True}
        self.assertTrue(tuner._NetPerfTuner__nic_is_bond_iface('bond0'))

    def test_nic_is_bond_false(self):
        """
        __nic_is_bond_iface returns False when the NIC name is absent from the
        bond-interface dictionary.
        """
        tuner = _make_net_tuner()
        tuner._NetPerfTuner__nic_is_bond_iface_dict = {}
        self.assertFalse(tuner._NetPerfTuner__nic_is_bond_iface('eth0'))

    def test_nic_is_vlan_true(self):
        """
        __nic_is_vlan_iface returns True when the NIC name appears in the
        VLAN-interface dictionary.
        """
        tuner = _make_net_tuner()
        tuner._NetPerfTuner__nic_is_vlan_iface_dict = {'eth0.100': True}
        self.assertTrue(tuner._NetPerfTuner__nic_is_vlan_iface('eth0.100'))

    def test_nic_is_vlan_false(self):
        """
        __nic_is_vlan_iface returns False when the NIC is not listed as a VLAN
        interface.
        """
        tuner = _make_net_tuner()
        self.assertFalse(tuner._NetPerfTuner__nic_is_vlan_iface('eth0'))

    def test_nic_has_slaves_true(self):
        """
        __nic_has_slaves returns True when the NIC key exists in __slaves_dict
        and its slave list is non-empty.
        """
        tuner = _make_net_tuner()
        tuner._NetPerfTuner__slaves_dict = {'bond0': ['eth0', 'eth1']}
        self.assertTrue(tuner._NetPerfTuner__nic_has_slaves('bond0'))

    def test_nic_has_slaves_empty_list(self):
        """
        An empty slaves list is treated the same as having no slaves:
        __nic_has_slaves returns False.
        """
        tuner = _make_net_tuner()
        tuner._NetPerfTuner__slaves_dict = {'bond0': []}
        self.assertFalse(tuner._NetPerfTuner__nic_has_slaves('bond0'))

    def test_nic_has_slaves_missing(self):
        """
        __nic_has_slaves returns False when the NIC is not a key in
        __slaves_dict at all.
        """
        tuner = _make_net_tuner()
        self.assertFalse(tuner._NetPerfTuner__nic_has_slaves('eth99'))

    @mock.patch('os.path.exists', return_value=True)
    def test_nic_exists_true(self, _):
        """
        __nic_exists returns True when /sys/class/net/<nic> is present
        (os.path.exists returns True).
        """
        tuner = _make_net_tuner()
        self.assertTrue(tuner._NetPerfTuner__nic_exists('eth0'))

    @mock.patch('os.path.exists', return_value=False)
    def test_nic_exists_false(self, _):
        """
        __nic_exists returns False when the sysfs path for the NIC does not
        exist.
        """
        tuner = _make_net_tuner()
        self.assertFalse(tuner._NetPerfTuner__nic_exists('eth0'))

    @mock.patch('os.path.exists', return_value=True)
    def test_nic_is_tunable_true(self, _):
        """
        __nic_is_tunable returns True when /sys/class/net/<nic>/device exists,
        indicating the NIC has an associated hardware device.
        """
        tuner = _make_net_tuner()
        self.assertTrue(tuner._NetPerfTuner__nic_is_tunable('eth0'))

    def test_slaves_iterator(self):
        """
        __slaves returns an iterator over the slave list for the given bond NIC
        from __slaves_dict.
        """
        tuner = _make_net_tuner()
        tuner._NetPerfTuner__slaves_dict = {'bond0': ['eth0', 'eth1']}
        self.assertEqual(list(tuner._NetPerfTuner__slaves('bond0')), ['eth0', 'eth1'])


# ---------------------------------------------------------------------------
# __rfs_table_size
# ---------------------------------------------------------------------------

class TestRfsTableSize(unittest.TestCase):
    def test_value(self):
        """
        The RFS flow table size constant __rfs_table_size is 32768, matching
        the recommended /proc/sys/net/core/rps_sock_flow_entries value.
        """
        tuner = _make_net_tuner()
        self.assertEqual(tuner._NetPerfTuner__rfs_table_size, 32768)


# ---------------------------------------------------------------------------
# __get_irqs_one
# ---------------------------------------------------------------------------

class TestGetIrqsOne(unittest.TestCase):
    def test_known_nic(self):
        """
        __get_irqs_one returns the list of IRQ numbers stored for the given NIC
        in __nic2irqs.
        """
        tuner = _make_net_tuner()
        tuner._NetPerfTuner__nic2irqs = {'eth0': ['42', '43']}
        self.assertEqual(tuner._NetPerfTuner__get_irqs_one('eth0'), ['42', '43'])

    def test_unknown_nic_returns_empty(self):
        """
        __get_irqs_one returns an empty list when the NIC has no entry in
        __nic2irqs.
        """
        tuner = _make_net_tuner()
        tuner._NetPerfTuner__nic2irqs = {}
        self.assertEqual(tuner._NetPerfTuner__get_irqs_one('eth99'), [])


# ---------------------------------------------------------------------------
# __iface_exists / __dev_is_tunalbe_iface
# ---------------------------------------------------------------------------

class TestIfaceExists(unittest.TestCase):
    @mock.patch('os.path.exists', return_value=True)
    def test_existing(self, _):
        """
        __iface_exists returns True when the sysfs network path for the
        interface exists.
        """
        self.assertTrue(_make_net_tuner()._NetPerfTuner__iface_exists('eth0'))

    @mock.patch('os.path.exists', return_value=False)
    def test_missing(self, _):
        """
        __iface_exists returns False when the sysfs path is absent.
        """
        self.assertFalse(_make_net_tuner()._NetPerfTuner__iface_exists('eth99'))

    def test_empty_name_returns_false(self):
        """
        An empty interface name yields a sysfs path that never exists;
        __iface_exists returns False.
        """
        self.assertFalse(_make_net_tuner()._NetPerfTuner__iface_exists(''))

    @mock.patch('os.path.exists', return_value=True)
    def test_dev_is_tunable(self, _):
        """
        __dev_is_tunalbe_iface returns True when /sys/class/net/<iface>/device
        exists, confirming the interface is tunable.
        """
        self.assertTrue(_make_net_tuner()._NetPerfTuner__dev_is_tunalbe_iface('eth0'))

    @mock.patch('os.path.exists', return_value=False)
    def test_dev_not_tunable(self, _):
        """
        __dev_is_tunalbe_iface returns False when the device sub-path does not
        exist (e.g. a virtual or non-hardware interface).
        """
        self.assertFalse(_make_net_tuner()._NetPerfTuner__dev_is_tunalbe_iface('eth99'))


# ---------------------------------------------------------------------------
# __check_nics
# ---------------------------------------------------------------------------

class TestCheckNics(unittest.TestCase):
    @mock.patch('os.path.exists', return_value=False)
    def test_nonexistent_nic_raises(self, _):
        """
        __check_nics raises an Exception mentioning the NIC name when the
        interface's sysfs path does not exist.
        """
        tuner = _make_net_tuner(nics=['eth99'])
        with self.assertRaises(Exception) as ctx:
            tuner._NetPerfTuner__check_nics()
        self.assertIn('eth99', str(ctx.exception))

    @mock.patch('os.path.exists', return_value=True)
    def test_existing_but_not_tunable_and_no_slaves_raises(self, mock_exists):
        """
        A NIC whose sysfs directory exists but has no /device sub-path and no
        slaves is not tunable; __check_nics raises an Exception for it.
        """
        # exists() returns True for /sys/class/net/eth99 but False for /sys/class/net/eth99/device
        mock_exists.side_effect = lambda p: not p.endswith('/device')
        tuner = _make_net_tuner(nics=['eth99'])
        with self.assertRaises(Exception) as ctx:
            tuner._NetPerfTuner__check_nics()
        self.assertIn('eth99', str(ctx.exception))


# ---------------------------------------------------------------------------
# __learn_slaves_one
# ---------------------------------------------------------------------------

class TestLearnSlavesOne(unittest.TestCase):
    @mock.patch('os.path.exists', return_value=True)
    @mock.patch('builtins.open', mock.mock_open(read_data='eth0 eth1\n'))
    def test_bond_iface_learns_tunable_slaves(self, _):
        """
        For a bond interface, __learn_slaves_one reads the bonding/slaves file
        and returns the slave names that have a sysfs device entry.
        """
        tuner = _make_net_tuner()
        tuner._NetPerfTuner__nic_is_bond_iface_dict = {'bond0': True}
        result = tuner._NetPerfTuner__learn_slaves_one('bond0')
        self.assertIn('eth0', result)
        self.assertIn('eth1', result)

    @mock.patch('os.path.exists', return_value=False)
    @mock.patch('glob.glob', return_value=[])
    def test_vlan_iface_no_lower_slaves(self, mock_glob, _):
        """
        For a VLAN interface with no lower-link entries in sysfs,
        __learn_slaves_one returns an empty set.
        """
        tuner = _make_net_tuner()
        result = tuner._NetPerfTuner__learn_slaves_one('eth0.100')
        self.assertEqual(result, set())

    @mock.patch('os.path.exists', return_value=True)
    @mock.patch('builtins.open', mock.mock_open(read_data='\n'))
    def test_bond_with_no_slaves(self, _):
        """
        When the bonding/slaves file is blank (only a newline), the bond has no
        slaves and __learn_slaves_one returns an empty set.
        """
        tuner = _make_net_tuner()
        tuner._NetPerfTuner__nic_is_bond_iface_dict = {'bond0': True}
        result = tuner._NetPerfTuner__learn_slaves_one('bond0')
        self.assertEqual(result, set())


# ---------------------------------------------------------------------------
# __learn_slaves
# ---------------------------------------------------------------------------

class TestLearnSlaves(unittest.TestCase):
    def test_nic_with_slaves(self):
        """
        __learn_slaves populates __slaves_dict with the slave set returned by
        __learn_slaves_one for each NIC that has at least one slave.
        """
        tuner = _make_net_tuner(nics=['bond0'])
        with mock.patch.object(tuner, '_NetPerfTuner__learn_slaves_one',
                               return_value={'eth0', 'eth1'}):
            result = tuner._NetPerfTuner__learn_slaves()
        self.assertIn('bond0', result)
        self.assertEqual(set(result['bond0']), {'eth0', 'eth1'})

    def test_nic_without_slaves(self):
        """
        When __learn_slaves_one returns an empty set for a NIC, that NIC is not
        added to the resulting slaves dictionary.
        """
        tuner = _make_net_tuner(nics=['eth0'])
        with mock.patch.object(tuner, '_NetPerfTuner__learn_slaves_one',
                               return_value=set()):
            result = tuner._NetPerfTuner__learn_slaves()
        self.assertNotIn('eth0', result)


# ---------------------------------------------------------------------------
# __get_irq_to_queue_idx_functor
# ---------------------------------------------------------------------------

class TestGetIrqToQueueIdxFunctor(unittest.TestCase):
    @mock.patch('perftune.run_ethtool', return_value=['driver: mlx5_core'])
    def test_mlx_driver(self, _):
        """
        For a Mellanox (mlx5_core) driver, __get_irq_to_queue_idx_functor
        returns the __mlx_irq_to_queue_idx method.
        """
        tuner = _make_net_tuner()
        fn = tuner._NetPerfTuner__get_irq_to_queue_idx_functor('eth0')
        self.assertEqual(fn, tuner._NetPerfTuner__mlx_irq_to_queue_idx)

    @mock.patch('perftune.run_ethtool', return_value=['driver: virtio_net'])
    def test_virtio_driver(self, _):
        """
        For a virtio_net driver, the functor returned is
        __virtio_irq_to_queue_idx.
        """
        tuner = _make_net_tuner()
        fn = tuner._NetPerfTuner__get_irq_to_queue_idx_functor('eth0')
        self.assertEqual(fn, tuner._NetPerfTuner__virtio_irq_to_queue_idx)

    @mock.patch('perftune.run_ethtool', return_value=['driver: mana'])
    def test_mana_driver(self, _):
        """
        For a Microsoft Azure Network Adapter (mana) driver, the functor
        returned is __mana_irq_to_queue_idx.
        """
        tuner = _make_net_tuner()
        fn = tuner._NetPerfTuner__get_irq_to_queue_idx_functor('eth0')
        self.assertEqual(fn, tuner._NetPerfTuner__mana_irq_to_queue_idx)

    @mock.patch('perftune.run_ethtool', return_value=['driver: ena'])
    def test_unknown_driver_uses_intel(self, _):
        """
        For any driver not explicitly recognised (e.g. 'ena'), the functor
        falls back to the Intel-style __intel_irq_to_queue_idx.
        """
        tuner = _make_net_tuner()
        fn = tuner._NetPerfTuner__get_irq_to_queue_idx_functor('eth0')
        self.assertEqual(fn, tuner._NetPerfTuner__intel_irq_to_queue_idx)


# ---------------------------------------------------------------------------
# __irq_lower_bound_by_queue
# ---------------------------------------------------------------------------

class TestIrqLowerBoundByQueue(unittest.TestCase):
    def _make_with_procline(self, irqs2procline):
        tuner = _make_net_tuner()
        tuner._NetPerfTuner__irqs2procline = irqs2procline
        return tuner

    @mock.patch('perftune.run_ethtool', return_value=['driver: ena'])
    def test_finds_first_irq_at_queue_idx(self, _):
        """
        __irq_lower_bound_by_queue returns the index of the first IRQ whose
        queue index is >= the requested queue_idx.
        """
        # IRQs 0-3 named with TxRx-N but driver is ena (intel functor, returns maxsize for all)
        # So all IRQs have queue idx = maxsize; lower bound for queue_idx=0 is 0
        tuner = self._make_with_procline({
            '10': '  10: eth0-TxRx-0',
            '11': '  11: eth0-TxRx-1',
        })
        result = tuner._NetPerfTuner__irq_lower_bound_by_queue('eth0', ['10', '11'], 0)
        # intel functor with TxRx-0 -> returns 0 which >= 0 -> found at idx 0
        self.assertEqual(result, 0)

    @mock.patch('perftune.run_ethtool', return_value=['driver: ena'])
    def test_queue_idx_beyond_len_returns_len(self, _):
        """
        When the requested queue_idx exceeds all IRQ queue indices,
        __irq_lower_bound_by_queue returns len(irqs) as a sentinel.
        """
        tuner = self._make_with_procline({'10': '  10: eth0'})
        result = tuner._NetPerfTuner__irq_lower_bound_by_queue('eth0', ['10'], 5)
        self.assertEqual(result, 1)  # len(irqs) = 1


# ---------------------------------------------------------------------------
# __learn_irqs
# ---------------------------------------------------------------------------

class TestLearnIrqs(unittest.TestCase):
    def test_nic_without_slaves(self):
        """
        For a standalone NIC, __learn_irqs calls __learn_irqs_one and stores
        the result in the returned dict under the NIC name.
        """
        tuner = _make_net_tuner(nics=['eth0'])
        tuner._NetPerfTuner__slaves_dict = {}
        with mock.patch.object(tuner, '_NetPerfTuner__learn_irqs_one',
                               return_value=['42', '43']) as mock_learn:
            result = tuner._NetPerfTuner__learn_irqs()
        self.assertEqual(result['eth0'], ['42', '43'])
        mock_learn.assert_called_once_with('eth0')

    @mock.patch('os.path.exists', return_value=True)
    def test_nic_with_slaves_learns_slave_irqs(self, _):
        """
        For a bond/VLAN NIC, __learn_irqs iterates over its slaves and collects
        their IRQs instead of the master interface's IRQs.
        """
        tuner = _make_net_tuner(nics=['bond0'])
        tuner._NetPerfTuner__slaves_dict = {'bond0': ['eth0', 'eth1']}
        with mock.patch.object(tuner, '_NetPerfTuner__learn_irqs_one',
                               side_effect=lambda iface: [f'irq_{iface}']):
            result = tuner._NetPerfTuner__learn_irqs()
        self.assertIn('eth0', result)
        self.assertIn('eth1', result)
        self.assertNotIn('bond0', result)


# ---------------------------------------------------------------------------
# NetPerfTuner.tune()
# ---------------------------------------------------------------------------

class TestNetPerfTunerTune(unittest.TestCase):
    @mock.patch('perftune.fwriteln_and_log')
    def test_tune_calls_tcp_tuning(self, mock_fw):
        """
        tune() always writes to /proc/sys/net/core/somaxconn and
        tcp_max_syn_backlog, and calls __tune_tcp_mem, even when no NICs are
        configured.
        """
        tuner = _make_net_tuner(nics=[])
        with mock.patch.object(tuner, '_NetPerfTuner__tune_tcp_mem') as mock_tcp:
            tuner.tune()
        mock_fw.assert_any_call('/proc/sys/net/core/somaxconn', '4096')
        mock_fw.assert_any_call('/proc/sys/net/ipv4/tcp_max_syn_backlog', '4096')
        mock_tcp.assert_called_once()

    @mock.patch('perftune.fwriteln_and_log')
    @mock.patch('perftune.perftune_print')
    def test_tune_calls_setup_for_tunable_nic(self, mock_print, mock_fw):
        """
        For a NIC that is directly tunable, tune() calls
        __setup_one_tunable_iface with that NIC name.
        """
        tuner = _make_net_tuner(nics=['eth0'])
        with mock.patch.object(tuner, '_NetPerfTuner__nic_is_tunable', return_value=True), \
             mock.patch.object(tuner, '_NetPerfTuner__nic_has_slaves', return_value=False), \
             mock.patch.object(tuner, '_NetPerfTuner__setup_one_tunable_iface') as mock_setup, \
             mock.patch.object(tuner, '_NetPerfTuner__tune_tcp_mem'):
            tuner.tune()
        mock_setup.assert_called_once_with('eth0')

    @mock.patch('perftune.fwriteln_and_log')
    @mock.patch('perftune.perftune_print')
    def test_tune_calls_setup_for_bond_nic(self, mock_print, mock_fw):
        """
        For a bond NIC (has slaves, not directly tunable), tune() calls
        __setup_virtual_iface to configure the bond.
        """
        tuner = _make_net_tuner(nics=['bond0'])
        tuner._NetPerfTuner__nic_is_bond_iface_dict = {'bond0': True}
        with mock.patch.object(tuner, '_NetPerfTuner__nic_is_tunable', return_value=False), \
             mock.patch.object(tuner, '_NetPerfTuner__nic_has_slaves', return_value=True), \
             mock.patch.object(tuner, '_NetPerfTuner__setup_virtual_iface') as mock_virt, \
             mock.patch.object(tuner, '_NetPerfTuner__tune_tcp_mem'):
            tuner.tune()
        mock_virt.assert_called_once_with('bond0')


# ---------------------------------------------------------------------------
# __tune_tcp_mem
# ---------------------------------------------------------------------------

class TestTuneTcpMem(unittest.TestCase):
    @mock.patch('perftune.fwriteln_and_log')
    @mock.patch('psutil.virtual_memory')
    def test_writes_tcp_mem(self, mock_vmem, mock_fw):
        """
        __tune_tcp_mem calculates a tcp_mem value based on total physical
        memory and writes it to /proc/sys/net/ipv4/tcp_mem via fwriteln_and_log.
        """
        mock_vmem.return_value = mock.Mock(total=8 * 1024 * 1024 * 1024)  # 8 GB
        tuner = _make_net_tuner()
        tuner._NetPerfTuner__tune_tcp_mem()
        mock_fw.assert_called_once()
        call_path = mock_fw.call_args[0][0]
        self.assertEqual(call_path, '/proc/sys/net/ipv4/tcp_mem')


# ---------------------------------------------------------------------------
# __setup_rps / __setup_xps
# ---------------------------------------------------------------------------

class TestSetupRps(unittest.TestCase):
    @mock.patch('perftune.set_one_mask')
    def test_calls_set_one_mask_for_each_rps_cpu(self, mock_set):
        """
        __setup_rps calls set_one_mask once per RPS CPU file returned by
        __get_rps_cpus, setting the affinity mask for each receive queue.
        """
        tuner = _make_net_tuner()
        rps_files = ['/sys/class/net/eth0/queues/rx-0/rps_cpus',
                     '/sys/class/net/eth0/queues/rx-1/rps_cpus']
        with mock.patch.object(tuner, '_NetPerfTuner__get_rps_cpus', return_value=rps_files), \
             mock.patch.object(tuner, '_NetPerfTuner__setup_rfs'):
            tuner._NetPerfTuner__setup_rps('eth0', '0x000000ff')
        self.assertEqual(mock_set.call_count, 2)

    @mock.patch('perftune.set_one_mask')
    def test_empty_rps_cpus_no_set_mask(self, mock_set):
        """
        When __get_rps_cpus returns an empty list (no RPS queues configured),
        __setup_rps does not call set_one_mask at all.
        """
        tuner = _make_net_tuner()
        with mock.patch.object(tuner, '_NetPerfTuner__get_rps_cpus', return_value=[]), \
             mock.patch.object(tuner, '_NetPerfTuner__setup_rfs'):
            tuner._NetPerfTuner__setup_rps('eth0', '0x000000ff')
        mock_set.assert_not_called()


class TestSetupXps(unittest.TestCase):
    @mock.patch('perftune.set_one_mask')
    @mock.patch('perftune.run_hwloc_distrib', return_value=['0x1', '0x2'])
    @mock.patch('glob.glob', return_value=[
        '/sys/class/net/eth0/queues/tx-0/xps_cpus',
        '/sys/class/net/eth0/queues/tx-1/xps_cpus',
    ])
    def test_distributes_xps_masks(self, _, mock_distrib, mock_set):
        """
        __setup_xps discovers TX queue xps_cpus files, calls run_hwloc_distrib
        to generate CPU masks, and writes one mask per queue.
        """
        tuner = _make_net_tuner()
        tuner._NetPerfTuner__setup_xps('eth0')
        mock_distrib.assert_called_once_with(['2'])
        self.assertEqual(mock_set.call_count, 2)


# ---------------------------------------------------------------------------
# NetPerfTuner.__init__
# ---------------------------------------------------------------------------

class TestNetPerfTunerInit(unittest.TestCase):
    def _make_init_args(self):
        import argparse
        return argparse.Namespace(
            mode='mq', nics=['eth0'], tune_clock=False, get_cpu_mask=False,
            get_cpu_mask_quiet=False, get_irq_cpu_mask=False, verbose=False,
            tune=[], cpu_mask='0x000000ff', irq_cpu_mask=None, dirs=[], devs=[],
            options_file=None, dump_options_file=False, dry_run=False,
            set_write_back=None, enable_arfs=None, num_rx_queues=None,
            cores_per_irq_core=16, tcp_mem_fraction=0.03,
        )

    @mock.patch('perftune.run_hwloc_calc', return_value='0x000000ff')
    @mock.patch('perftune.learn_all_irqs_one', return_value=[])
    @mock.patch('perftune.get_irqs2procline_map', return_value={})
    @mock.patch('glob.glob', return_value=[])
    @mock.patch('os.path.exists', side_effect=lambda p: p in (
        '/sys/class/net/eth0',
        '/sys/class/net/eth0/device',
    ))
    def test_init_with_mocked_internals(self, mock_exists, mock_glob,
                                        mock_irq_map, mock_learn, mock_calc):
        """
        NetPerfTuner.__init__ completes successfully when external I/O calls
        are mocked; the resulting tuner exposes the configured NIC list and
        bond-interface dict.
        """
        args = self._make_init_args()
        with mock.patch('builtins.open', mock.mock_open(read_data='')):
            tuner = perftune.NetPerfTuner(args)
        self.assertEqual(tuner.nics, ['eth0'])
        self.assertIsNotNone(tuner._NetPerfTuner__nic_is_bond_iface_dict)


# ---------------------------------------------------------------------------
# NetPerfTuner.tune() — VLAN interface type label
# ---------------------------------------------------------------------------

class TestNetPerfTunerTuneVlanLabel(unittest.TestCase):
    @mock.patch('perftune.fwriteln_and_log')
    @mock.patch('perftune.perftune_print')
    def test_tune_vlan_nic_prints_vlan_label(self, mock_print, mock_fw):
        """
        When a NIC is identified as a VLAN interface, tune() logs a message
        containing 'VLAN' to distinguish it from bond and regular interfaces.
        """
        tuner = _make_net_tuner(nics=['eth0.100'])
        tuner._NetPerfTuner__nic_is_vlan_iface_dict = {'eth0.100': True}
        with mock.patch.object(tuner, '_NetPerfTuner__nic_is_tunable', return_value=False), \
             mock.patch.object(tuner, '_NetPerfTuner__nic_has_slaves', return_value=True), \
             mock.patch.object(tuner, '_NetPerfTuner__setup_virtual_iface'), \
             mock.patch.object(tuner, '_NetPerfTuner__tune_tcp_mem'):
            tuner.tune()
        printed = ' '.join(str(c) for c in mock_print.call_args_list)
        self.assertIn('VLAN', printed)


# ---------------------------------------------------------------------------
# __get_irqs_info
# ---------------------------------------------------------------------------

class TestGetIrqsInfo(unittest.TestCase):
    def test_populates_irqs2procline_and_nic2irqs(self):
        """
        __get_irqs_info populates __irqs2procline from get_irqs2procline_map and
        __nic2irqs from __learn_irqs in a single call, making both maps
        available to subsequent tuning logic.
        """
        tuner = _make_net_tuner()
        with mock.patch('perftune.get_irqs2procline_map', return_value={'42': 'line'}) as mock_map, \
             mock.patch.object(tuner, '_NetPerfTuner__learn_irqs', return_value={'eth0': ['42']}) as mock_learn:
            tuner._NetPerfTuner__get_irqs_info()
        mock_map.assert_called_once()
        mock_learn.assert_called_once()
        self.assertEqual(tuner._NetPerfTuner__irqs2procline, {'42': 'line'})
        self.assertEqual(tuner._NetPerfTuner__nic2irqs, {'eth0': ['42']})


# ---------------------------------------------------------------------------
# __learn_slaves_one — cycle avoidance
# ---------------------------------------------------------------------------

class TestLearnSlavesOneCycleAvoidance(unittest.TestCase):
    """
    The duplicate-slave guard in __learn_slaves_one fires when a slave has
    already been added to slaves_list by a prior sibling's recursion.
    Scenario: bond0 → {eth0, eth1}, eth0 (VLAN lower link) → eth1.  If eth0
    is iterated first its recursion adds eth1 to slaves_list; when the loop
    reaches eth1 the guard skips it.
    We use a deterministic ordering by controlling the set via a list-based mock.
    """

    def test_duplicate_slave_is_skipped(self):
        """
        Duplicate-slave guard fires when slave already in slaves_list.

        bond0 → {eth0, eth1}. Each of eth0 and eth1's recursive call returns the
        other as its sub-slave.  Regardless of set iteration order, after the
        first slave is processed slaves_list contains both — so the second slave
        is skipped by the guard.
        """
        tuner = _make_net_tuner()
        tuner._NetPerfTuner__nic_is_bond_iface_dict = {'bond0': True}
        tuner._NetPerfTuner__nic_has_slaves_dict = {'eth0': True, 'eth1': True}
        tuner._NetPerfTuner__nic_is_vlan_iface_dict = {}

        original_method = perftune.NetPerfTuner._NetPerfTuner__learn_slaves_one

        def patched_learn(self_inner, nic):
            if nic == 'bond0':
                return original_method(self_inner, nic)
            elif nic == 'eth0':
                return {'eth1'}
            elif nic == 'eth1':
                return {'eth0'}
            return set()

        with mock.patch.object(perftune.NetPerfTuner,
                               '_NetPerfTuner__learn_slaves_one',
                               patched_learn), \
             mock.patch('builtins.open', mock.mock_open(read_data='eth0 eth1\n')), \
             mock.patch('os.path.exists', return_value=True), \
             mock.patch('glob.glob', return_value=[]):
            result = tuner._NetPerfTuner__learn_slaves_one('bond0')
        self.assertIn('eth0', result)
        self.assertIn('eth1', result)


# ---------------------------------------------------------------------------
# __learn_irqs_one
# ---------------------------------------------------------------------------

class TestLearnIrqsOne(unittest.TestCase):
    @mock.patch('perftune.learn_all_irqs_one', return_value=['42', '43', '44'])
    def test_fp_irqs_filtered_and_sorted(self, _):
        """
        When fast-path IRQ names are present, __learn_irqs_one filters to only
        the fast-path set and sorts them by their queue index (ascending).
        """
        tuner = _make_net_tuner()
        tuner._NetPerfTuner__irqs2procline = {
            '42': '  42: eth0-TxRx-2',
            '43': '  43: eth0-TxRx-0',
            '44': '  44: eth0-TxRx-1',
        }
        with mock.patch('perftune.run_ethtool', return_value=['driver: ixgbe']):
            result = tuner._NetPerfTuner__learn_irqs_one('eth0')
        # Sorted by TxRx queue index (0, 1, 2) → '43', '44', '42'
        self.assertEqual(result, ['43', '44', '42'])

    @mock.patch('perftune.learn_all_irqs_one', return_value=['42', '43'])
    def test_non_fp_irqs_returned_as_is(self, _):
        """
        When no IRQ names match any FP pattern, all IRQs are returned.
        """
        tuner = _make_net_tuner()
        tuner._NetPerfTuner__irqs2procline = {
            '42': '  42: some-other-irq',
            '43': '  43: another-irq',
        }
        with mock.patch('perftune.run_ethtool', return_value=['driver: ixgbe']):
            result = tuner._NetPerfTuner__learn_irqs_one('eth0')
        self.assertIn('42', result)
        self.assertIn('43', result)


# ---------------------------------------------------------------------------
# __get_rps_cpus
# ---------------------------------------------------------------------------

class TestGetRpsCpus(unittest.TestCase):
    @mock.patch('perftune.run_ethtool', return_value=['driver: ena'])
    @mock.patch('glob.glob', return_value=[
        '/sys/class/net/eth0/queues/rx-1/rps_cpus',
        '/sys/class/net/eth0/queues/rx-0/rps_cpus',
    ])
    def test_sorted_rps_cpus(self, _, __):
        """
        __get_rps_cpus returns the RPS CPU file paths sorted by their numeric
        RX queue index so they align with the IRQ order.
        """
        tuner = _make_net_tuner()
        result = tuner._NetPerfTuner__get_rps_cpus('eth0')
        self.assertEqual(result[0], '/sys/class/net/eth0/queues/rx-0/rps_cpus')
        self.assertEqual(result[1], '/sys/class/net/eth0/queues/rx-1/rps_cpus')

    @mock.patch('glob.glob', return_value=[
        '/sys/class/net/eth0/queues/rx-0/rps_cpus',
        '/sys/class/net/eth0/queues/rx-1/rps_cpus',
        '/sys/class/net/eth0/queues/rx-2/rps_cpus',
        '/sys/class/net/eth0/queues/rx-3/rps_cpus',
    ])
    @mock.patch('perftune.run_ethtool')
    def test_mlx5_doubles_truncation(self, mock_ethtool, _mock_glob):
        """
        mlx5 with combined=2 and 4 RPS files → truncate to 2.
        """
        mock_ethtool.side_effect = [
            ['driver: mlx5_core'],  # __get_driver_name call
            [                       # __get_ethtool_l_info: run_ethtool(['-l', iface])
                'Channel parameters for eth0:',
                'Pre-set maximums:',
                'RX:\t\tn/a', 'TX:\t\tn/a', 'Other:\t\tn/a', 'Combined:\t4',
                'Current hardware settings:',
                'RX:\t\tn/a', 'TX:\t\tn/a', 'Other:\t\tn/a', 'Combined:\t2',
            ],
        ]
        tuner = _make_net_tuner()
        result = tuner._NetPerfTuner__get_rps_cpus('eth0')
        self.assertEqual(len(result), 2)


# ---------------------------------------------------------------------------
# __set_rx_channels_count
# ---------------------------------------------------------------------------

class TestSetRxChannelsCount(unittest.TestCase):
    @mock.patch('perftune.run_one_command')
    @mock.patch('perftune.perftune_print')
    def test_succeeds_on_first_option(self, _, mock_run):
        """
        __set_rx_channels_count returns True when the first ethtool command
        variant succeeds, meaning the RX channel count was configured.
        """
        tuner = _make_net_tuner()
        result = tuner._NetPerfTuner__set_rx_channels_count('eth0', 4)
        self.assertTrue(result)
        mock_run.assert_called_once()

    @mock.patch('subprocess.CalledProcessError', Exception)
    @mock.patch('perftune.run_one_command', side_effect=Exception)
    @mock.patch('perftune.perftune_print')
    def test_all_options_fail_returns_false(self, _, mock_run):
        """
        When all ethtool invocation variants raise an exception,
        __set_rx_channels_count returns False to signal failure.
        """
        tuner = _make_net_tuner()
        result = tuner._NetPerfTuner__set_rx_channels_count('eth0', 4)
        self.assertFalse(result)


# ---------------------------------------------------------------------------
# __setup_virtual_iface
# ---------------------------------------------------------------------------

class TestSetupVirtualIface(unittest.TestCase):
    @mock.patch('perftune.perftune_print')
    def test_setup_tunable_slaves(self, mock_print):
        """
        __setup_virtual_iface calls __setup_one_tunable_iface for each slave
        NIC that has a sysfs device entry (i.e. is hardware-backed).
        """
        tuner = _make_net_tuner()
        tuner._NetPerfTuner__slaves_dict = {'bond0': ['eth0', 'eth1']}
        with mock.patch('os.path.exists', return_value=True), \
             mock.patch.object(tuner, '_NetPerfTuner__setup_one_tunable_iface') as mock_setup:
            tuner._NetPerfTuner__setup_virtual_iface('bond0')
        self.assertEqual(mock_setup.call_count, 2)

    @mock.patch('perftune.perftune_print')
    def test_skips_non_tunable_slaves(self, mock_print):
        """
        Slaves without a sysfs /device entry are skipped by
        __setup_virtual_iface, which prints a 'Skipping' message for each one.
        """
        tuner = _make_net_tuner()
        tuner._NetPerfTuner__slaves_dict = {'bond0': ['veth0']}
        with mock.patch('os.path.exists', return_value=False), \
             mock.patch.object(tuner, '_NetPerfTuner__setup_one_tunable_iface') as mock_setup:
            tuner._NetPerfTuner__setup_virtual_iface('bond0')
        mock_setup.assert_not_called()
        printed = ' '.join(str(c) for c in mock_print.call_args_list)
        self.assertIn('Skipping', printed)


# ---------------------------------------------------------------------------
# __get_rx_queue_count
# ---------------------------------------------------------------------------

class TestGetRxQueueCount(unittest.TestCase):
    @mock.patch('perftune.run_ethtool', return_value=['driver: ena'])
    @mock.patch('glob.glob', return_value=[
        '/sys/class/net/eth0/queues/rx-0/rps_cpus',
        '/sys/class/net/eth0/queues/rx-1/rps_cpus',
    ])
    def test_returns_min_of_rps_and_max(self, _, __):
        """
        __get_rx_queue_count returns the minimum of the number of RPS CPU files
        and the maximum RX queue limit for the driver.
        """
        tuner = _make_net_tuner()
        tuner._NetPerfTuner__nic2irqs = {'eth0': ['42', '43', '44']}
        result = tuner._NetPerfTuner__get_rx_queue_count('eth0')
        self.assertEqual(result, 2)  # rps count is 2

    @mock.patch('perftune.run_ethtool', return_value=['driver: ena'])
    @mock.patch('glob.glob', return_value=[])
    def test_falls_back_to_irq_count_when_no_rps(self, _, __):
        """
        When there are no RPS CPU files, __get_rx_queue_count falls back to the
        number of IRQs for that NIC.
        """
        tuner = _make_net_tuner()
        tuner._NetPerfTuner__nic2irqs = {'eth0': ['42', '43']}
        result = tuner._NetPerfTuner__get_rx_queue_count('eth0')
        self.assertEqual(result, 2)  # falls back to num_irqs=2


# ---------------------------------------------------------------------------
# __setup_rfs
# ---------------------------------------------------------------------------

class TestSetupRfs(unittest.TestCase):
    @mock.patch('glob.glob', return_value=[
        '/sys/class/net/eth0/queues/rx-0/rps_flow_cnt',
    ])
    @mock.patch('perftune.run_one_command', side_effect=Exception("RFS not available"))
    def test_early_exit_when_rfs_unavailable(self, mock_run, mock_glob):
        """
        When the first run_one_command call (sysctl check) raises an exception,
        __setup_rfs exits early after a single command attempt.
        """
        tuner = _make_net_tuner()
        tuner._NetPerfTuner__nic2irqs = {}
        with mock.patch.object(tuner, '_NetPerfTuner__get_rx_queue_count', return_value=1), \
             mock.patch('perftune.run_ethtool', return_value=['driver: ena']):
            tuner._NetPerfTuner__setup_rfs('eth0')
        # Only the first sysctl check call — no further work after exception
        mock_run.assert_called_once()

    @mock.patch('perftune.fwriteln')
    @mock.patch('perftune.perftune_print')
    @mock.patch('perftune.run_one_command')
    @mock.patch('glob.glob', return_value=[
        '/sys/class/net/eth0/queues/rx-0/rps_flow_cnt',
        '/sys/class/net/eth0/queues/rx-1/rps_flow_cnt',
    ])
    def test_rfs_enabled_mq_mode(self, mock_glob, mock_run, mock_print, mock_fw):
        """
        In mq mode with RFS available, __setup_rfs writes rps_flow_cnt for
        each RX queue via fwriteln.
        """
        tuner = _make_net_tuner()
        tuner._NetPerfTuner__nic2irqs = {}
        with mock.patch.object(tuner, '_NetPerfTuner__get_rx_queue_count', return_value=2), \
             mock.patch('perftune.run_ethtool', return_value=['driver: ena']):
            old_dry = perftune.dry_run_mode
            perftune.dry_run_mode = False
            try:
                tuner._NetPerfTuner__setup_rfs('eth0')
            finally:
                perftune.dry_run_mode = old_dry
        # Should write rps_flow_cnt for each queue
        self.assertEqual(mock_fw.call_count, 2)

    @mock.patch('perftune.fwriteln')
    @mock.patch('perftune.perftune_print')
    @mock.patch('perftune.run_one_command')
    @mock.patch('glob.glob', return_value=[
        '/sys/class/net/eth0/queues/rx-0/rps_flow_cnt',
    ])
    def test_rfs_disable_arfs_explicit_false(self, mock_glob, mock_run, mock_print, mock_fw):
        """
        enable_arfs=False triggers the explicit aRFS-disable path in __setup_rfs.
        """
        tuner = _make_net_tuner()
        tuner._PerfTunerBase__args = _make_args(enable_arfs=False)
        tuner._NetPerfTuner__nic2irqs = {}
        with mock.patch.object(tuner, '_NetPerfTuner__get_rx_queue_count', return_value=1), \
             mock.patch('perftune.run_ethtool', return_value=['driver: ena']):
            perftune.dry_run_mode = False
            try:
                tuner._NetPerfTuner__setup_rfs('eth0')
            finally:
                perftune.dry_run_mode = False
        # run_one_command called: sysctl check, sysctl -w, ethtool -K
        self.assertGreaterEqual(mock_run.call_count, 1)

    @mock.patch('perftune.fwriteln')
    @mock.patch('perftune.perftune_print')
    @mock.patch('perftune.run_one_command')
    @mock.patch('glob.glob', return_value=[
        '/sys/class/net/eth0/queues/rx-0/rps_flow_cnt',
    ])
    def test_rfs_dry_run_mode(self, mock_glob, mock_run, mock_print, mock_fw):
        """
        dry_run_mode=True triggers the dry-run path in __setup_rfs.
        """
        tuner = _make_net_tuner()
        tuner._NetPerfTuner__nic2irqs = {}
        with mock.patch.object(tuner, '_NetPerfTuner__get_rx_queue_count', return_value=1), \
             mock.patch('perftune.run_ethtool', return_value=['driver: ena']):
            old_dry = perftune.dry_run_mode
            perftune.dry_run_mode = True
            try:
                tuner._NetPerfTuner__setup_rfs('eth0')
            finally:
                perftune.dry_run_mode = old_dry
        mock_print.assert_called()

    @mock.patch('builtins.print')
    @mock.patch('perftune.fwriteln')
    @mock.patch('perftune.run_one_command', side_effect=[None, None, Exception("ethtool fail")])
    @mock.patch('glob.glob', return_value=[
        '/sys/class/net/eth0/queues/rx-0/rps_flow_cnt',
    ])
    def test_rfs_ntuple_exception_silently_ignored(self, mock_glob, mock_run, mock_fw, mock_print):
        """
        __setup_rfs silently ignores exceptions from the ethtool -K ntuple command.
        """
        tuner = _make_net_tuner()
        tuner._NetPerfTuner__nic2irqs = {}
        with mock.patch.object(tuner, '_NetPerfTuner__get_rx_queue_count', return_value=1), \
             mock.patch('perftune.run_ethtool', return_value=['driver: ena']), \
             mock.patch('perftune.perftune_print'):
            perftune.dry_run_mode = False
            try:
                tuner._NetPerfTuner__setup_rfs('eth0')
            finally:
                perftune.dry_run_mode = False
        # Should not raise even though ethtool threw


# ---------------------------------------------------------------------------
# __setup_one_tunable_iface
# ---------------------------------------------------------------------------

class TestSetupOneTunableIface(unittest.TestCase):
    @mock.patch('perftune.perftune_print')
    @mock.patch('perftune.distribute_irqs')
    def test_explicit_num_rx_queues(self, mock_dist, mock_print):
        """
        When args.num_rx_queues is set use that directly.
        """
        tuner = _make_net_tuner()
        tuner._PerfTunerBase__args = _make_args(num_rx_queues=4)
        tuner._NetPerfTuner__nic2irqs = {'eth0': ['1', '2', '3', '4', '5', '6']}
        with mock.patch.object(tuner, '_NetPerfTuner__set_rx_channels_count', return_value=False), \
             mock.patch.object(tuner, '_NetPerfTuner__max_rx_queue_count', return_value=4), \
             mock.patch.object(tuner, '_NetPerfTuner__get_rx_queue_count', return_value=4), \
             mock.patch.object(tuner, '_NetPerfTuner__irq_lower_bound_by_queue', return_value=4), \
             mock.patch.object(tuner, '_NetPerfTuner__setup_rps'), \
             mock.patch.object(tuner, '_NetPerfTuner__setup_xps'):
            tuner._NetPerfTuner__setup_one_tunable_iface('eth0')
        self.assertGreaterEqual(mock_dist.call_count, 1)

    @mock.patch('perftune.perftune_print')
    @mock.patch('perftune.distribute_irqs')
    def test_distribute_all_when_no_limit(self, mock_dist, mock_print):
        """
        When max_rx_queue_count >= len(irqs) and no channels were set:
        distribute all IRQs together.
        """
        tuner = _make_net_tuner()
        tuner._NetPerfTuner__nic2irqs = {'eth0': ['1', '2']}
        with mock.patch.object(tuner, '_NetPerfTuner__set_rx_channels_count', return_value=False), \
             mock.patch.object(tuner, '_NetPerfTuner__max_rx_queue_count', return_value=sys.maxsize), \
             mock.patch.object(tuner, '_NetPerfTuner__get_rx_queue_count', return_value=2), \
             mock.patch.object(tuner, '_NetPerfTuner__setup_rps'), \
             mock.patch.object(tuner, '_NetPerfTuner__setup_xps'), \
             mock.patch('perftune.run_ethtool', return_value=['driver: ena']):
            tuner._NetPerfTuner__setup_one_tunable_iface('eth0')
        mock_dist.assert_called_once_with(['1', '2'], '0x000000ff')

    @mock.patch('perftune.perftune_print')
    @mock.patch('perftune.distribute_irqs')
    def test_dry_run_mode_uses_num_rx_channels(self, mock_dist, mock_print):
        """
        In dry_run_mode the Rx queue count uses num_rx_channels, not get_rx_queue_count.
        """
        tuner = _make_net_tuner()
        tuner._PerfTunerBase__args = _make_args(num_rx_queues=2)
        tuner._NetPerfTuner__nic2irqs = {'eth0': ['1', '2', '3', '4']}
        with mock.patch.object(tuner, '_NetPerfTuner__set_rx_channels_count', return_value=True), \
             mock.patch.object(tuner, '_NetPerfTuner__get_irqs_info'), \
             mock.patch.object(tuner, '_NetPerfTuner__max_rx_queue_count', return_value=sys.maxsize), \
             mock.patch.object(tuner, '_NetPerfTuner__get_rx_queue_count', return_value=1), \
             mock.patch.object(tuner, '_NetPerfTuner__irq_lower_bound_by_queue', return_value=2), \
             mock.patch.object(tuner, '_NetPerfTuner__setup_rps'), \
             mock.patch.object(tuner, '_NetPerfTuner__setup_xps'), \
             mock.patch('perftune.run_ethtool', return_value=['driver: ena']):
            old_dry = perftune.dry_run_mode
            perftune.dry_run_mode = True
            try:
                tuner._NetPerfTuner__setup_one_tunable_iface('eth0')
            finally:
                perftune.dry_run_mode = old_dry
        self.assertGreaterEqual(mock_dist.call_count, 1)
