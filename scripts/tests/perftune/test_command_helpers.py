import io
import os
import subprocess
import sys
import unittest
from unittest import mock

import perftune


class TestPerftunePrint(unittest.TestCase):
    @mock.patch('builtins.print')
    def test_normal_mode(self, mock_print):
        """
        In normal (non-dry-run) mode, perftune_print delegates to print()
        with the message unchanged, without any prefix.
        """
        old = perftune.dry_run_mode
        perftune.dry_run_mode = False
        try:
            perftune.perftune_print("hello")
            mock_print.assert_called_once_with("hello")
        finally:
            perftune.dry_run_mode = old

    @mock.patch('builtins.print')
    def test_dry_run_mode(self, mock_print):
        """
        In dry-run mode, perftune_print prepends '# ' to the message before
        passing it to print(), so every output line looks like a shell comment.
        """
        old = perftune.dry_run_mode
        perftune.dry_run_mode = True
        try:
            perftune.perftune_print("hello")
            mock_print.assert_called_once_with("# hello")
        finally:
            perftune.dry_run_mode = old

    @mock.patch('builtins.print')
    def test_extra_args_forwarded(self, mock_print):
        """
        perftune_print forwards extra keyword arguments (e.g. end='') verbatim
        to the underlying print() call.
        """
        old = perftune.dry_run_mode
        perftune.dry_run_mode = False
        try:
            perftune.perftune_print("hello", end='')
            mock_print.assert_called_once_with("hello", end='')
        finally:
            perftune.dry_run_mode = old


class TestRunOneCommandInternal(unittest.TestCase):
    """
    Tests for the module-level __run_one_command.
    """

    _fn = staticmethod(getattr(perftune, '_PerfTunerBase__run_one_command', None) or
                        getattr(perftune, '__run_one_command', None) or
                        getattr(perftune, f'_{perftune.__name__}__run_one_command', None))

    def _get_fn(self):
        # Module-level double-underscore functions aren't mangled
        fn = getattr(perftune, '_TestRunOneCommandInternal__run_one_command', None)
        if fn is None:
            # Access via the module's __dict__
            fn = perftune.__dict__.get('__run_one_command')
        return fn

    @mock.patch('subprocess.Popen')
    def test_successful_command(self, mock_popen):
        """
        __run_one_command returns the decoded stdout string when the subprocess
        exits with code 0, and calls Popen with the expected arguments.
        """
        proc = mock.Mock()
        proc.communicate.return_value = (b'output\n', None)
        proc.returncode = 0
        mock_popen.return_value = proc

        fn = perftune.__dict__['__run_one_command']
        result = fn(['echo', 'hello'])
        self.assertEqual(result, 'output\n')
        mock_popen.assert_called_once_with(['echo', 'hello'], stdout=subprocess.PIPE, stderr=None)

    @mock.patch('subprocess.Popen')
    def test_failed_command_raises(self, mock_popen):
        """
        __run_one_command raises subprocess.CalledProcessError when the process
        exits with a non-zero return code and check=True (the default).
        """
        proc = mock.Mock()
        proc.communicate.return_value = (b'error output', b'stderr')
        proc.returncode = 1
        mock_popen.return_value = proc

        fn = perftune.__dict__['__run_one_command']
        with self.assertRaises(subprocess.CalledProcessError) as ctx:
            fn(['false'], check=True)
        self.assertEqual(ctx.exception.returncode, 1)

    @mock.patch('subprocess.Popen')
    def test_failed_command_no_check(self, mock_popen):
        """
        With check=False, __run_one_command returns the output even when the
        process exits with a non-zero return code, suppressing the exception.
        """
        proc = mock.Mock()
        proc.communicate.return_value = (b'output', None)
        proc.returncode = 1
        mock_popen.return_value = proc

        fn = perftune.__dict__['__run_one_command']
        result = fn(['false'], check=False)
        self.assertEqual(result, 'output')

    @mock.patch('subprocess.Popen')
    def test_stderr_forwarded(self, mock_popen):
        """
        The stderr argument is forwarded unchanged to subprocess.Popen so
        callers can redirect or suppress diagnostic output.
        """
        proc = mock.Mock()
        proc.communicate.return_value = (b'', None)
        proc.returncode = 0
        mock_popen.return_value = proc

        fn = perftune.__dict__['__run_one_command']
        fn(['cmd'], stderr=subprocess.DEVNULL)
        mock_popen.assert_called_once_with(['cmd'], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)


