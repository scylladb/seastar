import unittest
from unittest import mock

import perftune


class TestComputeCpuMaskForMode(unittest.TestCase):
    @mock.patch('perftune.run_hwloc_calc')
    def test_sq_mode(self, mock_calc):
        """
        In sq mode, compute_cpu_mask_for_mode calls hwloc-calc with
        [cpu_mask, '~PU:0'] to exclude logical CPU 0 from the compute set.
        """
        mock_calc.return_value = '0x000000fe'
        result = perftune.PerfTunerBase.compute_cpu_mask_for_mode(
            perftune.PerfTunerBase.SupportedModes.sq, '0x000000ff')
        mock_calc.assert_called_once_with(['0x000000ff', '~PU:0'])
        self.assertEqual(result, '0x000000fe')

    @mock.patch('perftune.run_hwloc_calc')
    def test_sq_split_mode(self, mock_calc):
        """
        In sq_split mode, compute_cpu_mask_for_mode calls hwloc-calc with
        [cpu_mask, '~core:0'] to exclude the entire core 0 (including HT
        siblings) from the compute set.
        """
        mock_calc.return_value = '0x000000fc'
        result = perftune.PerfTunerBase.compute_cpu_mask_for_mode(
            perftune.PerfTunerBase.SupportedModes.sq_split, '0x000000ff')
        mock_calc.assert_called_once_with(['0x000000ff', '~core:0'])
        self.assertEqual(result, '0x000000fc')

    def test_mq_mode(self):
        """
        In mq mode all CPUs are used for compute, so compute_cpu_mask_for_mode
        returns cpu_mask unchanged without calling hwloc-calc.
        """
        result = perftune.PerfTunerBase.compute_cpu_mask_for_mode(
            perftune.PerfTunerBase.SupportedModes.mq, '0x000000ff')
        self.assertEqual(result, '0x000000ff')

    def test_no_irq_restrictions_mode(self):
        """
        In no_irq_restrictions mode the compute mask equals cpu_mask, returned
        without any hwloc-calc call.
        """
        result = perftune.PerfTunerBase.compute_cpu_mask_for_mode(
            perftune.PerfTunerBase.SupportedModes.no_irq_restrictions, '0x000000ff')
        self.assertEqual(result, '0x000000ff')

    @mock.patch('perftune.run_hwloc_calc', return_value='0x0')
    def test_zero_mask_raises(self, mock_calc):
        """
        When the hwloc-calc result is '0x0', compute_cpu_mask_for_mode raises
        CPUMaskIsZeroException because there would be no compute CPUs.
        """
        with self.assertRaises(perftune.PerfTunerBase.CPUMaskIsZeroException):
            perftune.PerfTunerBase.compute_cpu_mask_for_mode(
                perftune.PerfTunerBase.SupportedModes.sq, '0x00000001')

    def test_unsupported_mode_raises(self):
        """
        An integer value not in SupportedModes raises ValueError or Exception
        from compute_cpu_mask_for_mode.
        """
        with self.assertRaises((ValueError, Exception)):
            perftune.PerfTunerBase.compute_cpu_mask_for_mode(42, '0xff')


class TestIrqsCpuMaskForMode(unittest.TestCase):
    @mock.patch('perftune.run_hwloc_calc')
    def test_sq_mode(self, mock_calc):
        """
        In sq mode, irqs_cpu_mask_for_mode returns the complement of the compute
        mask relative to cpu_mask (i.e. only CPU 0 handles IRQs).
        """
        mock_calc.side_effect = lambda args: {
            str(['0x000000ff', '~PU:0']): '0x000000fe',
            str(['0x000000ff', '~0x000000fe']): '0x00000001',
        }.get(str(args), '0x000000ff')

        result = perftune.PerfTunerBase.irqs_cpu_mask_for_mode(
            perftune.PerfTunerBase.SupportedModes.sq, '0x000000ff')
        self.assertEqual(result, '0x00000001')

    def test_mq_mode_returns_cpu_mask(self):
        """
        In mq mode all CPUs are eligible for IRQs, so irqs_cpu_mask_for_mode
        returns cpu_mask unchanged.
        """
        result = perftune.PerfTunerBase.irqs_cpu_mask_for_mode(
            perftune.PerfTunerBase.SupportedModes.mq, '0x000000ff')
        self.assertEqual(result, '0x000000ff')

    def test_no_irq_restrictions_returns_cpu_mask(self):
        """
        In no_irq_restrictions mode the IRQ mask equals cpu_mask.
        """
        result = perftune.PerfTunerBase.irqs_cpu_mask_for_mode(
            perftune.PerfTunerBase.SupportedModes.no_irq_restrictions, '0x000000ff')
        self.assertEqual(result, '0x000000ff')

    @mock.patch('perftune.run_hwloc_calc')
    def test_zero_irq_mask_raises(self, mock_calc):
        """
        When the derived IRQ mask is zero, irqs_cpu_mask_for_mode raises
        CPUMaskIsZeroException.
        """
        mock_calc.side_effect = lambda args: {
            str(['0x00000001', '~PU:0']): '0x0',
        }.get(str(args), '0x0')

        with self.assertRaises(perftune.PerfTunerBase.CPUMaskIsZeroException):
            perftune.PerfTunerBase.irqs_cpu_mask_for_mode(
                perftune.PerfTunerBase.SupportedModes.sq, '0x00000001')


