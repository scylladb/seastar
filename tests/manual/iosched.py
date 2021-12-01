#!/bin/env python3

import os.path
import yaml
import shutil
import subprocess
import argparse
from functools import reduce

# Default task-quota-ms is 0.5ms
# IO subsystem sets its latency goal to 1.5 of the above
# Test adds 5% to compensate for potential glitches
default_latency_goal = 0.5 * 1.5 * 1.05 / 1000

parser = argparse.ArgumentParser(description='IO scheduler tester')
parser.add_argument('--directory', help='Directory to run on', default='/mnt')
parser.add_argument('--seastar-build-dir', help='Path to seastar build directory', default='./build/dev/', dest='bdir')
parser.add_argument('--duration', help='One run duration', default=60)
parser.add_argument('--latency-goal', help='Target latency the scheduler should meet', default=default_latency_goal)
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

    def max_read_length(self):
        return min(self._info['disk_read_max_length'] / 1024, 128)

    def min_write_length(self):
        return 4

    def max_write_length(self):
        return min(self._info['disk_write_max_length'] / 1024, 64)


class job:
    def __init__(self, typ, req_size_kb, prl):
        self._typ = typ
        self._req_size = req_size_kb
        self._prl = prl
        self._shares = 100

    def prl(self):
        return self._prl

    def rqsz(self):
        return self._req_size

    def to_conf_entry(self, name):
        return {
            'name': name,
            'shards': 'all',
            'type': self._typ,
            'shard_info': {
                'parallelism': self._prl,
                'reqsize': f'{self._req_size}kB',
                'shares': self._shares
            }
        }

class io_tester:
    def __init__(self, args):
        self._jobs = []
        self._duration = args.duration
        self._io_tester = args.bdir + '/apps/io_tester/io_tester'
        self._dir = args.directory
        self._use_fraction = 0.8

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
        self._proc = subprocess.Popen([self._io_tester, '--storage', self._dir, '-c1', '--poll-mode', '--conf', 'conf.yaml', '--duration', f'{self._duration}', '--io-properties-file', 'io_properties.yaml'], stdout=subprocess.PIPE)
        res = self._proc.communicate()
        res = res[0].split(b'---\n')[1]
        return yaml.safe_load(res)[0]


def run_jobs(jobs, args):
    iot = io_tester(args)
    names = []
    for j in jobs:
        iot.add_job(j, jobs[j])
        names.append(j)

    results = iot.run()
    statuses = []

    # For now the only criteria is that requests don't spend
    # more time in disk than the IO latency goal is
    for n in names:
        res = results[n]['stats']
        total_ops = res['io_queue_total_operations']
        max_in_disk = args.latency_goal * total_ops

        status = {
            'meets_goal': res['io_queue_total_exec_sec'] <= max_in_disk,
            'queues': res['io_queue_total_delay_sec'] > total_ops * 0.0005
        }
        statuses.append(status)

        label = ''
        average_exec = '%.3f' % ((res['io_queue_total_exec_sec'] / total_ops) * 1000)
        average_inq = '%.3f' % ((res['io_queue_total_delay_sec'] / total_ops) * 1000)
        average_lat = '%.3f' % (results[n]['latencies']['average'] / 1000)
        label += '' if status['meets_goal'] else '  no-goal'
        label += ' saturated' if status['queues'] else ''
        print(f'\t{n:16}  exec={average_exec}ms  queue={average_inq}ms  latency={average_lat}ms{label}')

    return statuses

def check_goal(statuses):
    return reduce(lambda a, b: a and b, map(lambda x: x['meets_goal'], statuses))

def check_saturated(statuses):
    return reduce(lambda a, b: a or b, map(lambda x: x['queues'], statuses))

def run_saturation_test(name, get_jobs, args):
    print(f'** running {name} saturation test')
    prl = 4
    meets_goal = True
    saturated = False
    while True:
        st = run_jobs(get_jobs(prl), args)
        saturated = check_saturated(st)
        meets_goal &= check_goal(st)

        if saturated or prl > 8000:
            break
        else:
            prl *= 4

    for p in [ prl * 2, prl * 3 ]:
        st = run_jobs(get_jobs(p), args)
        meets_goal &= check_goal(st)

    if meets_goal:
        test_statuses[name] = 'PASS'
        print("PASS")
    elif saturated:
        test_statuses[name] = 'FAIL'
        print("FAIL -- doesn't meet goal")
    else:
        test_statuses[name] = 'FAIL (*)'
        print("FAIL -- doesn't meet goal, no saturation")

def run_base_test(name, get_jobs, args):
    print(f'* running {name} test')
    st = run_jobs(get_jobs(1), args)
    if check_goal(st):
        return True

    test_statuses[name] = 'SKIP'
    print("SKIP -- doesn't meet goal, no concurrency")
    return False

def run_pure_test(name, get_jobs, args):
    if run_base_test(name, get_jobs, args):
        run_saturation_test(name, get_jobs, args)

def run_mixed_tests(name, get_jobs, args):
    if run_base_test(name, lambda p: get_jobs(p, p), args):
        run_saturation_test(name + ' symmetrical', lambda p: get_jobs(p, p), args)
        run_saturation_test(name + ' one-write', lambda p: get_jobs(p, 1), args)
        run_saturation_test(name + ' one-read', lambda p: get_jobs(1, p), args)


def get_read_job(p, rqsz):
    j = job('randread', rqsz, p)
    return { f'{rqsz}k_x{p}': j }

def get_write_job(p, rqsz):
    j = job('seqwrite', rqsz, p)
    return { f'{rqsz}k_x{p}': j }

def get_mixed_jobs(rp, rqsz, wp, wqsz):
    rj = job('randread', rqsz, rp)
    wj = job('seqwrite', wqsz, wp)
    return { f'r_{rqsz}k_x{rp}': rj, f'w_{wqsz}k_x{wp}': wj }

iotune(args).ensure_io_properties()
ioinf = ioinfo(args)

test_statuses = {}

run_pure_test('pure read', lambda p: get_read_job(p, ioinf.max_read_length()), args)
run_pure_test('pure small write', lambda p: get_write_job(p, ioinf.min_write_length()), args)
run_pure_test('pure large write', lambda p: get_write_job(p, ioinf.max_write_length()), args)
run_mixed_tests('mixed read / small write', lambda rp, wp: get_mixed_jobs(rp, ioinf.max_read_length(), wp, ioinf.min_write_length()), args)
run_mixed_tests('mixed read / large write', lambda rp, wp: get_mixed_jobs(rp, ioinf.max_read_length(), wp, ioinf.max_write_length()), args)

for tn in test_statuses:
    print(f'{tn:40}: {test_statuses[tn]}')