class TestRunOneCommand(unittest.TestCase):
    @mock.patch('builtins.print')
    def test_dry_run_prints_command(self, mock_print):
        """
        In dry-run mode, run_one_command prints the shell-quoted command string
        instead of executing it, so the output can be inspected or replayed.
        """
        old = perftune.dry_run_mode
        perftune.dry_run_mode = True
        try:
            perftune.run_one_command(['echo', 'hello world'])
            mock_print.assert_called_once_with("echo 'hello world'")
        finally:
            perftune.dry_run_mode = old

    @mock.patch('subprocess.Popen')
    def test_normal_mode_runs_command(self, mock_popen):
        """
        Outside dry-run mode, run_one_command actually invokes the subprocess
        via Popen rather than printing the command.
        """
        proc = mock.Mock()
        proc.communicate.return_value = (b'', None)
        proc.returncode = 0
        mock_popen.return_value = proc

        old = perftune.dry_run_mode
        perftune.dry_run_mode = False
        try:
            perftune.run_one_command(['echo', 'hello'])
        finally:
            perftune.dry_run_mode = old
        mock_popen.assert_called_once()


class TestRunReadOnlyCommand(unittest.TestCase):
    @mock.patch('subprocess.Popen')
    def test_returns_output(self, mock_popen):
        """
        run_read_only_command returns the raw stdout string produced by the
        subprocess, preserving newlines.
        """
        proc = mock.Mock()
        proc.communicate.return_value = (b'result\n', None)
        proc.returncode = 0
        mock_popen.return_value = proc

        result = perftune.run_read_only_command(['cat', '/proc/version'])
        self.assertEqual(result, 'result\n')


class TestRunHwlocDistrib(unittest.TestCase):
    @mock.patch('perftune.run_read_only_command')
    def test_returns_list_of_lines(self, mock_cmd):
        """
        run_hwloc_distrib prepends 'hwloc-distrib' to the given argument list,
        passes it to run_read_only_command, and splits the result into a list
        of non-empty lines (stripping the trailing newline).
        """
        mock_cmd.return_value = '0x00000001\n0x00000002\n'
        result = perftune.run_hwloc_distrib(['2', '--single'])
        self.assertEqual(result, ['0x00000001', '0x00000002'])
        mock_cmd.assert_called_once_with(['hwloc-distrib', '2', '--single'])


class TestRunHwlocCalc(unittest.TestCase):
    @mock.patch('perftune.run_read_only_command')
    def test_returns_stripped_string(self, mock_cmd):
        """
        run_hwloc_calc prepends 'hwloc-calc' to the argument list, calls
        run_read_only_command, and strips the trailing newline from the result.
        """
        mock_cmd.return_value = '0x000000ff\n'
        result = perftune.run_hwloc_calc(['all'])
        self.assertEqual(result, '0x000000ff')
        mock_cmd.assert_called_once_with(['hwloc-calc', 'all'])


class TestRunEthtool(unittest.TestCase):
    @mock.patch('perftune.run_read_only_command')
    def test_returns_list_of_lines(self, mock_cmd):
        """
        run_ethtool prepends 'ethtool' to the argument list, calls
        run_read_only_command, and splits the result into a list of lines.
        """
        mock_cmd.return_value = 'line1\nline2\n'
        result = perftune.run_ethtool(['-l', 'eth0'])
        self.assertEqual(result, ['line1', 'line2'])
        mock_cmd.assert_called_once_with(['ethtool', '-l', 'eth0'])


class TestFwriteln(unittest.TestCase):
    @mock.patch('builtins.print')
    def test_dry_run_prints(self, mock_print):
        """
        In dry-run mode, fwriteln does not open any file; instead it prints an
        'echo value > path' shell command that could be replayed.
        """
        old = perftune.dry_run_mode
        perftune.dry_run_mode = True
        try:
            perftune.fwriteln('/tmp/test', 'value', 'log msg')
            mock_print.assert_called_once_with("echo value > /tmp/test")
        finally:
            perftune.dry_run_mode = old

    @mock.patch('builtins.print')
    @mock.patch('builtins.open', mock.mock_open())
    def test_normal_mode_writes(self, mock_print):
        """
        Outside dry-run mode, fwriteln opens the target file for writing and
        prints the log_message to confirm the write.
        """
        old = perftune.dry_run_mode
        perftune.dry_run_mode = False
        try:
            perftune.fwriteln('/tmp/test', 'value', 'log msg')
            mock_print.assert_called_once_with('log msg')
        finally:
            perftune.dry_run_mode = old

    @mock.patch('builtins.print')
    @mock.patch('builtins.open', side_effect=PermissionError("denied"))
    def test_error_logged(self, mock_open, mock_print):
        """
        When the file open fails and log_errors=True, fwriteln prints a
        'failed to write' diagnostic message rather than raising an exception.
        """
        old = perftune.dry_run_mode
        perftune.dry_run_mode = False
        try:
            perftune.fwriteln('/tmp/test', 'value', 'log msg', log_errors=True)
            self.assertTrue(any('failed to write' in str(c) for c in mock_print.call_args_list))
        finally:
            perftune.dry_run_mode = old

    @mock.patch('builtins.print')
    @mock.patch('builtins.open', side_effect=PermissionError("denied"))
    def test_error_suppressed(self, mock_open, mock_print):
        """
        When log_errors=False, fwriteln silently swallows the I/O exception
        without printing any error diagnostic.
        """
        old = perftune.dry_run_mode
        perftune.dry_run_mode = False
        try:
            perftune.fwriteln('/tmp/test', 'value', 'log msg', log_errors=False)
            for call in mock_print.call_args_list:
                self.assertNotIn('failed to write', str(call))
        finally:
            perftune.dry_run_mode = old


