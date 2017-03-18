#!/usr/bin/python3

import abc
import argparse
import enum
import functools
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
    banned_irqs_list = list(banned_irqs)

    # If there is nothing to ban - quit
    if len(banned_irqs_list) == 0:
        return

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
            print("You have to prevent it from moving IRQs {} manually!".format(banned_irqs_list))
            return

    orig_file = "{}.scylla.orig".format(config_file)

    # Save the original file
    if not os.path.exists(orig_file):
        print("Saving the original irqbalance configuration is in {}".format(orig_file))
        shutil.copyfile(config_file, orig_file)
    else:
        print("File {} already exists - not overwriting.".format(orig_file))

    # Read the config file lines
    cfile_lines = open(config_file, 'r').readlines()

    # Build the new config_file contents with the new options configuration
    print("Restarting irqbalance: going to ban the following IRQ numbers: {} ...".format(", ".join(banned_irqs_list)))

    # Search for the original options line
    opt_lines = list(filter(lambda line : re.search("^\s*{}".format(options_key), line), cfile_lines))
    if len(opt_lines) == 0:
        new_options = "{}=\"".format(options_key)
    elif len(opt_lines) == 1:
        # cut the last "
        new_options = re.sub("\"\s*$", "", opt_lines[0].rstrip())
    else:
        raise Exception("Invalid format in {}: more than one lines with {} key".format(config_file, options_key))

    for irq in banned_irqs_list:
        # prevent duplicate "ban" entries for the same IRQ
        patt_str = "\-\-banirq\={}\Z|\-\-banirq\={}\s".format(irq, irq)
        if not re.search(patt_str, new_options):
            new_options += " --banirq={}".format(irq)

    new_options += "\""

    with open(config_file, 'w') as cfile:
        for line in cfile_lines:
            if not re.search("^\s*{}".format(options_key), line):
                cfile.write(line)

        cfile.write(new_options + "\n")

    if systemd:
        print("Restarting irqbalance via systemctl...")
        run_one_command(['systemctl', 'try-restart', 'irqbalance'])
    else:
        print("Restarting irqbalance directly (init.d)...")
        run_one_command(['/etc/init.d/irqbalance', 'restart'])

def learn_irqs_from_proc_interrupts(pattern, irq2procline):
    return [ irq for irq, proc_line in filter(lambda irq_proc_line_pair : re.search(pattern, irq_proc_line_pair[1]), irq2procline.items()) ]

def learn_all_irqs_one(irq_conf_dir, irq2procline, xen_dev_name):
    """
    Returns a list of IRQs of a single device.

    irq_conf_dir: a /sys/... directory with the IRQ information for the given device
    irq2procline: a map of IRQs to the corresponding lines in the /proc/interrupts
    xen_dev_name: a device name pattern as it appears in the /proc/interrupts on Xen systems
    """
    msi_irqs_dir_name = os.path.join(irq_conf_dir, 'msi_irqs')
    # Device uses MSI IRQs
    if os.path.exists(msi_irqs_dir_name):
        return os.listdir(msi_irqs_dir_name)

    irq_file_name = os.path.join(irq_conf_dir, 'irq')
    # Device uses INT#x
    if os.path.exists(irq_file_name):
        return [ line.lstrip().rstrip() for line in open(irq_file_name, 'r').readlines() ]

    # No irq file detected
    modalias = open(os.path.join(irq_conf_dir, 'modalias'), 'r').readline()

    # virtio case
    if re.search("^virtio", modalias):
        return list(itertools.chain.from_iterable(
            map(lambda dirname : learn_irqs_from_proc_interrupts(dirname, irq2procline),
                filter(lambda dirname : re.search('virtio', dirname),
                       itertools.chain.from_iterable([ dirnames for dirpath, dirnames, filenames in os.walk(os.path.join(irq_conf_dir, 'driver')) ])))))

    # xen case
    if re.search("^xen:", modalias):
        return learn_irqs_from_proc_interrupts(xen_dev_name, irq2procline)

