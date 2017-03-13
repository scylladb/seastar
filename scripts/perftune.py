#!/usr/bin/python3

import argparse
import glob
import itertools
import os
import psutil
import re
import shutil
import subprocess
import sys

def run_one_command(prog_args, my_stderr=None):
    return str(subprocess.check_output(prog_args, stderr=my_stderr), 'utf-8')

def run_hwloc_distrib(prog_args):
    """
    Returns a list of strings - each representing a single line of hwloc-distrib output.
    """
    return run_one_command(['hwloc-distrib'] + prog_args).splitlines()

def run_hwloc_calc(prog_args):
    """
    Returns a single string with the result of the execution.
    """
    return run_one_command(['hwloc-calc'] + prog_args).rstrip()

def fwriteln(fname, line):
    try:
        with open(fname, 'w') as f:
            f.write(line)
    except:
        print("Failed to write into {}: {}".format(fname, sys.exc_info()))

def fwriteln_and_log(fname, line):
    print("Writing '{}' to {}".format(line, fname))
    fwriteln(fname, line)

def set_one_mask(conf_file, mask):
    mask = re.sub('0x', '', mask)
    print("Setting mask {} in {}".format(mask, conf_file))
    fwriteln(conf_file, mask)

def distribute_irqs(irqs, cpu_mask):
    for i, mask in enumerate(run_hwloc_distrib(["{}".format(len(irqs)), '--single', '--restrict', cpu_mask])):
        set_one_mask("/proc/irq/{}/smp_affinity".format(irqs[i]), mask)

def is_process_running(name):
    return any([psutil.Process(pid).name() == name for pid in psutil.pids()])

def restart_irqbalance(banned_irqs):
    """
    Restart irqbalance if it's running and ban it from moving the IRQs from the
    given list.
    """
    config_file = '/etc/default/irqbalance'
    options_key = 'OPTIONS'
    systemd = False

    # return early if irqbalance is not running
    if not is_process_running('irqbalance'):
        print("irqbalance is not running")
        return

    if not os.path.exists(config_file):
        if os.path.exists('/etc/sysconfig/irqbalance'):
            config_file = '/etc/sysconfig/irqbalance'
            options_key = 'IRQBALANCE_ARGS'
            systemd = True
        else:
            print("Unknown system configuration - not restarting irqbalance!")
            print("You have to prevent it from moving IRQs {} manually!".format(banned_irqs))
            return

    orig_file = "{}.scylla.orig".format(config_file)

    # Save the original file
    if not os.path.exists(orig_file):
        shutil.copyfile(config_file, orig_file)

    # Read the config file lines
    cfile_lines = open(config_file, 'r').readlines()

    # Build the new config_file contents with the new options configuration
    print("Restarting irqbalance: going to ban the following IRQ numbers: ", end='')

    new_options = "{}=\"".format(options_key)
    for irq in banned_irqs:
        new_options += " --banirq={}".format(irq)
        print("{}".format(irq), end='')

    new_options += "\""

    print('...')
    print("Original irqbalance configuration is in {}".format(orig_file))

    with open(config_file, 'w') as cfile:
        for line in cfile_lines:
            if not re.search(options_key, line):
                cfile.write(line)

        cfile.write(new_options + "\n")

    if systemd:
        print("Restarting irqbalance via systemctl...")
        run_one_command(['systemctl', 'try-restart', 'irqbalance'])
    else:
        print("Restarting irqbalance directly (init.d)...")
        run_one_command(['/etc/init.d/irqbalance', 'restart'])
################################################################################
class NetPerfTuner:
    def __init__(self, args):
        self.__args = args
        self.__rfs_table_size = 32768
        self.__nic_is_bond_iface = self.__check_dev_is_bond_iface()
        self.__slaves = self.__learn_slaves()
        self.__irq2procline = {}
        for line in open('/proc/interrupts', 'r').readlines():
            self.__irq2procline[line.split(':')[0].lstrip().rstrip()] = line

        self.__nic2irqs = {}
        self.__learn_irqs()