class TestReadlines(unittest.TestCase):
    @mock.patch('builtins.open', mock.mock_open(read_data='line1\nline2\n'))
    def test_returns_lines(self):
        """
        readlines opens the given path, reads all lines and returns them as a
        list, preserving the trailing newline on each line.
        """
        result = perftune.readlines('/tmp/test')
        self.assertEqual(result, ['line1\n', 'line2\n'])

    @mock.patch('builtins.print')
    @mock.patch('builtins.open', side_effect=FileNotFoundError("not found"))
    def test_error_returns_empty(self, mock_open, mock_print):
        """
        When the file does not exist, readlines catches the exception, prints
        a diagnostic, and returns an empty list instead of propagating.
        """
        result = perftune.readlines('/nonexistent')
        self.assertEqual(result, [])
        self.assertTrue(mock_print.called)


class TestFwritelnAndLog(unittest.TestCase):
    @mock.patch('perftune.fwriteln')
    def test_delegates_to_fwriteln(self, mock_fw):
        """
        fwriteln_and_log constructs a 'Writing <value> to <path>' log message
        and delegates to fwriteln with that message and log_errors=True.
        """
        perftune.fwriteln_and_log('/tmp/f', 'val')
        mock_fw.assert_called_once_with('/tmp/f', 'val', log_message="Writing 'val' to /tmp/f", log_errors=True)

    @mock.patch('perftune.fwriteln')
    def test_log_errors_forwarded(self, mock_fw):
        """
        fwriteln_and_log passes the log_errors argument through to fwriteln
        unchanged, allowing callers to suppress error output.
        """
        perftune.fwriteln_and_log('/tmp/f', 'val', log_errors=False)
        mock_fw.assert_called_once_with('/tmp/f', 'val', log_message="Writing 'val' to /tmp/f", log_errors=False)


class TestSetOneMask(unittest.TestCase):
    @mock.patch('perftune.fwriteln')
    @mock.patch('os.path.exists', return_value=True)
    def test_strips_0x_prefix(self, mock_exists, mock_fw):
        """
        set_one_mask strips the leading '0x' prefix from the mask string before
        writing it to the smp_affinity file.
        """
        perftune.set_one_mask('/proc/irq/42/smp_affinity', '0xff')
        call_args = mock_fw.call_args
        self.assertEqual(call_args[0][1], 'ff')

    @mock.patch('perftune.fwriteln')
    @mock.patch('os.path.exists', return_value=True)
    def test_double_commas_filled(self, mock_exists, mock_fw):
        """
        set_one_mask replaces every pair of consecutive commas (',,') with
        ',0,' so that zero-valued mask components are written explicitly.
        """
        perftune.set_one_mask('/proc/irq/42/smp_affinity', '0xff,,0xff')
        call_args = mock_fw.call_args
        self.assertEqual(call_args[0][1], 'ff,0,ff')

    @mock.patch('os.path.exists', return_value=False)
    def test_missing_file_raises(self, mock_exists):
        """
        set_one_mask raises an Exception with a descriptive message when the
        target smp_affinity file does not exist.
        """
        with self.assertRaises(Exception) as ctx:
            perftune.set_one_mask('/nonexistent', '0xff')
        self.assertIn("doesn't exist", str(ctx.exception))

    @mock.patch('perftune.fwriteln')
    @mock.patch('os.path.exists', return_value=True)
    def test_triple_commas_filled(self, mock_exists, mock_fw):
        """
        Multiple consecutive commas are resolved iteratively: ',,,' becomes
        ',0,0,' after two passes of the double-comma substitution.
        """
        perftune.set_one_mask('/proc/irq/42/smp_affinity', '0xff,,,0xff')
        call_args = mock_fw.call_args
        self.assertEqual(call_args[0][1], 'ff,0,0,ff')


