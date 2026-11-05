import sys
import unittest

import perftune


class TestCpuMaskIsZero(unittest.TestCase):
    def test_single_zero(self):
        """
        A single '0x0' component is recognised as a zero mask.
        """
        self.assertTrue(perftune.PerfTunerBase.cpu_mask_is_zero('0x0'))

    def test_multi_component_zeros(self):
        """
        Multiple comma-separated zero components all evaluate to a zero mask.
        """
        self.assertTrue(perftune.PerfTunerBase.cpu_mask_is_zero('0x0,0x0'))

    def test_empty_components(self):
        """
        Empty components (',,') between explicit zeros are ignored; the overall
        mask is still zero.
        """
        self.assertTrue(perftune.PerfTunerBase.cpu_mask_is_zero('0x0,,0x0'))

    def test_all_empty(self):
        """
        A mask string that is entirely commas with no hex components is treated
        as all-zeros.
        """
        self.assertTrue(perftune.PerfTunerBase.cpu_mask_is_zero(',,'))

    def test_single_nonzero(self):
        """
        A single non-zero hex component means the mask is not zero.
        """
        self.assertFalse(perftune.PerfTunerBase.cpu_mask_is_zero('0xff'))

    def test_nonzero_with_gaps(self):
        """
        Non-zero components separated by empty gaps (',,') make the overall
        mask non-zero.
        """
        self.assertFalse(perftune.PerfTunerBase.cpu_mask_is_zero('0xffff,,0xffff'))

    def test_one_nonzero_in_chain(self):
        """
        When one component in a chain is non-zero, cpu_mask_is_zero returns
        False even if the others are zero.
        """
        self.assertFalse(perftune.PerfTunerBase.cpu_mask_is_zero('0x0,0x1,0x0'))


class TestSupportedModes(unittest.TestCase):
    SM = perftune.PerfTunerBase.SupportedModes

    def test_ordering(self):
        """
        sq_split < sq < mq < no_irq_restrictions, reflecting the ordering from
        most-restrictive to least-restrictive tuning mode.
        """
        self.assertLess(self.SM.sq_split, self.SM.sq)
        self.assertLess(self.SM.sq, self.SM.mq)
        self.assertLess(self.SM.mq, self.SM.no_irq_restrictions)

    def test_combine_single(self):
        """
        combine() with a single-element list returns that mode unchanged.
        """
        self.assertEqual(self.SM.combine([self.SM.mq]), self.SM.mq)

    def test_combine_picks_minimum(self):
        """
        combine([mq, sq]) returns sq because sq is more restrictive (smaller
        IntEnum value) than mq.
        """
        self.assertEqual(
            self.SM.combine([self.SM.mq, self.SM.sq]),
            self.SM.sq)

    def test_combine_all(self):
        """
        combine([mq, sq, sq_split]) returns sq_split, the most restrictive mode
        in the set.
        """
        self.assertEqual(
            self.SM.combine([self.SM.mq, self.SM.sq, self.SM.sq_split]),
            self.SM.sq_split)

    def test_combine_with_no_irq_restrictions(self):
        """
        no_irq_restrictions is the least restrictive mode; combining it with mq
        yields mq.
        """
        self.assertEqual(
            self.SM.combine([self.SM.no_irq_restrictions, self.SM.mq]),
            self.SM.mq)

    def test_names_contains_all(self):
        """
        SupportedModes.names() exposes all four mode identifiers:
        sq_split, sq, mq, and no_irq_restrictions.
        """
        names = self.SM.names()
        for m in ('sq_split', 'sq', 'mq', 'no_irq_restrictions'):
            self.assertIn(m, names)

    def test_values(self):
        """
        The integer values of the modes are fixed: sq_split=0, sq=1, mq=2,
        no_irq_restrictions=9999.
        """
        self.assertEqual(self.SM.sq_split.value, 0)
        self.assertEqual(self.SM.sq.value, 1)
        self.assertEqual(self.SM.mq.value, 2)
        self.assertEqual(self.SM.no_irq_restrictions.value, 9999)

    def test_combine_invalid_value_raises(self):
        """
        Passing an integer that is not a valid SupportedModes member to
        combine() raises a ValueError.
        """
        with self.assertRaises(ValueError):
            self.SM.combine([42])