#### Public methods ############################
    def tune(self):
        """
        Tune the networking server configuration.
        """
        if self.__dev_is_hw_iface(self.__args.nic):
            print("Setting a physical interface {}...".format(self.__args.nic))
            self.__setup_one_hw_iface(self.__args.nic)
        elif self.__dev_is_bond_iface():
            print("Setting {} bonding interface...".format(self.__args.nic))
            self.__setup_bonding_iface()
        else:
            sys.exit("Not supported virtual device {}".format(self.__args.nic))

        # Increase the socket listen() backlog
        fwriteln_and_log('/proc/sys/net/core/somaxconn', '4096')

        # Increase the maximum number of remembered connection requests, which are still
        # did not receive an acknowledgment from connecting client.
        fwriteln_and_log('/proc/sys/net/ipv4/tcp_max_syn_backlog', '4096')

    def get_cpu_mask(self):
        """
        Return the CPU mask to use for seastar application binding.
        """
        if self.__dev_is_bond_iface():
            return self.__gen_cpumask_bonding_iface()
        elif self.__dev_is_hw_iface(self.__args.nic):
            return self.__gen_cpumask_one_hw_iface(self.__args.nic)
        else:
            sys.exit("Not supported virtual device {}".format(self.__args.nic))

    def get_def_mq_mode(self, iface):
        """
        Returns the default configuration mode for the given interface.
        """
        num_irqs = len(self.__get_irqs_one(iface))
        rx_queues_count = len(self.__get_rps_cpus(iface))

        if rx_queues_count == 0:
            rx_queues_count = num_irqs

        num_cores = int(run_hwloc_calc(['--number-of', 'core', 'machine:0', '--restrict', self.__args.cpu_mask]))

        if num_cores > 4 * rx_queues_count:
            return 'sq'
        else:
            return 'mq'

    def get_irqs(self):
        """
        Returns the iterator for all IRQs that are going to be configured (according to args.nic parameter).
        For instance, for a bonding interface that's going to include IRQs of all its slaves.
        """
        return itertools.chain.from_iterable(self.__nic2irqs.values())