class TestDistributeIrqs(unittest.TestCase):
    @mock.patch('perftune.set_one_mask')
    @mock.patch('perftune.run_hwloc_distrib')
    def test_empty_irqs_noop(self, mock_distrib, mock_set):
        """
        distribute_irqs is a no-op when the IRQ list is empty: neither
        run_hwloc_distrib nor set_one_mask are called.
        """
        perftune.distribute_irqs([], '0xff')
        mock_distrib.assert_not_called()
        mock_set.assert_not_called()

    @mock.patch('perftune.set_one_mask')
    @mock.patch('perftune.run_hwloc_distrib', return_value=['0x1', '0x2'])
    def test_distributes_to_each_irq(self, mock_distrib, mock_set):
        """
        distribute_irqs calls run_hwloc_distrib with the IRQ count and CPU mask,
        then calls set_one_mask once per IRQ with the corresponding mask entry.
        """
        perftune.distribute_irqs(['42', '43'], '0xff')
        mock_distrib.assert_called_once_with(['2', '--single', '--restrict', '0xff'])
        self.assertEqual(mock_set.call_count, 2)
        mock_set.assert_any_call('/proc/irq/42/smp_affinity', '0x1', log_errors=True)
        mock_set.assert_any_call('/proc/irq/43/smp_affinity', '0x2', log_errors=True)

    @mock.patch('perftune.set_one_mask')
    @mock.patch('perftune.run_hwloc_distrib', return_value=['0x1'])
    def test_log_errors_forwarded(self, mock_distrib, mock_set):
        """
        The log_errors flag is forwarded from distribute_irqs to each
        set_one_mask call so callers can control error verbosity.
        """
        perftune.distribute_irqs(['42'], '0xff', log_errors=False)
        mock_set.assert_called_once_with('/proc/irq/42/smp_affinity', '0x1', log_errors=False)


class TestIsProcessRunning(unittest.TestCase):
    @mock.patch('perftune.run_read_only_command')
    def test_running_process(self, mock_cmd):
        """
        is_process_running returns True when ps output contains a non-defunct
        line for the named process.
        """
        mock_cmd.return_value = '  123 pts/0    00:00:00 irqbalance\n'
        self.assertTrue(perftune.is_process_running('irqbalance'))

    @mock.patch('perftune.run_read_only_command')
    def test_no_process(self, mock_cmd):
        """
        is_process_running returns False when ps produces no output,
        indicating the process is not currently running.
        """
        mock_cmd.return_value = ''
        self.assertFalse(perftune.is_process_running('irqbalance'))

    @mock.patch('perftune.run_read_only_command')
    def test_defunct_process_ignored(self, mock_cmd):
        """
        A zombie (<defunct>) process line is filtered out; is_process_running
        returns False because no live instance was found.
        """
        mock_cmd.return_value = '  123 pts/0    00:00:00 irqbalance <defunct>\n'
        self.assertFalse(perftune.is_process_running('irqbalance'))


class TestLearnIrqsFromProcInterrupts(unittest.TestCase):
    def test_matches_pattern(self):
        """
        learn_irqs_from_proc_interrupts returns only the IRQ numbers whose
        /proc/interrupts line matches the given regex pattern.
        """
        irq2procline = {
            '42': '  42:  1000  IR-PCI-MSI-edge  eth0-TxRx-0',
            '43': '  43:  2000  IR-PCI-MSI-edge  eth0-TxRx-1',
            '44': '  44:  500   IR-PCI-MSI-edge  timer',
        }
        result = perftune.learn_irqs_from_proc_interrupts('TxRx', irq2procline)
        self.assertEqual(sorted(result), ['42', '43'])

    def test_no_match(self):
        """
        When the pattern matches no line in the irq2procline map, the function
        returns an empty list.
        """
        irq2procline = {'42': '  42:  1000  timer'}
        result = perftune.learn_irqs_from_proc_interrupts('eth0', irq2procline)
        self.assertEqual(result, [])

    def test_empty_map(self):
        """
        With an empty irq2procline map the function returns an empty list
        regardless of the pattern.
        """
        result = perftune.learn_irqs_from_proc_interrupts('any', {})
        self.assertEqual(result, [])


