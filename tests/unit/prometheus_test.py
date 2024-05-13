#!/usr/bin/env python3
#
# This file is open source software, licensed to you under the terms
# of the Apache License, Version 2.0 (the "License").  See the NOTICE file
# distributed with this work for additional information regarding copyright
# ownership.  You may not use this file except in compliance with the License.
#
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#
#
# Copyright (C) 2024 Scylladb, Ltd.
#

import argparse
import re
import subprocess
import sys
import unittest
import urllib.request
import urllib.parse

from typing import Optional
from collections import namedtuple


class Exposition:
    def __init__(self,
                 name: str,
                 type_: str,
                 value: str,
                 labels: Optional[dict[str, str]] = None) -> None:
        self.name = name
        if type_ == 'counter':
            self.value = float(value)
        elif type_ == 'gauge':
            self.value = float(value)
        else:
            # we don't verify histogram or summary yet
            self.value = None
        self.labels = labels


class Metrics:
    prefix = 'seastar'
    group = 'test_group'
    # parse lines like:
    # rest_api_scheduler_queue_length{group="main",shard="0"} 0.000000
    # where:
    #   - "rest_api" is the prometheus prefix
    #   - "scheduler" is the metric group name
    #   - "queue_length" is the name of the metric
    #   - the kv pairs in "{}" are labels"
    #   - "0.000000" is the value of the metric
    # this format is compatible with
    # https://github.com/prometheus/docs/blob/main/content/docs/instrumenting/exposition_formats.md
    # NOTE: scylla does not include timestamp in the exported metrics
    pattern = re.compile(r'''(?P<metric_name>\w+)   # rest_api_scheduler_queue_length
                             \{(?P<labels>[^\}]*)\} # {group="main",shard="0"}
                             \s+                    # <space>
                             (?P<value>[^\s]+)      # 0.000000''', re.X)

    def __init__(self, lines: list[str]) -> None:
        self.lines: list[str] = lines

    @classmethod
    def full_name(cls, name: str) -> str:
        '''return the full name of a metrics
        '''
        return f'{cls.group}_{name}'

    @staticmethod
    def _parse_labels(s: str) -> dict[str, str]:
        return dict(name_value.split('=', 1) for name_value in s.split(','))

    def get(self,
            name: Optional[str] = None,
            labels: Optional[dict[str, str]] = None) -> list[Exposition]:
        '''Return all expositions matching the given name and labels
        '''
        full_name = None
        if name is not None:
            full_name = f'{self.prefix}_{self.group}_{name}'
        results: list[Exposition] = []
        metric_type = None

        for line in self.lines:
            if not line:
                continue
            if line.startswith('# HELP'):
                continue
            if line.startswith('# TYPE'):
                _, _, metric_name, type_ = line.split()
                if full_name is None or metric_name == full_name:
                    metric_type = type_
                continue
            matched = self.pattern.match(line)
            assert matched, f'malformed metric line: {line}'

            metric_name = matched.group('metric_name')
            if full_name and metric_name != full_name:
                continue

            metric_labels = self._parse_labels(matched.group('labels'))
            if labels is not None and metric_labels != labels:
                continue

            metric_value = matched.group('value')
            results.append(Exposition(metric_name,
                                      metric_type,
                                      metric_value,
                                      metric_labels))
        return results

    def get_help(self, name: str) -> Optional[str]:
        full_name = f'{self.prefix}_{self.group}_{name}'
        header = f'# HELP {full_name}'
        for line in self.lines:
            if line.startswith(header):
                tokens = line.split(maxsplit=3)
                return tokens[-1]
        return None


class TestPrometheus(unittest.TestCase):
    exporter_path = None
    exporter_process = None
    exporter_config = None
    port = 10001

    @classmethod
    def setUpClass(cls) -> None:
        args = [cls.exporter_path,
                '--port', f'{cls.port}',
                '--conf', cls.exporter_config,
                '--smp=2']
        cls.exporter_process = subprocess.Popen(args,
                                                stdout=subprocess.PIPE,
                                                stderr=subprocess.DEVNULL,
                                                bufsize=0, text=True)
        # wait until the server is ready for serve
        cls.exporter_process.stdout.readline()

    @classmethod
    def tearDownClass(cls) -> None:
        cls.exporter_process.terminate()

    @classmethod
    def _get_metrics(cls,
                     name: Optional[str] = None,
                     labels: Optional[dict[str, str]] = None,
                     with_help: bool = True,
                     aggregate: bool = True) -> Metrics:
        query: dict[str, str] = {}
        if name is not None:
            query['__name__'] = name
        if labels is not None:
            query.update(labels)
        if not with_help:
            query['__help__'] = 'false'
        if not aggregate:
            query['__aggregate__'] = 'false'
        params = urllib.parse.urlencode(query)
        host = 'localhost'
        url = f'http://{host}:{cls.port}/metrics?{params}'
        with urllib.request.urlopen(url) as f:
            body = f.read().decode('utf-8')
            return Metrics(body.rstrip().split('\n'))

    def test_filtering_by_label(self) -> None:
        TestCase = namedtuple('TestCase', ['label', 'regex', 'found'])
        label = 'private'
        tests = [
            TestCase(label=label, regex='dne', found=0),
            TestCase(label=label, regex='404', found=0),
            TestCase(label=label, regex='2', found=1),
            # aggregated
            TestCase(label=label, regex='2|3', found=1),
        ]
        for test in tests:
            with self.subTest(regex=test.regex, found=test.found):
                metrics = self._get_metrics(labels={test.label: test.regex})
                self.assertEqual(len(metrics.get()), test.found)

    def test_aggregated(self) -> None:
        name = 'counter_1'
        # see also rest_api_httpd.cc::aggregate_by_name
        TestCase = namedtuple('TestCase', ['aggregate', 'expected_values'])
        tests = [
            TestCase(aggregate=False, expected_values=[1, 2]),
            TestCase(aggregate=True, expected_values=[3])
        ]
        for test in tests:
            with self.subTest(aggregate=test.aggregate,
                              values=test.expected_values):
                metrics = self._get_metrics(Metrics.full_name(name), aggregate=test.aggregate)
                expositions = metrics.get(name)
                actual_values = sorted(e.value for e in expositions)
                self.assertEqual(actual_values, test.expected_values)

    def test_help(self) -> None:
        name = 'counter_1'
        tests = [True, False]
        for with_help in tests:
            with self.subTest(with_help=with_help):
                metrics = self._get_metrics(Metrics.full_name(name), with_help=with_help)
                msg = metrics.get_help(name)
                if with_help:
                    self.assertIsNotNone(msg)
                else:
                    self.assertIsNone(msg)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--exporter',
                        required=True,
                        help='Path to the exporter executable')
    parser.add_argument('--config',
                        required=True,
                        help='Path to the metrics definition file')
    opts, remaining = parser.parse_known_args()
    remaining.insert(0, sys.argv[0])
    TestPrometheus.exporter_path = opts.exporter
    TestPrometheus.exporter_config = opts.config
    unittest.main(argv=remaining)
