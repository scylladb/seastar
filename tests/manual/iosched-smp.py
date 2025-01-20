#!/bin/env python3

import os.path
import yaml
import shutil
import subprocess
import argparse
from functools import reduce

parser = argparse.ArgumentParser(description='IO scheduler tester')
parser.add_argument('--directory', help='Directory to run on', default='/mnt')
parser.add_argument('--seastar-build-dir', help='Path to seastar build directory', default='./build/dev/', dest='bdir')
parser.add_argument('--duration', help='One run duration', default=60)
parser.add_argument('--test', help='Test name to run')
args = parser.parse_args()

class iotune:
    def __init__(self, args):
        self._iotune = args.bdir + '/apps/iotune/iotune'
        self._dir = args.directory

    def ensure_io_properties(self):
        if os.path.exists('io_properties.yaml'):
            print('Using existing io_properties file')
        else:
            print('Running iotune')
            subprocess.check_call([self._iotune, '--evaluation-directory', self._dir, '-c1', '--properties-file', 'io_properties.yaml'])

class ioinfo:
    def __init__(self, args):
        self._ioinfo = args.bdir + '/apps/io_tester/ioinfo'
        self._dir = args.directory
        res = subprocess.check_output([self._ioinfo, '--directory', self._dir, '--io-properties-file', 'io_properties.yaml'])
        self._info = yaml.safe_load(res)

    def min_read_length(self):
        return 4

    def max_read_length(self):
        return min(self._info['disk_read_max_length'] / 1024, 128)

    def min_write_length(self):
        return 4

    def max_write_length(self):
        return min(self._info['disk_write_max_length'] / 1024, 64)


class job:
    def __init__(self, typ, req_size_kb, prl, shards, shares, rps=None):
        self._typ = typ
        self._req_size = req_size_kb
        self._prl = prl
        self._shares = shares
        self._shards = shards
        self._rps = rps

    def to_conf_entry(self, name):
        ret = {
            'name': name,
            'shards': self._shards,
            'type': self._typ,
            'shard_info': {
                'parallelism': self._prl,
                'reqsize': f'{self._req_size}kB',
                'shares': self._shares
            }
        }
        if self._rps is not None:
            ret['shard_info']['rps'] = self._rps
        return ret

    def shares(self):
        return self._shares


class io_tester:
    def __init__(self, args, smp):
        self._jobs = []
        self._duration = args.duration
        self._io_tester = args.bdir + '/apps/io_tester/io_tester'
        self._dir = args.directory
        self._use_fraction = 0.1
        self._smp = smp

    def add_job(self, name, job):
        self._jobs.append(job.to_conf_entry(name))

    def _setup_data_sizes(self):
        du = shutil.disk_usage(self._dir)
        one_job_space_mb = int(du.free * self._use_fraction / len(self._jobs) / (100*1024*1024)) * 100 # round down to 100MB
        for j in self._jobs:
            j['data_size'] = f'{one_job_space_mb}MB'

    def run(self):
        if not self._jobs:
            raise 'Empty jobs'

        self._setup_data_sizes()
        yaml.dump(self._jobs, open('conf.yaml', 'w'))
        self._proc = subprocess.Popen([self._io_tester, '--storage', self._dir, f'-c{self._smp}', '--num-io-groups', '1', '--conf', 'conf.yaml', '--duration', f'{self._duration}', '--io-properties-file', 'io_properties.yaml'], stdout=subprocess.PIPE)
        res = self._proc.communicate()
        res = res[0].split(b'---\n')[1]
        return yaml.safe_load(res)


def run_jobs(jobs, args, smp):
    iot = io_tester(args, smp)
    results = {}
    for j in jobs:
        iot.add_job(j, jobs[j])
        results[j] = { 'iops': 0, 'shares': jobs[j].shares() }

    out = iot.run()
    statuses = {}

    for j in results:
        for shard in out:
            if j in shard:
                results[j]['iops'] += shard[j]['IOPS']

    return results


iotune(args).ensure_io_properties()
ioinf = ioinfo(args)

def run_shares_balance_test():
    for s in [ 100, 200, 400, 800 ]:
        def ratios(res, idx):
            shares_ratio = float(res[f'shard_{idx}']['shares']) / float(res['shard_0']['shares'])
            iops_ratio = float(res[f'shard_{idx}']['iops']) / float(res['shard_0']['iops'])
            return (shares_ratio, iops_ratio)

        res = run_jobs({
                'shard_0': job('randread', ioinf.min_read_length(), 16, ['0'], 100),
                'shard_1': job('randread', ioinf.min_read_length(), 24, ['1'], s),
        }, args, 2)
        print(f'{res}')
        shares_ratio, iops_ratio = ratios(res, 1)
        print(f'IOPS ratio {iops_ratio:.3}, expected {shares_ratio:.3}, deviation {int(abs(shares_ratio - iops_ratio)*100.0/shares_ratio)}%')

        for s2 in [ 200, 400, 800 ]:
            if s2 <= s:
                continue

            res = run_jobs({
                    'shard_0': job('randread', ioinf.min_read_length(), 16, ['0'], 100),
                    'shard_1': job('randread', ioinf.min_read_length(), 24, ['1'], s),
                    'shard_2': job('randread', ioinf.min_read_length(), 32, ['2'], s2),
            }, args, 3)
            print(f'{res}')
            shares_ratio, iops_ratio = ratios(res, 1)
            print(f'shard-1 IOPS ratio {iops_ratio:.3}, expected {shares_ratio:.3}, deviation {int(abs(shares_ratio - iops_ratio)*100.0/shares_ratio)}%')
            shares_ratio, iops_ratio = ratios(res, 2)
            print(f'shard-2 IOPS ratio {iops_ratio:.3}, expected {shares_ratio:.3}, deviation {int(abs(shares_ratio - iops_ratio)*100.0/shares_ratio)}%')

def run_rps_balance_test():
    for rps in [ 1000, 2000, 4000 ]:
        res = run_jobs({
            'shard_0': job('randread', ioinf.min_read_length(), 1, ['0'], 100, rps=1000),
            'shard_1': job('randread', ioinf.min_read_length(), 1, ['1'], 100, rps=rps),
        }, args, 2)
        print(f'iops = {res["shard_0"]["iops"]}:{res["shard_1"]["iops"]} want 1000:{rps}')

run_shares_balance_test()
run_rps_balance_test()