class TestCheckSysfsNumaTopology(unittest.TestCase):
    @mock.patch('os.path.exists')
    @mock.patch('os.path.isdir', return_value=True)
    def test_package_cpus_available(self, mock_isdir, mock_exists):
        """
        check_sysfs_numa_topology_is_valid returns True when the
        package_cpus topology file exists under /sys/devices/system/cpu/cpu0.
        """
        mock_exists.side_effect = lambda p: 'package_cpus' in p
        self.assertTrue(perftune.check_sysfs_numa_topology_is_valid())

    @mock.patch('os.path.exists')
    @mock.patch('os.path.isdir', return_value=True)
    def test_core_cpus_available(self, mock_isdir, mock_exists):
        """
        check_sysfs_numa_topology_is_valid returns True when core_cpus exists
        (the alternative to package_cpus on some kernel versions).
        """
        mock_exists.side_effect = lambda p: 'core_cpus' in p and 'package' not in p
        self.assertTrue(perftune.check_sysfs_numa_topology_is_valid())

    @mock.patch('os.path.exists')
    @mock.patch('os.path.isdir', return_value=True)
    def test_core_siblings_available(self, mock_isdir, mock_exists):
        """
        check_sysfs_numa_topology_is_valid returns True when the older
        core_siblings sysfs file is present.
        """
        def exists_side_effect(p):
            if 'core_siblings' in p:
                return True
            return False
        mock_exists.side_effect = exists_side_effect
        self.assertTrue(perftune.check_sysfs_numa_topology_is_valid())

    @mock.patch('os.path.exists')
    @mock.patch('os.path.isdir', return_value=True)
    def test_thread_siblings_available(self, mock_isdir, mock_exists):
        """
        check_sysfs_numa_topology_is_valid returns True when the older
        thread_siblings sysfs file is present.
        """
        def exists_side_effect(p):
            if 'thread_siblings' in p:
                return True
            return False
        mock_exists.side_effect = exists_side_effect
        self.assertTrue(perftune.check_sysfs_numa_topology_is_valid())

    @mock.patch('os.path.exists', return_value=False)
    @mock.patch('os.path.isdir', return_value=True)
    def test_nothing_available(self, mock_isdir, mock_exists):
        """
        When none of the known topology files exist, the function returns False,
        signalling that NUMA topology cannot be determined.
        """
        self.assertFalse(perftune.check_sysfs_numa_topology_is_valid())

    @mock.patch('os.path.isdir', return_value=False)
    def test_no_cpu_dir(self, mock_isdir):
        """
        When /sys/devices/system/cpu does not exist, the function returns False
        without checking for any topology sub-files.
        """
        self.assertFalse(perftune.check_sysfs_numa_topology_is_valid())


class TestLearnAllIrqsOne(unittest.TestCase):
    @mock.patch('os.listdir', return_value=['42', '43', '44'])
    @mock.patch('os.path.exists', return_value=True)
    def test_msi_irqs(self, mock_exists, mock_listdir):
        """
        When the msi_irqs directory exists under the device, learn_all_irqs_one
        returns its contents (the MSI IRQ numbers) via os.listdir.
        """
        result = perftune.learn_all_irqs_one('/sys/dev/0000:00:1f.2', {}, 'blkif')
        self.assertEqual(result, ['42', '43', '44'])

    @mock.patch('builtins.open', mock.mock_open(read_data='55\n'))
    @mock.patch('os.path.exists')
    def test_irq_file(self, mock_exists):
        """
        When there is no msi_irqs dir but an 'irq' file exists, the function
        reads it and returns the stripped IRQ numbers it contains.
        """
        def exists_side_effect(p):
            if 'msi_irqs' in p:
                return False
            if p.endswith('/irq'):
                return True
            return False
        mock_exists.side_effect = exists_side_effect

        result = perftune.learn_all_irqs_one('/sys/dev/test', {}, 'blkif')
        self.assertEqual(result, ['55'])

    @mock.patch('os.walk', return_value=[])
    @mock.patch('builtins.open', mock.mock_open(read_data='virtio0\n'))
    @mock.patch('os.path.exists', return_value=False)
    def test_virtio_modalias(self, mock_exists, mock_walk):
        """
        A virtio modalias causes the function to walk the driver directory
        looking for virtio IRQs; an empty os.walk result yields an empty list.
        """
        result = perftune.learn_all_irqs_one('/sys/dev/test', {}, 'eth0')
        self.assertEqual(result, [])

    @mock.patch('builtins.open', mock.mock_open(read_data='xen:vbd\n'))
    @mock.patch('os.path.exists', return_value=False)
    def test_xen_modalias(self, mock_exists):
        """
        A 'xen:' modalias causes the function to search /proc/interrupts for
        the xen_dev_name pattern and return only the matching IRQ numbers.
        """
        irq2procline = {
            '42': '  42:  1000  xen-blkif',
            '43': '  43:  2000  timer',
        }
        result = perftune.learn_all_irqs_one('/sys/dev/test', irq2procline, 'blkif')
        self.assertEqual(result, ['42'])

    @mock.patch('builtins.open', mock.mock_open(read_data='acpi:PNP0501:\n'))
    @mock.patch('os.path.exists', return_value=False)
    def test_unknown_modalias_returns_empty(self, mock_exists):
        """
        An unrecognised modalias prefix (e.g. 'acpi:') results in an empty
        list because no IRQ discovery strategy applies.
        """
        result = perftune.learn_all_irqs_one('/sys/dev/test', {}, 'blkif')
        self.assertEqual(result, [])