#### Private methods ############################
    def __get_irqs_one(self, iface):
        """
        Returns the list of IRQ numbers for the given interface.
        """
        return self.__nic2irqs[iface]

    def __setup_rfs(self, iface):
        rps_limits = glob.glob("/sys/class/net/{}/queues/*/rps_flow_cnt".format(iface))
        one_q_limit = int(self.__rfs_table_size / len(rps_limits))

        # If RFS feature is not present - get out
        try:
            run_one_command(['sysctl', 'net.core.rps_sock_flow_entries'])
        except:
            return

        # Enable RFS
        print("Setting net.core.rps_sock_flow_entries to {}".format(self.__rfs_table_size))
        run_one_command(['sysctl', '-w', 'net.core.rps_sock_flow_entries={}'.format(self.__rfs_table_size)])

        # Set each RPS queue limit
        for rfs_limit_cnt in rps_limits:
            print("Setting limit {} in {}".format(one_q_limit, rfs_limit_cnt))
            fwriteln(rfs_limit_cnt, "{}".format(one_q_limit))

        # Enable ntuple filtering HW offload on the NIC
        print("Trying to enable ntuple filtering HW offload for {}...".format(iface), end='')
        try:
            run_one_command(['ethtool','-K', iface, 'ntuple', 'on'], stderr=subprocess.DEVNULL)
            print("ok")
        except:
            print("not supported")

    def __setup_rps(self, iface, no_cpu0):
        if no_cpu0:
            mask = run_hwloc_calc([self.__args.cpu_mask, '~core:0'])
        else:
            mask = self.__args.cpu_mask

        for one_rps_cpus in self.__get_rps_cpus(iface):
            set_one_mask(one_rps_cpus, mask)

        self.__setup_rfs(iface)

    def __setup_xps(self, iface):
        xps_cpus_list = glob.glob("/sys/class/net/{}/queues/*/xps_cpus".format(iface))
        masks = run_hwloc_distrib(["{}".format(len(xps_cpus_list))])

        for i, mask in enumerate(masks):
            set_one_mask(xps_cpus_list[i], mask)

    def __dev_is_hw_iface(self, iface):
        return os.path.exists("/sys/class/net/{}/device".format(iface))

    def __check_dev_is_bond_iface(self):
        if not os.path.exists('/sys/class/net/bonding_masters'):
            return False

        return any([re.search(self.__args.nic, line) for line in open('/sys/class/net/bonding_masters', 'r').readlines()])

    def __dev_is_bond_iface(self):
        return self.__nic_is_bond_iface

    def __learn_slaves(self):
        slaves = []
        if self.__dev_is_bond_iface():
            for line in open("/sys/class/net/{}/bonding/slaves".format(self.__args.nic), 'r').readlines():
                slaves.extend(line.split())

        return slaves

    def __get_slaves(self):
        """
        Returns an iterator for all slaves of the args.nic.
        If agrs.nic is not a bonding interface an attempt to use the returned iterator
        will immediately raise a StopIteration exception - use __dev_is_bond_iface() check to avoid this.
        """
        return iter(self.__slaves)

    def __learn_irqs_from_proc_interrupts(self, pattern):
        irqs = []
        for irq, proc_line in self.__irq2procline.items():
            if re.search(pattern, proc_line):
                irqs.append(irq)

        return irqs

    def __learn_all_irqs_one(self, iface):
        msi_irqs_dir_name = "/sys/class/net/{}/device/msi_irqs".format(iface)
        # Device uses MSI IRQs
        if os.path.exists(msi_irqs_dir_name):
            return os.listdir(msi_irqs_dir_name)

        irqs = []
        irq_file_name = "/sys/class/net/{}/device/irq".format(iface)
        # Device uses INT#x
        if os.path.exists(irq_file_name):
            with open(irq_file_name, 'r') as irq_file:
                for line in irq_file:
                    irqs.append(line.lstrip().rstrip())

            return irqs

        # No irq file detected
        modalias = open("/sys/class/net/{}/device/modalias".format(iface), 'r').readline()

        # virtio case
        if re.search("^virtio", modalias):
            for (dirpath, dirnames, filenames) in os.walk("/sys/class/net/{}/device/driver".format(iface)):
                for dirname in dirnames:
                    if re.search('virtio', dirname):
                        irqs.extend(self.__learn_irqs_from_proc_interrupts(dirname))

            return irqs

        # xen case
        if re.search("^xen:vif", modalias):
            return self.__learn_irqs_from_proc_interrupts(iface)

    def __learn_irqs_one(self, iface):
        """
        This is a slow method that is going to read from the system files. Never
        use it outside the initialization code. Use __get_irqs_one() instead.

        Filter the fast path queues IRQs from the __get_all_irqs_one() result according to the known
        patterns.
        Right now we know about the following naming convention of the fast path queues vectors:
          - Intel:    <bla-bla>-TxRx-<bla-bla>
          - Broadcom: <bla-bla>-fp-<bla-bla>
          - ena:      <bla-bla>-Tx-Rx-<bla-bla>

        So, we will try to filter the etries in /proc/interrupts for IRQs we've got from get_all_irqs_one()
        according to the patterns above.

        If as a result all IRQs are filtered out (if there are no IRQs with the names from the patterns above) then
        this means that the given NIC uses a different IRQs naming pattern. In this case we won't filter any IRQ.

        Otherwise, we will use only IRQs which names fit one of the patterns above.
        """
        all_irqs = self.__learn_all_irqs_one(iface)
        fp_irqs_re = re.compile("\-TxRx\-|\-fp\-|\-Tx\-Rx\-")
        irqs = []
        for irq in all_irqs:
            if fp_irqs_re.search(self.__irq2procline[irq]):
                irqs.append(irq)

        if len(irqs) > 0:
            return irqs
        else:
            return all_irqs

    def __learn_irqs(self):
        """
        This is a slow method that is going to read from the system files. Never
        use it outside the initialization code.
        """
        if self.__dev_is_bond_iface():
            for slave in self.__get_slaves():
                if self.__dev_is_hw_iface(slave):
                    self.__nic2irqs[slave] = self.__learn_irqs_one(slave)
        else:
            self.__nic2irqs[self.__args.nic] = self.__learn_irqs_one(self.__args.nic)

    def __get_rps_cpus(self, iface):
        """
        Prints all rps_cpus files names for the given HW interface.

        There is a single rps_cpus file for each RPS queue and there is a single RPS
        queue for each HW Rx queue. Each HW Rx queue should have an IRQ.
        Therefore the number of these files is equal to the number of fast path Rx IRQs for this interface.
        """
        return glob.glob("/sys/class/net/{}/queues/*/rps_cpus".format(iface))

    def __setup_one_hw_iface(self, iface):
        if self.__args.mode:
            mq_mode = self.__args.mode
        else:
            mq_mode = self.get_def_mq_mode(iface)

        if mq_mode == 'sq':
            for irq in self.__nic2irqs[iface]:
                set_one_mask("/proc/irq/{}/smp_affinity".format(irq), '1')

            self.__setup_rps(iface, True)
        else: # mode == 'mq'
            distribute_irqs(self.__nic2irqs[iface], self.__args.cpu_mask)
            self.__setup_rps(iface, False)

        self.__setup_xps(iface)

    def __setup_bonding_iface(self):
        for slave in self.__get_slaves():
            if self.__dev_is_hw_iface(slave):
                print("Setting up {}...".format(slave))
                self.__setup_one_hw_iface(slave)
            else:
                print("Skipping {} (not a physical slave device?)".format(slave))

    def __gen_mode_cpu_mask(self, mq_mode):
        if mq_mode == 'sq':
            return run_hwloc_calc([self.__args.cpu_mask, '~core:0'])
        else:
            return self.__args.cpu_mask

    def __gen_cpumask_one_hw_iface(self, iface):
        if self.__args.mode:
            mq_mode = self.__args.mode
        else:
            mq_mode = self.get_def_mq_mode(iface)

        return self.__gen_mode_cpu_mask(mq_mode)

    def __gen_cpumask_bonding_iface(self):
        if self.__args.mode:
            return self.__gen_mode_cpu_mask(self.__args.mode)

        found_sq = any([self.__dev_is_hw_iface(slave) and self.get_def_mq_mode(slave) == 'sq' for slave in self.__get_slaves()])
        if found_sq:
            return self.__gen_mode_cpu_mask('sq')
        else:
            return self.__gen_mode_cpu_mask('mq')

