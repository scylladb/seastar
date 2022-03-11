#!/bin/env python3

import yaml
import time
import subprocess
import multiprocessing
import argparse
import shutil
import os
import json

t_parser = argparse.ArgumentParser(description='IO scheduler tester')
t_parser.add_argument('--directory', help='Directory to run on', default='/mnt')
t_parser.add_argument('--seastar-build-dir', help='Path to seastar build directory', default='./build/dev/', dest='bdir')
t_parser.add_argument('--duration', help='One run duration', type=int, default=60)
t_parser.add_argument('--shards', help='Number of shards to use', type=int)

sub_parser = t_parser.add_subparsers(help='Use --help for the list of tests')
sub_parser.required = True

parser = sub_parser.add_parser('mixed', help='Run mixed test')
parser.add_argument('--read-reqsize', help='Size of a read request in kbytes', type=int, default=4)
parser.add_argument('--read-fibers', help='Number of reading fibers', type=int, default=5)
parser.add_argument('--read-shares', help='Shares for read workload', type=int, default=2500)
parser.add_argument('--write-reqsize', help='Size of a write request in kbytes', type=int, default=64)
parser.add_argument('--write-fibers', help='Number of writing fibers', type=int, default=2)
parser.add_argument('--write-shares', help='Shares for write workload', type=int, default=100)
parser.add_argument('--sleep-type', help='The io_tester conf.options.sleep_type option', default='busyloop')
parser.add_argument('--pause-dist', help='The io_tester conf.option.pause_distribution option', default='uniform')

args = t_parser.parse_args()


class iotune:
    def __init__(self, args):
        self._iotune = args.bdir + '/apps/iotune/iotune'
        self._dir = args.directory

    def ensure_io_properties(self):
        if os.path.exists('io_properties.yaml'):
            print('Using existing io_properties file')
        else:
            print('Running iotune')
            subprocess.check_call([self._iotune, '--evaluation-directory', self._dir, '--properties-file', 'io_properties.yaml'])


class job:
    def __init__(self, typ, req_size_kb, shares = 100, prl = None, rps = None):
        self._typ = typ
        self._req_size = req_size_kb
        self._prl = prl
        self._rps = rps
        self._shares = shares

    def prl(self):
        return self._prl

    def rqsz(self):
        return self._req_size

    def to_conf_entry(self, name, options):
        ret = {
            'name': name,
            'shards': 'all',
            'type': self._typ,
            'shard_info': {
                'reqsize': f'{self._req_size}kB',
                'shares': self._shares,
            },
        }
        if options is not None:
            ret['options'] = options
        if self._prl is not None:
            ret['shard_info']['parallelism'] = int(self._prl)
        if self._rps is not None:
            ret['shard_info']['rps'] = int(self._rps)
        return ret