def get_irqs2procline_map():
    return { line.split(':')[0].lstrip().rstrip() : line for line in open('/proc/interrupts', 'r').readlines() }

################################################################################
class PerfTunerBase(metaclass=abc.ABCMeta):
    def __init__(self, args):
        self.__args = args
        self.__args.cpu_mask = run_hwloc_calc(['--restrict', self.__args.cpu_mask, 'all'])
        self.__mode = None
        self.__compute_cpu_mask = None
        self.__irq_cpu_mask = None

#### Public methods ##########################
    class SupportedModes(enum.IntEnum):
        """
        Modes are ordered from the one that cuts the biggest number of CPUs
        from the compute CPUs' set to the one that takes the smallest ('mq' doesn't
        cut any CPU from the compute set).

        This fact is used when we calculate the 'common quotient' mode out of a
        given set of modes (e.g. default modes of different Tuners) - this would
        be the smallest among the given modes.
        """
        sq_split = 0
        sq = 1
        mq = 2

        @staticmethod
        def names():
            return PerfTunerBase.SupportedModes.__members__.keys()

    @staticmethod
    def compute_cpu_mask_for_mode(mq_mode, cpu_mask):
        mq_mode = PerfTunerBase.SupportedModes(mq_mode)

        if mq_mode == PerfTunerBase.SupportedModes.sq:
            # all but CPU0
            return run_hwloc_calc([cpu_mask, '~PU:0'])
        elif mq_mode == PerfTunerBase.SupportedModes.sq_split:
            # all but CPU0 and its HT siblings
            return run_hwloc_calc([cpu_mask, '~core:0'])
        elif mq_mode == PerfTunerBase.SupportedModes.mq:
            # all available cores
            return cpu_mask
        else:
            raise Exception("Unsupported mode: {}".format(mq_mode))

    @staticmethod
    def irqs_cpu_mask_for_mode(mq_mode, cpu_mask):
        mq_mode = PerfTunerBase.SupportedModes(mq_mode)

        if mq_mode != PerfTunerBase.SupportedModes.mq:
            return run_hwloc_calc([cpu_mask, "~{}".format(PerfTunerBase.compute_cpu_mask_for_mode(mq_mode, cpu_mask))])
        else: # mq_mode == PerfTunerBase.SupportedModes.mq
            # distribute equally between all available cores
            return cpu_mask

    @property
    def mode(self):
        """
        Return the configuration mode
        """
        # Make sure the configuration mode is set (see the __set_mode_and_masks() description).
        if self.__mode is None:
            self.__set_mode_and_masks()

        return self.__mode

    @mode.setter
    def mode(self, new_mode):
        """
        Set the new configuration mode and recalculate the corresponding masks.
        """
        # Make sure the new_mode is of PerfTunerBase.AllowedModes type
        self.__mode = PerfTunerBase.SupportedModes(new_mode)
        self.__compute_cpu_mask = PerfTunerBase.compute_cpu_mask_for_mode(self.__mode, self.__args.cpu_mask)
        self.__irq_cpu_mask = PerfTunerBase.irqs_cpu_mask_for_mode(self.__mode, self.__args.cpu_mask)

    @property
    def compute_cpu_mask(self):
        """
        Return the CPU mask to use for seastar application binding.
        """
        # see the __set_mode_and_masks() description
        if self.__compute_cpu_mask is None:
            self.__set_mode_and_masks()

        return self.__compute_cpu_mask

    @property
    def irqs_cpu_mask(self):
        """
        Return the mask of CPUs used for IRQs distribution.
        """
        # see the __set_mode_and_masks() description
        if self.__irq_cpu_mask is None:
            self.__set_mode_and_masks()

        return self.__irq_cpu_mask

    @property
    def args(self):
        return self.__args

    @property
    def irqs(self):
        return self._get_irqs()

#### "Protected"/Public (pure virtual) methods ###########
    @abc.abstractmethod
    def tune(self):
        pass

    @abc.abstractmethod
    def _get_def_mode(self):
        """
        Return a default configuration mode.
        """
        pass

    @abc.abstractmethod
    def _get_irqs(self):
        """
        Return the iteratable value with all IRQs to be configured.
        """
        pass