################################################################################

argp = argparse.ArgumentParser(description = 'Configure various system parameters in order to improve the seastar application performance.', formatter_class=argparse.RawDescriptionHelpFormatter,
                               epilog=
'''
This script will:

    - Ban relevant IRQs from being moved by irqbalance.
    - Configure various system parameters in /proc/sys.
    - Distribute the IRQs (using SMP affinity configuration) among CPUs according to the configuration mode (see below).

As a result some of the CPUs may be destined to only handle the IRQs and taken out of the CPU set
that should be used to run the seastar application ("compute CPU set").

Modes description:

 sq - set all IRQs of a given NIC to CPU0 and configure RPS
      to spreads NAPIs' handling between other CPUs.

 mq - distribute NIC's IRQs among all CPUs instead of binding
      them all to CPU0. In this mode RPS is always enabled to
      spreads NAPIs' handling between all CPUs.

 If there isn't any mode given script will use a default mode:
    - If number of physical CPU cores per Rx HW queue is greater than 4 - use the 'sq' mode.
    - Otherwise use the 'mq' mode.

Default values:

 --nic NIC       - default: eth0
 --cpu-mask MASK - default: all available cores mask
''')
argp.add_argument('--mode', choices=['mq', 'sq'], help='configuration mode')
argp.add_argument('--nic', help='network interface name', default='eth0')
argp.add_argument('--get-cpu-mask', action='store_true', help="print the CPU mask to be used for compute")
argp.add_argument('--cpu-mask', help="mask of cores to use, by default use all available cores", default=run_hwloc_calc(['all']), metavar='MASK')
################################################################################

args = argp.parse_args()

# Don't let the cpu_mask have bits outside the CPU set of this machine
args.cpu_mask = run_hwloc_calc(['--restrict', args.cpu_mask, 'all'])

net_perf_tuner = NetPerfTuner(args)

if args.get_cpu_mask:
    print(net_perf_tuner.get_cpu_mask())
else:
    # Ban irqbalance from moving NICs IRQs
    restart_irqbalance(net_perf_tuner.get_irqs())
    # Tune the networking
    net_perf_tuner.tune()