class TestGetIrqs2ProclineMap(unittest.TestCase):
    @mock.patch('builtins.open', mock.mock_open(read_data='  42: 1000 PCI-MSI eth0\n  43: 2000 PCI-MSI eth1\n'))
    def test_parses_proc_interrupts(self):
        """
        get_irqs2procline_map reads /proc/interrupts and builds a dict keyed by
        IRQ number (stripped of whitespace), with the full line as the value.
        """
        result = perftune.get_irqs2procline_map()
        self.assertIn('42', result)
        self.assertIn('43', result)
        self.assertIn('eth0', result['42'])


class TestRestartIrqbalance(unittest.TestCase):
    @mock.patch('perftune.is_process_running')
    def test_empty_banned_irqs_returns_early(self, mock_running):
        """
        When the banned-IRQs list is empty, restart_irqbalance returns
        immediately without ever checking whether irqbalance is running.
        """
        perftune.restart_irqbalance([])
        mock_running.assert_not_called()

    @mock.patch('perftune.perftune_print')
    @mock.patch('perftune.is_process_running', return_value=False)
    def test_not_running_returns_early(self, mock_running, mock_print):
        """
        When irqbalance is not running, restart_irqbalance prints a notice and
        returns without writing any config file or running any command.
        """
        perftune.restart_irqbalance(['42'])
        mock_print.assert_called_once_with("irqbalance is not running")

    @mock.patch('perftune.run_one_command')
    @mock.patch('builtins.open', mock.mock_open(read_data=''))
    @mock.patch('shutil.copyfile')
    @mock.patch('os.path.exists')
    @mock.patch('perftune.is_process_running', return_value=True)
    def test_systemd_style(self, mock_running, mock_exists, mock_copy, mock_run):
        """
        On a systemd-style system (the irqbalance.service unit file exists),
        restart_irqbalance uses 'systemctl try-restart irqbalance' to restart.
        """
        old = perftune.dry_run_mode
        perftune.dry_run_mode = False
        try:
            def exists_side_effect(p):
                if p == '/lib/systemd/system/irqbalance.service':
                    return True
                if p == '/etc/default/irqbalance':
                    return True
                if p.endswith('.scylla.orig'):
                    return False
                return False
            mock_exists.side_effect = exists_side_effect

            perftune.restart_irqbalance(['42', '43'])
            mock_run.assert_called_once_with(['systemctl', 'try-restart', 'irqbalance'])
        finally:
            perftune.dry_run_mode = old

    @mock.patch('perftune.run_one_command')
    @mock.patch('builtins.open', mock.mock_open(read_data=''))
    @mock.patch('shutil.copyfile')
    @mock.patch('os.path.exists')
    @mock.patch('perftune.is_process_running', return_value=True)
    def test_initd_style(self, mock_running, mock_exists, mock_copy, mock_run):
        """
        On an init.d-style system (no systemd unit file), restart_irqbalance
        falls back to calling '/etc/init.d/irqbalance restart'.
        """
        old = perftune.dry_run_mode
        perftune.dry_run_mode = False
        try:
            def exists_side_effect(p):
                if 'systemd' in p:
                    return False
                if p == '/etc/default/irqbalance':
                    return True
                if p.endswith('.scylla.orig'):
                    return False
                return False
            mock_exists.side_effect = exists_side_effect

            perftune.restart_irqbalance(['42'])
            mock_run.assert_called_once_with(['/etc/init.d/irqbalance', 'restart'])
        finally:
            perftune.dry_run_mode = old

    @mock.patch('perftune.perftune_print')
    @mock.patch('os.path.exists')
    @mock.patch('perftune.is_process_running', return_value=True)
    def test_unknown_config_warns(self, mock_running, mock_exists, mock_print):
        """
        When none of the known config file paths exist, restart_irqbalance
        prints an 'Unknown system configuration' warning and returns without
        modifying anything.
        """
        mock_exists.return_value = False

        perftune.restart_irqbalance(['42'])
        calls = [str(c) for c in mock_print.call_args_list]
        self.assertTrue(any('Unknown system configuration' in c for c in calls))

    @mock.patch('perftune.run_one_command')
    @mock.patch('builtins.open', mock.mock_open(read_data=''))
    @mock.patch('shutil.copyfile')
    @mock.patch('os.path.exists')
    @mock.patch('perftune.is_process_running', return_value=True)
    def test_orig_file_already_exists(self, mock_running, mock_exists, mock_copy, mock_run):
        """
        If the .scylla.orig backup already exists, restart_irqbalance skips
        the backup step (shutil.copyfile is not called again).
        """
        old = perftune.dry_run_mode
        perftune.dry_run_mode = False
        try:
            def exists_side_effect(p):
                if 'systemd' in p:
                    return False
                if p == '/etc/default/irqbalance':
                    return True
                if p.endswith('.scylla.orig'):
                    return True
                return False
            mock_exists.side_effect = exists_side_effect

            perftune.restart_irqbalance(['42'])
            mock_copy.assert_not_called()
        finally:
            perftune.dry_run_mode = old

    @mock.patch('perftune.run_one_command')
    @mock.patch('builtins.open')
    @mock.patch('shutil.copyfile')
    @mock.patch('os.path.exists')
    @mock.patch('perftune.is_process_running', return_value=True)
    def test_existing_options_line(self, mock_running, mock_exists, mock_copy, mock_open_f, mock_run):
        """
        When the config file already contains an OPTIONS line,
        restart_irqbalance appends --banirq entries to that existing line
        rather than creating a new OPTIONS entry.
        """
        old = perftune.dry_run_mode
        perftune.dry_run_mode = False
        try:
            def exists_side_effect(p):
                if 'systemd' in p:
                    return False
                if p == '/etc/default/irqbalance':
                    return True
                if p.endswith('.scylla.orig'):
                    return False
                return False
            mock_exists.side_effect = exists_side_effect

            read_mock = mock.mock_open(read_data='OPTIONS="--hintpolicy=exact"\n')
            write_mock = mock.mock_open()

            def open_side_effect(fname, mode='r', *a, **kw):
                if mode == 'r':
                    return read_mock(fname, mode, *a, **kw)
                return write_mock(fname, mode, *a, **kw)

            mock_open_f.side_effect = open_side_effect
            perftune.restart_irqbalance(['42'])
        finally:
            perftune.dry_run_mode = old

    @mock.patch('builtins.print')
    @mock.patch('builtins.open', mock.mock_open(read_data='OPTIONS=""\n'))
    @mock.patch('os.path.exists')
    @mock.patch('perftune.is_process_running', return_value=True)
    def test_dry_run_mode(self, mock_running, mock_exists, mock_print):
        """
        In dry-run mode, restart_irqbalance prints the sed/echo commands that
        would modify the config file, including the --banirq argument, without
        actually writing anything.
        """
        old = perftune.dry_run_mode
        perftune.dry_run_mode = True
        try:
            def exists_side_effect(p):
                if 'systemd' in p:
                    return False
                if p == '/etc/default/irqbalance':
                    return True
                return False
            mock_exists.side_effect = exists_side_effect

            perftune.restart_irqbalance(['42'])
            printed = ' '.join(str(c) for c in mock_print.call_args_list)
            self.assertIn('banirq=42', printed)
        finally:
            perftune.dry_run_mode = old

    @mock.patch('builtins.open')
    @mock.patch('shutil.copyfile')
    @mock.patch('os.path.exists')
    @mock.patch('perftune.is_process_running', return_value=True)
    def test_multiple_options_lines_raises(self, mock_running, mock_exists, mock_copy, mock_open_f):
        """
        When the config file contains more than one OPTIONS line,
        restart_irqbalance raises an Exception reporting 'more than one lines'.
        """
        old = perftune.dry_run_mode
        perftune.dry_run_mode = False
        try:
            def exists_side_effect(p):
                if 'systemd' in p:
                    return False
                if p == '/etc/default/irqbalance':
                    return True
                if p.endswith('.scylla.orig'):
                    return False
                return False
            mock_exists.side_effect = exists_side_effect

            mock_open_f.return_value = mock.mock_open(
                read_data='OPTIONS="a"\nOPTIONS="b"\n')()

            with self.assertRaises(Exception) as ctx:
                perftune.restart_irqbalance(['42'])
            self.assertIn('more than one lines', str(ctx.exception))
        finally:
            perftune.dry_run_mode = old

    @mock.patch('perftune.run_one_command')
    @mock.patch('builtins.open')
    @mock.patch('shutil.copyfile')
    @mock.patch('os.path.exists')
    @mock.patch('perftune.is_process_running', return_value=True)
    def test_sysconfig_fallback(self, mock_running, mock_exists, mock_copy, mock_open_f, mock_run):
        """
        When /etc/default/irqbalance is absent but /etc/sysconfig/irqbalance
        exists, restart_irqbalance uses that file and restarts via init.d
        (because no systemd unit file is present).
        """
        old = perftune.dry_run_mode
        perftune.dry_run_mode = False
        try:
            def exists_side_effect(p):
                if 'systemd' in p:
                    return False
                if p == '/etc/default/irqbalance':
                    return False
                if p == '/etc/sysconfig/irqbalance':
                    return True
                if p.endswith('.scylla.orig'):
                    return False
                return False
            mock_exists.side_effect = exists_side_effect

            read_mock = mock.mock_open(read_data='')
            write_mock = mock.mock_open()

            def open_side_effect(fname, mode='r', *a, **kw):
                if mode == 'r':
                    return read_mock(fname, mode, *a, **kw)
                return write_mock(fname, mode, *a, **kw)

            mock_open_f.side_effect = open_side_effect
            perftune.restart_irqbalance(['42'])
            mock_run.assert_called_once_with(['/etc/init.d/irqbalance', 'restart'])
        finally:
            perftune.dry_run_mode = old

    @mock.patch('perftune.run_one_command')
    @mock.patch('builtins.open')
    @mock.patch('shutil.copyfile')
    @mock.patch('os.path.exists')
    @mock.patch('perftune.is_process_running', return_value=True)
    def test_confd_fallback_systemd(self, mock_running, mock_exists, mock_copy, mock_open_f, mock_run):
        """
        When the only available config is /etc/conf.d/irqbalance and
        /proc/1/comm reports 'systemd', restart_irqbalance detects systemd as
        the init system and restarts via 'systemctl try-restart irqbalance'.
        """
        old = perftune.dry_run_mode
        perftune.dry_run_mode = False
        try:
            def exists_side_effect(p):
                if 'systemd' in p:
                    return False
                if p == '/etc/default/irqbalance':
                    return False
                if p == '/etc/sysconfig/irqbalance':
                    return False
                if p == '/etc/conf.d/irqbalance':
                    return True
                if p.endswith('.scylla.orig'):
                    return False
                return False
            mock_exists.side_effect = exists_side_effect

            comm_mock = mock.mock_open(read_data='systemd\n')
            config_read = mock.mock_open(read_data='')
            config_write = mock.mock_open()

            def open_side_effect(fname, mode='r', *a, **kw):
                if fname == '/proc/1/comm':
                    return comm_mock(fname, mode, *a, **kw)
                if mode == 'r':
                    return config_read(fname, mode, *a, **kw)
                return config_write(fname, mode, *a, **kw)

            mock_open_f.side_effect = open_side_effect
            perftune.restart_irqbalance(['42'])
            mock_run.assert_called_once_with(['systemctl', 'try-restart', 'irqbalance'])
        finally:
            perftune.dry_run_mode = old