class io_tester:
    def __init__(self, args, opts = None, ioprop = 'io_properties.yaml'):
        self._jobs = []
        self._io_tester = args.bdir + '/apps/io_tester/io_tester'
        self._dir = args.directory
        self._use_fraction = 0.8
        self._max_data_size_gb = 8
        self._job_options = opts
        self._io_tester_args = [
            '--io-properties-file', ioprop,
            '--storage', self._dir,
            '--duration', f'{args.duration}',
        ]
        if args.shards is not None:
            self._io_tester_args += [ f'-c{args.shards}' ]

    def add_job(self, name, job):
        self._jobs.append(job.to_conf_entry(name, self._job_options))

    def _setup_data_sizes(self):
        du = shutil.disk_usage(self._dir)
        one_job_space_mb = int(du.free * self._use_fraction / len(self._jobs) / (100*1024*1024)) * 100 # round down to 100MB
        if one_job_space_mb > self._max_data_size_gb * 1024:
            one_job_space_mb = self._max_data_size_gb * 1024
        for j in self._jobs:
            j['data_size'] = f'{one_job_space_mb}MB'

    def names(self):
        return [ j.name for j in _jobs ]

    def run(self):
        if not self._jobs:
            raise 'Empty jobs'

        self._setup_data_sizes()
        yaml.Dumper.ignore_aliases = lambda *args : True
        yaml.dump(self._jobs, open('conf.yaml', 'w'))
        res = subprocess.check_output([self._io_tester, '--conf', 'conf.yaml', *self._io_tester_args], stderr=subprocess.PIPE)
        res = res.split(b'---\n')[1]
        res = yaml.safe_load(res)

        ret = {}
        for shard_res in res:
            for wl in shard_res:
                if wl == 'shard':
                    continue

                if wl not in ret:
                    ret[wl] = {
                            'throughput': 0.0,
                            'IOPS': 0.0,
                            'latencies': {
                                'p0.95': 0,
                            },
                            'stats': {
                                'total_requests': 0,
                                'io_queue_total_operations': 0,
                                'io_queue_total_exec_sec': 0,
                                'io_queue_total_delay_sec': 0,
                            },
                    }

                ret[wl]['throughput'] += shard_res[wl]['throughput']
                ret[wl]['IOPS'] += shard_res[wl]['IOPS']
                ret[wl]['latencies']['p0.95'] += shard_res[wl]['latencies']['p0.95']
                ret[wl]['stats']['total_requests'] += shard_res[wl]['stats']['total_requests']
                ret[wl]['stats']['io_queue_total_operations'] += shard_res[wl]['stats']['io_queue_total_operations']
                ret[wl]['stats']['io_queue_total_delay_sec'] += shard_res[wl]['stats']['io_queue_total_delay_sec']
                ret[wl]['stats']['io_queue_total_exec_sec'] += shard_res[wl]['stats']['io_queue_total_exec_sec']

        for wl in ret:
            ret[wl]['latencies']['p0.95'] /= len(res)

        return ret


iot = iotune(args)
iot.ensure_io_properties()

ioprop = yaml.safe_load(open('io_properties.yaml'))

for prop in ioprop['disks']:
    if prop['mountpoint'] == args.directory:
        ioprop = prop
        break
else:
    raise 'Cannot find required mountpoint in io-properties'


def mixed_show_stat_header():
    print('-' * 20 + '8<' + '-' * 20)
    print('name           througput(kbs) iops lat95(us) queue-time(us) execution-time(us) K(bw) K(iops) K()')


def mixed_run_and_show_results(m, ioprop):
    def xtimes(st, nm):
        return (st[nm] * 1000000) / st["io_queue_total_operations"]

    res = m.run()
    for name in res:
        st = res[name]
        throughput = st['throughput']
        iops = st['IOPS']
        lats = st['latencies']
        stats = st['stats']

        if name.startswith('read'):
            k_bw = throughput / (ioprop['read_bandwidth']/1000)
            k_iops = iops / ioprop['read_iops']
        else:
            k_bw = throughput / (ioprop['write_bandwidth']/1000)
            k_iops = iops / ioprop['write_iops']

        print(f'{name:20} {int(throughput):7} {int(iops):5} {lats["p0.95"]:.1f} {xtimes(stats, "io_queue_total_delay_sec"):.1f} {xtimes(stats, "io_queue_total_exec_sec"):.1f} {k_bw:.3f} {k_iops:.3f} {k_bw + k_iops:.3f}')


def run_mixed_test(args, ioprop):
    nr_cores = args.shards
    if nr_cores is None:
        nr_cores = multiprocessing.cpu_count()

    read_rps_per_shard = int(ioprop['read_iops'] / nr_cores * 0.5)
    read_rps = read_rps_per_shard / args.read_fibers

    options = {
        'sleep_type': args.sleep_type,
        'pause_distribution': args.pause_dist,
    }

    print(f'Read RPS:{read_rps} fibers:{args.read_fibers}')

    mixed_show_stat_header()

    m = io_tester(args, opts = options)
    m.add_job(f'read_rated_{args.read_shares}', job('randread', args.read_reqsize, shares = args.read_shares, prl = args.read_fibers, rps = read_rps))
    mixed_run_and_show_results(m, ioprop)

    m = io_tester(args, opts = options)
    m.add_job(f'write_{args.write_shares}', job('seqwrite', args.write_reqsize, shares = args.write_shares, prl = args.write_fibers))
    m.add_job(f'read_rated_{args.read_shares}', job('randread', args.read_reqsize, shares = args.read_shares, prl = args.read_fibers, rps = read_rps))
    mixed_run_and_show_results(m, ioprop)

run_mixed_test(args, ioprop)