class TestAutoDetectIrqMask(unittest.TestCase):
    @mock.patch('perftune.run_hwloc_calc')
    def test_small_machine_4_pus(self, mock_calc):
        """
        Up to 4 PUs: return cpu_mask as-is.
        """
        def calc_side_effect(args):
            if args == ['-I', 'numa', '0x0000000f']:
                return '0'
            if '--number-of' in args:
                if 'core' in args:
                    return '4'
                if 'PU' in args:
                    return '4'
            if '--restrict' in args and 'all' not in args and '--number-of' not in args:
                return '0x0000000f'
            return '0x0000000f'
        mock_calc.side_effect = calc_side_effect

        result = perftune.auto_detect_irq_mask('0x0000000f', 16)
        self.assertEqual(result, '0x0000000f')

    @mock.patch('perftune.run_hwloc_calc')
    def test_medium_machine_4_cores_8_pus(self, mock_calc):
        """
        Up to 4 cores but >4 PUs: return PU:0.
        """
        call_log = []

        def calc_side_effect(args):
            call_log.append(args)
            if args == ['-I', 'numa', '0x000000ff']:
                return '0'
            if '--number-of' in args and 'core' in args and 'numa:0' in args:
                return '4'
            if '--number-of' in args and 'PU' in args and 'numa:0' in args:
                return '8'
            if '--number-of' in args and 'core' in args and 'machine:0' in args:
                return '4'
            if '--number-of' in args and 'PU' in args and 'machine:0' in args:
                return '8'
            if '--restrict' in args and 'PU:0' in args:
                return '0x00000001'
            return '0x000000ff'
        mock_calc.side_effect = calc_side_effect

        result = perftune.auto_detect_irq_mask('0x000000ff', 16)
        self.assertEqual(result, '0x00000001')

    @mock.patch('perftune.run_hwloc_calc')
    def test_medium_machine_single_core(self, mock_calc):
        """
        Up to cores_per_irq_core cores: return core:0.
        """
        def calc_side_effect(args):
            if args == ['-I', 'numa', '0x0000ffff']:
                return '0'
            if '--number-of' in args and 'core' in args and 'numa:0' in args:
                return '8'
            if '--number-of' in args and 'PU' in args and 'numa:0' in args:
                return '16'
            if '--number-of' in args and 'core' in args and 'machine:0' in args:
                return '8'
            if '--number-of' in args and 'PU' in args and 'machine:0' in args:
                return '16'
            if '--restrict' in args and 'core:0' in args:
                return '0x00000003'
            return '0x0000ffff'
        mock_calc.side_effect = calc_side_effect

        result = perftune.auto_detect_irq_mask('0x0000ffff', 16)
        self.assertEqual(result, '0x00000003')

    @mock.patch('perftune.run_hwloc_calc')
    def test_big_machine_multi_numa(self, mock_calc):
        """
        More than cores_per_irq_core cores: distribute across NUMAs.
        """
        def calc_side_effect(args):
            if args == ['-I', 'numa', '0xffffffff']:
                return '0,1'
            if '--number-of' in args and 'core' in args and 'numa:0' in args:
                return '16'
            if '--number-of' in args and 'PU' in args and 'numa:0' in args:
                return '32'
            if '--number-of' in args and 'core' in args and 'numa:1' in args:
                return '16'
            if '--number-of' in args and 'PU' in args and 'numa:1' in args:
                return '32'
            if '--number-of' in args and 'core' in args and 'machine:0' in args:
                return '32'
            if '--number-of' in args and 'PU' in args and 'machine:0' in args:
                return '64'
            if '--restrict' in args and any('node:' in a for a in args):
                return '0x00000303'
            return '0xffffffff'
        mock_calc.side_effect = calc_side_effect

        result = perftune.auto_detect_irq_mask('0xffffffff', 16)
        self.assertEqual(result, '0x00000303')

    @mock.patch('perftune.run_hwloc_calc')
    def test_asymmetric_numa_raises(self, mock_calc):
        """
        Different core counts on different NUMAs raises AutodetectError.
        """
        def calc_side_effect(args):
            if args == ['-I', 'numa', '0xffffffff']:
                return '0,1'
            if '--number-of' in args and 'core' in args and 'numa:0' in args:
                return '16'
            if '--number-of' in args and 'PU' in args and 'numa:0' in args:
                return '32'
            if '--number-of' in args and 'core' in args and 'numa:1' in args:
                return '8'
            if '--number-of' in args and 'PU' in args and 'numa:1' in args:
                return '16'
            return '0xffffffff'
        mock_calc.side_effect = calc_side_effect

        with self.assertRaises(perftune.AutodetectError):
            perftune.auto_detect_irq_mask('0xffffffff', 16)
