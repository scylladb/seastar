import argparse
import io
import unittest
from unittest import mock

import perftune


def _make_default_args(**overrides):
    """
    Create a minimal argparse.Namespace matching perftune's expected attributes.
    """
    defaults = dict(
        mode=None,
        nics=[],
        tune_clock=False,
        get_cpu_mask=False,
        get_cpu_mask_quiet=False,
        get_irq_cpu_mask=False,
        verbose=False,
        tune=[],
        cpu_mask=None,
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


class TestParseOptionsFile(unittest.TestCase):
    def test_no_options_file_noop(self):
        """
        When args.options_file is None, parse_options_file returns immediately
        without opening any file or modifying the args namespace.
        """
        args = _make_default_args()
        perftune.parse_options_file(args)
        self.assertIsNone(args.options_file)

    @mock.patch('builtins.open', mock.mock_open(read_data=''))
    @mock.patch('yaml.safe_load', return_value=None)
    def test_empty_yaml_noop(self, mock_yaml):
        """
        An options file whose YAML content is None (empty file) does not modify
        any args attribute.
        """
        args = _make_default_args(options_file='/tmp/test.yaml')
        perftune.parse_options_file(args)

    @mock.patch('builtins.open', mock.mock_open(read_data='mode: mq\n'))
    @mock.patch('yaml.safe_load', return_value={'mode': 'mq'})
    def test_mode_from_yaml(self, mock_yaml):
        """
        A YAML key 'mode: mq' in the options file is loaded into args.mode
        when the existing args.mode is None.
        """
        args = _make_default_args(options_file='/tmp/test.yaml')
        perftune.parse_options_file(args)
        self.assertEqual(args.mode, 'mq')

    @mock.patch('builtins.open', mock.mock_open(read_data=''))
    @mock.patch('yaml.safe_load', return_value={'mode': 'mq'})
    def test_mode_not_overridden_if_already_set(self, mock_yaml):
        """
        A mode value already set on the command line (args.mode='sq') is not
        overwritten by the options file value.
        """
        args = _make_default_args(options_file='/tmp/test.yaml', mode='sq')
        perftune.parse_options_file(args)
        self.assertEqual(args.mode, 'sq')

    @mock.patch('builtins.open', mock.mock_open(read_data=''))
    @mock.patch('yaml.safe_load', return_value={'mode': 'invalid_mode'})
    def test_invalid_mode_raises(self, mock_yaml):
        """
        An unrecognised mode string in the options file causes parse_options_file
        to raise an exception containing "Bad 'mode'".
        """
        args = _make_default_args(options_file='/tmp/test.yaml')
        with self.assertRaises(Exception) as ctx:
            perftune.parse_options_file(args)
        self.assertIn("Bad 'mode'", str(ctx.exception))

    @mock.patch('builtins.open', mock.mock_open(read_data=''))
    @mock.patch('yaml.safe_load', return_value={'nic': ['eth0', 'eth1']})
    def test_nics_from_yaml_list(self, mock_yaml):
        """
        A YAML 'nic' key with a list value appends all listed interface names
        to args.nics.
        """
        args = _make_default_args(options_file='/tmp/test.yaml')
        perftune.parse_options_file(args)
        self.assertIn('eth0', args.nics)
        self.assertIn('eth1', args.nics)

    @mock.patch('builtins.open', mock.mock_open(read_data=''))
    @mock.patch('yaml.safe_load', return_value={'nic': 'eth0'})
    def test_nic_string_converted_to_list(self, mock_yaml):
        """
        A YAML 'nic' key with a plain string value is wrapped in a list before
        being appended to args.nics.
        """
        args = _make_default_args(options_file='/tmp/test.yaml')
        perftune.parse_options_file(args)
        self.assertIn('eth0', args.nics)

    @mock.patch('builtins.open', mock.mock_open(read_data=''))
    @mock.patch('yaml.safe_load', return_value={'tune_clock': True})
    def test_tune_clock(self, mock_yaml):
        """
        A 'tune_clock: true' key in the options file sets args.tune_clock to
        True.
        """
        args = _make_default_args(options_file='/tmp/test.yaml')
        perftune.parse_options_file(args)
        self.assertTrue(args.tune_clock)

    @mock.patch('builtins.open', mock.mock_open(read_data=''))
    @mock.patch('yaml.safe_load', return_value={'tune': ['net', 'disks']})
    def test_tune_modes(self, mock_yaml):
        """
        A 'tune' list in the options file sets args.tune to those mode names.
        """
        args = _make_default_args(options_file='/tmp/test.yaml')
        perftune.parse_options_file(args)
        self.assertIn('net', args.tune)
        self.assertIn('disks', args.tune)

    @mock.patch('builtins.open', mock.mock_open(read_data=''))
    @mock.patch('yaml.safe_load', return_value={'tune': ['invalid']})
    def test_invalid_tune_raises(self, mock_yaml):
        """
        An unrecognised entry in the 'tune' list raises an exception containing
        "Bad 'tune'".
        """
        args = _make_default_args(options_file='/tmp/test.yaml')
        with self.assertRaises(Exception) as ctx:
            perftune.parse_options_file(args)
        self.assertIn("Bad 'tune'", str(ctx.exception))

    @mock.patch('builtins.open', mock.mock_open(read_data=''))
    @mock.patch('yaml.safe_load', return_value={'cpu_mask': '0x000000ff'})
    def test_cpu_mask(self, mock_yaml):
        """
        A 'cpu_mask' key in the options file is stored in args.cpu_mask when
        no mask was set on the command line.
        """
        args = _make_default_args(options_file='/tmp/test.yaml')
        perftune.parse_options_file(args)
        self.assertEqual(args.cpu_mask, '0x000000ff')

    @mock.patch('builtins.open', mock.mock_open(read_data=''))
    @mock.patch('yaml.safe_load', return_value={'cpu_mask': '0x000000ff'})
    def test_cpu_mask_not_overridden(self, mock_yaml):
        """
        An existing command-line cpu_mask is not replaced by the options file
        value.
        """
        args = _make_default_args(options_file='/tmp/test.yaml', cpu_mask='0x0000000f')
        perftune.parse_options_file(args)
        self.assertEqual(args.cpu_mask, '0x0000000f')

    @mock.patch('builtins.open', mock.mock_open(read_data=''))
    @mock.patch('yaml.safe_load', return_value={'irq_cpu_mask': '0x00000001'})
    def test_irq_cpu_mask(self, mock_yaml):
        """
        An 'irq_cpu_mask' key in the options file is stored in
        args.irq_cpu_mask.
        """
        args = _make_default_args(options_file='/tmp/test.yaml')
        perftune.parse_options_file(args)
        self.assertEqual(args.irq_cpu_mask, '0x00000001')

    @mock.patch('builtins.open', mock.mock_open(read_data=''))
    @mock.patch('yaml.safe_load', return_value={'dir': ['/data', '/log']})
    def test_dirs(self, mock_yaml):
        """
        A 'dir' list in the options file is appended to args.dirs.
        """
        args = _make_default_args(options_file='/tmp/test.yaml')
        perftune.parse_options_file(args)
        self.assertIn('/data', args.dirs)
        self.assertIn('/log', args.dirs)

    @mock.patch('builtins.open', mock.mock_open(read_data=''))
    @mock.patch('yaml.safe_load', return_value={'dev': ['sda1', 'nvme0n1']})
    def test_devs(self, mock_yaml):
        """
        A 'dev' list in the options file is appended to args.devs.
        """
        args = _make_default_args(options_file='/tmp/test.yaml')
        perftune.parse_options_file(args)
        self.assertIn('sda1', args.devs)
        self.assertIn('nvme0n1', args.devs)

    @mock.patch('builtins.open', mock.mock_open(read_data=''))
    @mock.patch('yaml.safe_load', return_value={'write_back_cache': True})
    def test_write_back_cache(self, mock_yaml):
        """
        'write_back_cache: true' in the options file sets args.set_write_back
        to 1 (truthy integer).
        """
        args = _make_default_args(options_file='/tmp/test.yaml')
        perftune.parse_options_file(args)
        self.assertEqual(args.set_write_back, 1)

    @mock.patch('builtins.open', mock.mock_open(read_data=''))
    @mock.patch('yaml.safe_load', return_value={'arfs': False})
    def test_arfs(self, mock_yaml):
        """
        'arfs: false' in the options file sets args.enable_arfs to 0 (falsy
        integer).
        """
        args = _make_default_args(options_file='/tmp/test.yaml')
        perftune.parse_options_file(args)
        self.assertEqual(args.enable_arfs, 0)

    @mock.patch('builtins.open', mock.mock_open(read_data=''))
    @mock.patch('yaml.safe_load', return_value={'num_rx_queues': 4})
    def test_num_rx_queues(self, mock_yaml):
        """
        A 'num_rx_queues' key in the options file sets args.num_rx_queues to
        the specified integer value.
        """
        args = _make_default_args(options_file='/tmp/test.yaml')
        perftune.parse_options_file(args)
        self.assertEqual(args.num_rx_queues, 4)

    @mock.patch('builtins.open', mock.mock_open(read_data=''))
    @mock.patch('yaml.safe_load', return_value={'irq_core_auto_detection_ratio': 8})
    def test_irq_core_auto_detection_ratio(self, mock_yaml):
        """
        'irq_core_auto_detection_ratio' in the options file sets
        args.cores_per_irq_core to the given integer.
        """
        args = _make_default_args(options_file='/tmp/test.yaml')
        perftune.parse_options_file(args)
        self.assertEqual(args.cores_per_irq_core, 8)


class TestDumpConfig(unittest.TestCase):
    @mock.patch('perftune.perftune_print')
    def test_minimal_config(self, mock_print):
        """
        dump_config with a minimal set of args (tune, nics, cores_per_irq_core)
        prints a YAML string that includes irq_core_auto_detection_ratio.
        """
        args = _make_default_args(tune=['net'], nics=['eth0'], cores_per_irq_core=16)
        perftune.dump_config(args)
        mock_print.assert_called_once()
        output = mock_print.call_args[0][0]
        self.assertIn('irq_core_auto_detection_ratio: 16', output)

    @mock.patch('perftune.perftune_print')
    @mock.patch('perftune.PerfTunerBase.irqs_cpu_mask_for_mode', return_value='0x00000001')
    def test_mode_dumps_irq_mask(self, mock_irq_mask, mock_print):
        """
        When a mode is specified, dump_config includes irq_cpu_mask (computed
        via irqs_cpu_mask_for_mode) in the output.
        """
        args = _make_default_args(mode='sq', cpu_mask='0x000000ff', nics=['eth0'],
                                  cores_per_irq_core=16)
        perftune.dump_config(args)
        output = mock_print.call_args[0][0]
        self.assertIn('irq_cpu_mask', output)

    @mock.patch('perftune.perftune_print')
    def test_all_options(self, mock_print):
        """
        dump_config with all options set produces YAML containing cpu_mask,
        irq_cpu_mask, tune_clock, nic, num_rx_queues, write_back_cache, arfs,
        dir, and dev keys.
        """
        args = _make_default_args(
            tune=['net', 'disks'],
            nics=['eth0', 'eth1'],
            tune_clock=True,
            cpu_mask='0x000000ff',
            irq_cpu_mask='0x00000001',
            dirs=['/data'],
            devs=['sda1'],
            set_write_back=True,
            enable_arfs=True,
            num_rx_queues=8,
            cores_per_irq_core=16,
        )
        perftune.dump_config(args)
        output = mock_print.call_args[0][0]
        self.assertIn('cpu_mask', output)
        self.assertIn('irq_cpu_mask', output)
        self.assertIn('tune_clock', output)
        self.assertIn('nic', output)
        self.assertIn('num_rx_queues', output)
        self.assertIn('write_back_cache', output)
        self.assertIn('arfs', output)
        self.assertIn('dir', output)
        self.assertIn('dev', output)

    @mock.patch('perftune.perftune_print')
    def test_empty_nics_not_in_output(self, mock_print):
        """
        When args.nics is empty, dump_config does not include a 'nic:' key in
        the output.
        """
        args = _make_default_args(cores_per_irq_core=16)
        perftune.dump_config(args)
        output = mock_print.call_args[0][0]
        self.assertNotIn('nic:', output)