class TestParseCpuMaskFromYaml(unittest.TestCase):
    def test_single_hex_mask(self):
        """
        A valid single-component hex mask is returned unchanged by
        parse_cpu_mask_from_yaml.
        """
        y = {'cpu_mask': '0x00000001'}
        self.assertEqual(perftune.parse_cpu_mask_from_yaml(y, 'cpu_mask', 'f.yaml'), '0x00000001')

    def test_multi_component_mask(self):
        """
        A comma-separated two-component mask passes validation and is returned
        as-is.
        """
        y = {'cpu_mask': '0xFFFFFFFF,0xFFFFFFFF'}
        self.assertEqual(perftune.parse_cpu_mask_from_yaml(y, 'cpu_mask', 'f.yaml'), '0xFFFFFFFF,0xFFFFFFFF')

    def test_mask_with_empty_middle_component(self):
        """
        A mask with an empty middle component (e.g. '0xFFFF,,0xFFFF') is valid
        and returned unchanged.
        """
        y = {'cpu_mask': '0xFFFF,,0xFFFF'}
        self.assertEqual(perftune.parse_cpu_mask_from_yaml(y, 'cpu_mask', 'f.yaml'), '0xFFFF,,0xFFFF')

    def test_invalid_mask_raises(self):
        """
        A string that is not a valid hex mask raises an exception from
        parse_cpu_mask_from_yaml.
        """
        y = {'cpu_mask': 'not_a_mask'}
        with self.assertRaises(Exception):
            perftune.parse_cpu_mask_from_yaml(y, 'cpu_mask', 'f.yaml')

    def test_no_0x_prefix_raises(self):
        """
        A hex string without the '0x' prefix is rejected as an invalid mask.
        """
        y = {'cpu_mask': 'FFFF'}
        with self.assertRaises(Exception):
            perftune.parse_cpu_mask_from_yaml(y, 'cpu_mask', 'f.yaml')

    def test_empty_string_raises(self):
        """
        An empty string is not a valid CPU mask and raises an exception.
        """
        y = {'cpu_mask': ''}
        with self.assertRaises(Exception):
            perftune.parse_cpu_mask_from_yaml(y, 'cpu_mask', 'f.yaml')

    def test_three_component_mask(self):
        """
        A three-component mask with mixed zero and non-zero parts is accepted
        and returned verbatim.
        """
        y = {'cpu_mask': '0x0000FFFF,0x00000000,0x0000FFFF'}
        self.assertEqual(
            perftune.parse_cpu_mask_from_yaml(y, 'cpu_mask', 'f.yaml'),
            '0x0000FFFF,0x00000000,0x0000FFFF')


class TestExtendAndUnique(unittest.TestCase):
    def test_no_duplicates(self):
        """
        extend_and_unique merges two disjoint lists and returns all elements
        without any duplicates.
        """
        self.assertEqual(sorted(perftune.extend_and_unique([1, 2], [3, 4])), [1, 2, 3, 4])

    def test_with_duplicates(self):
        """
        Elements present in both lists appear only once in the result.
        """
        self.assertEqual(sorted(perftune.extend_and_unique([1, 2], [2, 3])), [1, 2, 3])

    def test_all_duplicates(self):
        """
        When both lists contain only the same single element, the result
        contains that element exactly once.
        """
        self.assertEqual(perftune.extend_and_unique([1, 1], [1, 1]), [1])

    def test_both_empty(self):
        """
        Merging two empty lists yields an empty list.
        """
        self.assertEqual(perftune.extend_and_unique([], []), [])

    def test_extend_empty(self):
        """
        Merging a non-empty list with an empty list preserves the original
        elements.
        """
        self.assertEqual(sorted(perftune.extend_and_unique([1, 2], [])), [1, 2])

    def test_into_empty(self):
        """
        Extending an empty list with a non-empty list returns the non-empty
        list's elements.
        """
        self.assertEqual(sorted(perftune.extend_and_unique([], [3, 4])), [3, 4])

    def test_strings(self):
        """
        extend_and_unique works with string elements as well as integers,
        deduplicating correctly.
        """
        result = perftune.extend_and_unique(['a', 'b'], ['b', 'c'])
        self.assertEqual(sorted(result), ['a', 'b', 'c'])