#### Private methods ############################
    def __set_mode_and_masks(self):
        """
        Sets the configuration mode and the corresponding CPU masks. We can't
        initialize them in the constructor because the default mode may depend
        on the child-specific values that are set in its constructor.

        That's why we postpone the mode's and the corresponding masks'
        initialization till after the child instance creation.
        """
        if self.__args.mode:
            self.mode = PerfTunerBase.SupportedModes[self.__args.mode]
        else:
            self.mode = self._get_def_mode()

#################################################
class NetPerfTuner(PerfTunerBase):
    def __init__(self, args):
        super().__init__(args)

        self.__nic_is_bond_iface = self.__check_dev_is_bond_iface()
        self.__slaves = self.__learn_slaves()

        # check that self.nic is either a HW device or a bonding interface
        self.__check_nic()

        self.__nic2irqs = self.__learn_irqs()

#### Public methods ############################
    def tune(self):
        """
        Tune the networking server configuration.
        """
        if self.nic_is_hw_iface:
            print("Setting a physical interface {}...".format(self.nic))
            self.__setup_one_hw_iface(self.nic)
        else:
            print("Setting {} bonding interface...".format(self.nic))
            self.__setup_bonding_iface()

        # Increase the socket listen() backlog
        fwriteln_and_log('/proc/sys/net/core/somaxconn', '4096')

        # Increase the maximum number of remembered connection requests, which are still
        # did not receive an acknowledgment from connecting client.
        fwriteln_and_log('/proc/sys/net/ipv4/tcp_max_syn_backlog', '4096')

    @property
    def nic_is_bond_iface(self):
        return self.__nic_is_bond_iface

    @property
    def nic(self):
        return self.args.nic

    @property
    def nic_is_hw_iface(self):
        return self.__dev_is_hw_iface(self.nic)

    @property
    def slaves(self):
        """
        Returns an iterator for all slaves of the args.nic.
        If agrs.nic is not a bonding interface an attempt to use the returned iterator
        will immediately raise a StopIteration exception - use __dev_is_bond_iface() check to avoid this.
        """
        return iter(self.__slaves)

#### Protected methods ##########################
    def _get_def_mode(self):
        if self.nic_is_bond_iface:
            return min(map(self.__get_hw_iface_def_mode, filter(self.__dev_is_hw_iface, self.slaves)))
        else:
            return self.__get_hw_iface_def_mode(self.nic)

    def _get_irqs(self):
        """
        Returns the iterator for all IRQs that are going to be configured (according to args.nic parameter).
        For instance, for a bonding interface that's going to include IRQs of all its slaves.
        """
        return itertools.chain.from_iterable(self.__nic2irqs.values())

