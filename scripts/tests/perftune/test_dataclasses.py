import dataclasses
import unittest

import perftune


class TestEthtoolLabel(unittest.TestCase):
    def test_returns_field_with_label_metadata(self):
        """
        _ethtool_label() returns a dataclasses.Field whose metadata dict
        contains the provided label string under the 'label' key.
        """
        field = perftune._ethtool_label('TestLabel')
        self.assertIsInstance(field, dataclasses.Field)
        self.assertEqual(field.metadata['label'], 'TestLabel')

    def test_different_labels_produce_different_fields(self):
        """
        Two calls to _ethtool_label() with different strings produce fields
        with different metadata label values.
        """
        f1 = perftune._ethtool_label('A')
        f2 = perftune._ethtool_label('B')
        self.assertNotEqual(f1.metadata['label'], f2.metadata['label'])


class TestLabeledDataclass(unittest.TestCase):
    def test_base_has_no_labels(self):
        """
        The base LabeledDataclass has no fields, so labels() returns an empty
        list and label_to_field_name() returns an empty dict.
        """
        self.assertEqual(perftune.LabeledDataclass.labels(), [])
        self.assertEqual(perftune.LabeledDataclass.label_to_field_name(), {})

    def test_validate_passes_for_valid_subclass(self):
        """
        _validate_labels() does not raise when every field in the subclass has
        an _ethtool_label-produced metadata entry.
        """
        @dataclasses.dataclass
        class Good(perftune.LabeledDataclass):
            x: int = perftune._ethtool_label('X-Label')
        Good._validate_labels()

    def test_validate_fails_for_unlabeled_field(self):
        """
        _validate_labels() raises a TypeError that includes the field name when
        a dataclass field lacks the required 'label' metadata.
        """
        @dataclasses.dataclass
        class Bad(perftune.LabeledDataclass):
            x: int = 0

        with self.assertRaises(TypeError) as ctx:
            Bad._validate_labels()
        self.assertIn('x', str(ctx.exception))

    def test_label_to_field_name_on_subclass(self):
        """
        label_to_field_name() returns a dict mapping each field's lower-cased
        label string to its Python field name.
        """
        @dataclasses.dataclass
        class Sub(perftune.LabeledDataclass):
            alpha: int = perftune._ethtool_label('Alpha-Label')
            beta: int = perftune._ethtool_label('Beta Label')

        self.assertEqual(Sub.label_to_field_name(), {
            'alpha-label': 'alpha',
            'beta label': 'beta',
        })

    def test_labels_preserves_order(self):
        """
        labels() returns the field labels in the same order as they are defined
        in the dataclass.
        """
        @dataclasses.dataclass
        class Sub(perftune.LabeledDataclass):
            first: int = perftune._ethtool_label('First')
            second: int = perftune._ethtool_label('Second')

        self.assertEqual(Sub.labels(), ['First', 'Second'])

    def test_mixed_labeled_and_unlabeled_fails(self):
        """
        A dataclass with at least one unlabeled field causes
        label_to_field_name() to raise a TypeError.
        """
        @dataclasses.dataclass
        class Mixed(perftune.LabeledDataclass):
            labeled: int = perftune._ethtool_label('Labeled')
            unlabeled: int = 0

        with self.assertRaises(TypeError):
            Mixed.label_to_field_name()


class TestEthtoolChannelPropertiesValues(unittest.TestCase):
    CLS = perftune.EthtoolChannelPropertiesValues

    def test_is_dataclass(self):
        """
        EthtoolChannelPropertiesValues is a proper dataclass as reported by
        dataclasses.is_dataclass().
        """
        self.assertTrue(dataclasses.is_dataclass(self.CLS))

    def test_construction_all_none(self):
        """
        EthtoolChannelPropertiesValues can be constructed with all four
        channel fields set to None, representing 'n/a' ethtool values.
        """
        v = self.CLS(rx=None, tx=None, other=None, combined=None)
        self.assertIsNone(v.rx)
        self.assertIsNone(v.tx)
        self.assertIsNone(v.other)
        self.assertIsNone(v.combined)

    def test_construction_all_ints(self):
        """
        All four fields (rx, tx, other, combined) accept integer values and
        store them correctly.
        """
        v = self.CLS(rx=1, tx=2, other=3, combined=4)
        self.assertEqual((v.rx, v.tx, v.other, v.combined), (1, 2, 3, 4))

    def test_construction_mixed(self):
        """
        A mix of None and integer values is accepted; individual fields are
        independently accessible.
        """
        v = self.CLS(rx=None, tx=None, other=None, combined=16)
        self.assertIsNone(v.rx)
        self.assertEqual(v.combined, 16)

    def test_equality(self):
        """
        Two instances with identical field values compare as equal.
        """
        a = self.CLS(rx=None, tx=None, other=None, combined=16)
        b = self.CLS(rx=None, tx=None, other=None, combined=16)
        self.assertEqual(a, b)

    def test_inequality(self):
        """
        Two instances that differ in at least one field compare as not equal.
        """
        a = self.CLS(rx=None, tx=None, other=None, combined=16)
        b = self.CLS(rx=None, tx=None, other=None, combined=2)
        self.assertNotEqual(a, b)

    def test_labels(self):
        """
        The labels of EthtoolChannelPropertiesValues are ['RX', 'TX', 'Other',
        'Combined'], matching the ethtool -l output headings.
        """
        self.assertEqual(self.CLS.labels(), ['RX', 'TX', 'Other', 'Combined'])

    def test_label_to_field_name(self):
        """
        label_to_field_name() maps the lower-cased label strings to their
        corresponding Python attribute names.
        """
        self.assertEqual(self.CLS.label_to_field_name(), {
            'rx': 'rx',
            'tx': 'tx',
            'other': 'other',
            'combined': 'combined',
        })


class TestEthtoolLChannelInfo(unittest.TestCase):
    CLS = perftune.EthtoolLChannelInfo
    PROPS = perftune.EthtoolChannelPropertiesValues

    def _make_props(self, combined=0):
        return self.PROPS(rx=None, tx=None, other=None, combined=combined)

    def test_is_dataclass(self):
        """
        EthtoolLChannelInfo is a proper dataclass.
        """
        self.assertTrue(dataclasses.is_dataclass(self.CLS))

    def test_construction(self):
        """
        EthtoolLChannelInfo holds two EthtoolChannelPropertiesValues instances
        (preset_maximums and current_hardware_settings) and exposes them as
        attributes.
        """
        info = self.CLS(
            preset_maximums=self._make_props(16),
            current_hardware_settings=self._make_props(2))
        self.assertEqual(info.preset_maximums.combined, 16)
        self.assertEqual(info.current_hardware_settings.combined, 2)

    def test_labels(self):
        """
        The section labels for EthtoolLChannelInfo are
        ['Pre-set maximums', 'Current hardware settings'], matching the ethtool
        -l output section headings.
        """
        self.assertEqual(self.CLS.labels(), ['Pre-set maximums', 'Current hardware settings'])

    def test_label_to_field_name(self):
        """
        label_to_field_name() maps the lower-cased section labels to
        'preset_maximums' and 'current_hardware_settings' respectively.
        """
        self.assertEqual(self.CLS.label_to_field_name(), {
            'pre-set maximums': 'preset_maximums',
            'current hardware settings': 'current_hardware_settings',
        })