class TestParseTriStateArg(unittest.TestCase):
    def test_none_returns_none(self):
        """
        A None input to parse_tri_state_arg is returned as None without any
        conversion.
        """
        self.assertIsNone(perftune.parse_tri_state_arg(None, 'test'))

    def test_true_values(self):
        """
        The strings '1', 'yes', 'true', 'on', and 'y' are all recognised as
        True by parse_tri_state_arg.
        """
        for val in ('1', 'yes', 'true', 'on', 'y'):
            self.assertTrue(perftune.parse_tri_state_arg(val, 'test'))

    def test_false_values(self):
        """
        The strings '0', 'no', 'false', 'off', and 'n' are all recognised as
        False by parse_tri_state_arg.
        """
        for val in ('0', 'no', 'false', 'off', 'n'):
            self.assertFalse(perftune.parse_tri_state_arg(val, 'test'))

    def test_invalid_value_exits(self):
        """
        An unrecognised string causes parse_tri_state_arg to call sys.exit with
        a message containing the invalid value and the flag name.
        """
        with self.assertRaises(SystemExit) as ctx:
            perftune.parse_tri_state_arg('invalid', '--test-flag')
        self.assertIn('Invalid --test-flag value: should be boolean but given:', str(ctx.exception))
        self.assertIn('invalid', str(ctx.exception))


class TestTuneModes(unittest.TestCase):
    def test_names_contains_all(self):
        """
        TuneModes.names() includes 'disks', 'net', and 'system'.
        """
        names = perftune.TuneModes.names()
        for n in ('disks', 'net', 'system'):
            self.assertIn(n, names)

    def test_values(self):
        """
        The integer values of the tune modes are disks=0, net=1, system=2.
        """
        self.assertEqual(perftune.TuneModes.disks.value, 0)
        self.assertEqual(perftune.TuneModes.net.value, 1)
        self.assertEqual(perftune.TuneModes.system.value, 2)


class TestAutodetectError(unittest.TestCase):
    def test_is_exception(self):
        """
        AutodetectError is a subclass of Exception so it can be caught by
        generic exception handlers.
        """
        self.assertTrue(issubclass(perftune.AutodetectError, Exception))

    def test_message(self):
        """
        The message passed to AutodetectError is available via str().
        """
        err = perftune.AutodetectError("test message")
        self.assertEqual(str(err), "test message")


class TestSupportedDiskTypes(unittest.TestCase):
    def test_values(self):
        """
        DiskPerfTuner.SupportedDiskTypes has nvme=0 and non_nvme=1.
        """
        self.assertEqual(perftune.DiskPerfTuner.SupportedDiskTypes.nvme.value, 0)
        self.assertEqual(perftune.DiskPerfTuner.SupportedDiskTypes.non_nvme.value, 1)

    def test_is_int_enum(self):
        """
        SupportedDiskTypes is an IntEnum, allowing integer comparisons and
        dict-key usage.
        """
        import enum
        self.assertTrue(issubclass(perftune.DiskPerfTuner.SupportedDiskTypes, enum.IntEnum))


class TestMinCoresPerIrqCore(unittest.TestCase):
    def test_returns_5(self):
        """
        PerfTunerBase.min_cores_per_irq_core() returns 5, the minimum accepted
        value for the cores_per_irq_core tuning parameter.
        """
        self.assertEqual(perftune.PerfTunerBase.min_cores_per_irq_core(), 5)


class TestCpuMaskIsZeroException(unittest.TestCase):
    def test_is_exception(self):
        """
        CPUMaskIsZeroException is a subclass of Exception.
        """
        self.assertTrue(issubclass(perftune.PerfTunerBase.CPUMaskIsZeroException, Exception))


class TestInvalidNUMATopologyException(unittest.TestCase):
    def test_is_exception(self):
        """
        InvalidNUMATopologyException is a subclass of Exception.
        """
        self.assertTrue(issubclass(perftune.PerfTunerBase.InvalidNUMATopologyException, Exception))