#### Private methods ############################
    @property
    def __rfs_table_size(self):
        return 32768

    def __check_nic(self):
        """
        Checks that self.nic is a supported interface
        """
        if not self.nic_is_hw_iface and not self.nic_is_bond_iface:
            raise Exception("Not supported virtual device {}".format(self.nic))

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

    def __setup_rps(self, iface, mask):
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

        return any([re.search(self.nic, line) for line in open('/sys/class/net/bonding_masters', 'r').readlines()])

    def __learn_slaves(self):
        if self.nic_is_bond_iface:
            return list(itertools.chain.from_iterable([ line.split() for line in open("/sys/class/net/{}/bonding/slaves".format(self.nic), 'r').readlines() ]))

        return []

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
        irqs2procline = get_irqs2procline_map()
        all_irqs = learn_all_irqs_one("/sys/class/net/{}/device".format(iface), irqs2procline, iface)
        fp_irqs_re = re.compile("\-TxRx\-|\-fp\-|\-Tx\-Rx\-")
        irqs = list(filter(lambda irq : fp_irqs_re.search(irqs2procline[irq]), all_irqs))
        if len(irqs) > 0:
            return irqs
        else:
            return all_irqs

    def __learn_irqs(self):
        """
        This is a slow method that is going to read from the system files. Never
        use it outside the initialization code.
        """
        if self.nic_is_bond_iface:
            return { slave : self.__learn_irqs_one(slave) for slave in filter(self.__dev_is_hw_iface, self.slaves) }
        else:
            return { self.nic : self.__learn_irqs_one(self.nic) }

    def __get_rps_cpus(self, iface):
        """
        Prints all rps_cpus files names for the given HW interface.

        There is a single rps_cpus file for each RPS queue and there is a single RPS
        queue for each HW Rx queue. Each HW Rx queue should have an IRQ.
        Therefore the number of these files is equal to the number of fast path Rx IRQs for this interface.
        """
        return glob.glob("/sys/class/net/{}/queues/*/rps_cpus".format(iface))

    def __setup_one_hw_iface(self, iface):
        # Bind the NIC's IRQs according to the configuration mode
        distribute_irqs(self.__get_irqs_one(iface), self.irqs_cpu_mask)

        self.__setup_rps(iface, self.compute_cpu_mask)
        self.__setup_xps(iface)

    def __setup_bonding_iface(self):
        for slave in self.slaves:
            if self.__dev_is_hw_iface(slave):
                print("Setting up {}...".format(slave))
                self.__setup_one_hw_iface(slave)
            else:
                print("Skipping {} (not a physical slave device?)".format(slave))

    def __get_hw_iface_def_mode(self, iface):
        """
        Returns the default configuration mode for the given interface.
        """
        num_irqs = len(self.__get_irqs_one(iface))
        rx_queues_count = len(self.__get_rps_cpus(iface))

        if rx_queues_count == 0:
            rx_queues_count = num_irqs

        num_cores = int(run_hwloc_calc(['--number-of', 'core', 'machine:0', '--restrict', self.args.cpu_mask]))
        num_PUs = int(run_hwloc_calc(['--number-of', 'PU', 'machine:0', '--restrict', self.args.cpu_mask]))

        if num_cores > 4 * rx_queues_count:
            return PerfTunerBase.SupportedModes.sq_split
        elif num_PUs > 4 * rx_queues_count:
            return PerfTunerBase.SupportedModes.sq
        else:
            return PerfTunerBase.SupportedModes.mq

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

 sq_split - divide all IRQs of a given NIC between CPU0 and its HT siblings and configure RPS
      to spreads NAPIs' handling between other CPUs.

 mq - distribute NIC's IRQs among all CPUs instead of binding
      them all to CPU0. In this mode RPS is always enabled to
      spreads NAPIs' handling between all CPUs.

 If there isn't any mode given script will use a default mode:
    - If number of physical CPU cores per Rx HW queue is greater than 4 - use the 'sq-split' mode.
    - Otherwise, if number of hyperthreads per Rx HW queue is greater than 4 - use the 'sq' mode.
    - Otherwise use the 'mq' mode.

Default values:

 --nic NIC       - default: eth0
 --cpu-mask MASK - default: all available cores mask
''')
argp.add_argument('--mode', choices=PerfTunerBase.SupportedModes.names(), help='configuration mode')
argp.add_argument('--nic', help='network interface name', default='eth0')
argp.add_argument('--get-cpu-mask', action='store_true', help="print the CPU mask to be used for compute")
argp.add_argument('--cpu-mask', help="mask of cores to use, by default use all available cores", default=run_hwloc_calc(['all']), metavar='MASK')
################################################################################

args = argp.parse_args()

try:
    net_perf_tuner = NetPerfTuner(args)

    if args.get_cpu_mask:
        print(net_perf_tuner.compute_cpu_mask)
    else:
        # Ban irqbalance from moving NICs IRQs
        restart_irqbalance(net_perf_tuner.irqs)

        # Tune the networking
        net_perf_tuner.tune()
except Exception as e:
    sys.exit("ERROR: {}. Your system can't be tuned until the issue is fixed.".format(e))