class TestRestartIrqbalanceNonOptionsLines(unittest.TestCase):
    """
    restart_irqbalance preserves non-OPTIONS config lines when rewriting the config file.
    """

    @mock.patch('perftune.run_one_command')
    @mock.patch('shutil.copyfile')
    @mock.patch('os.path.exists')
    @mock.patch('perftune.is_process_running', return_value=True)
    def test_non_options_lines_are_preserved(self, mock_running, mock_exists, mock_copy, mock_run):
        """
        When rewriting the config file, restart_irqbalance copies all
        non-OPTIONS lines (e.g. comments) verbatim to the new file, preserving
        the rest of the configuration.
        """
        old = perftune.dry_run_mode
        perftune.dry_run_mode = False
        try:
            def exists_side_effect(p):
                if 'systemd' in p:
                    return False
                if p == '/etc/default/irqbalance':
                    return True
                if p.endswith('.scylla.orig'):
                    return False
                return False
            mock_exists.side_effect = exists_side_effect

            config_with_comment = '# This is a comment\nOPTIONS=""\n'
            read_mock = mock.mock_open(read_data=config_with_comment)
            write_mock = mock.mock_open()

            def open_side_effect(fname, mode='r', *a, **kw):
                if mode == 'r':
                    return read_mock(fname, mode, *a, **kw)
                return write_mock(fname, mode, *a, **kw)

            with mock.patch('builtins.open', side_effect=open_side_effect):
                perftune.restart_irqbalance(['42'])

            # The write mock should have been called with the comment line
            write_handle = write_mock()
            written_calls = [str(c) for c in write_handle.write.call_args_list]
            self.assertTrue(any('# This is a comment' in c for c in written_calls))
        finally:
            perftune.dry_run_mode = old
