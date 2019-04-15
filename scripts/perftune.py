#!/usr/bin/python3

import abc
import argparse
import enum
import functools
import glob
import itertools
import logging
import multiprocessing
import os
import pathlib
import pyudev
import re
import shutil
import subprocess
import sys
import urllib.request
import yaml

def run_one_command(prog_args, my_stderr=None, check=True):
    proc = subprocess.Popen(prog_args, stdout = subprocess.PIPE, stderr = my_stderr)
    outs, errs = proc.communicate()
    outs = str(outs, 'utf-8')

    if check and proc.returncode != 0:
        raise subprocess.CalledProcessError(returncode=proc.returncode, cmd=" ".join(prog_args), output=outs, stderr=errs)

    return outs

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

def fwriteln(fname, line, log_message, log_errors=True):
    try:
        with open(fname, 'w') as f:
            f.write(line)

        print(log_message)
    except:
        if log_errors:
            print("{}: failed to write into {}: {}".format(log_message, fname, sys.exc_info()))

def readlines(fname):
    try:
        with open(fname, 'r') as f:
            return f.readlines()
    except:
        print("Failed to read {}: {}".format(fname, sys.exc_info()))
        return []

def fwriteln_and_log(fname, line, log_errors=True):
    msg = "Writing '{}' to {}".format(line, fname)
    fwriteln(fname, line, log_message=msg, log_errors=log_errors)

double_commas_pattern = re.compile(',,')

def set_one_mask(conf_file, mask, log_errors=True):
    if not os.path.exists(conf_file):
        raise Exception("Configure file to set mask doesn't exist: {}".format(conf_file))
    mask = re.sub('0x', '', mask)

    while double_commas_pattern.search(mask):
        mask = double_commas_pattern.sub(',0,', mask)

    msg = "Setting mask {} in {}".format(mask, conf_file)
    fwriteln(conf_file, mask, log_message=msg, log_errors=log_errors)

def distribute_irqs(irqs, cpu_mask, log_errors=True):
    # If IRQs' list is empty - do nothing
    if not irqs:
        return

    for i, mask in enumerate(run_hwloc_distrib(["{}".format(len(irqs)), '--single', '--restrict', cpu_mask])):
        set_one_mask("/proc/irq/{}/smp_affinity".format(irqs[i]), mask, log_errors=log_errors)

def is_process_running(name):
    return len(list(filter(lambda ps_line : not re.search('<defunct>', ps_line), run_one_command(['ps', '--no-headers', '-C', name], check=False).splitlines()))) > 0

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
    if not banned_irqs_list:
        return

    # return early if irqbalance is not running
    if not is_process_running('irqbalance'):
        print("irqbalance is not running")
        return

    # If this file exists - this a "new (systemd) style" irqbalance packaging.
    # This type of packaging uses IRQBALANCE_ARGS as an option key name, "old (init.d) style"
    # packaging uses an OPTION key.
    if os.path.exists('/lib/systemd/system/irqbalance.service'):
        options_key = 'IRQBALANCE_ARGS'
        systemd = True

    if not os.path.exists(config_file):
        if os.path.exists('/etc/sysconfig/irqbalance'):
            config_file = '/etc/sysconfig/irqbalance'
        elif os.path.exists('/etc/conf.d/irqbalance'):
            config_file = '/etc/conf.d/irqbalance'
            options_key = 'IRQBALANCE_OPTS'
            with open('/proc/1/comm', 'r') as comm:
                systemd = 'systemd' in comm.read()
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
    if not opt_lines:
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

    return []

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
        self.__is_aws_i3_nonmetal_instance = None

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
    def cpu_mask_is_zero(cpu_mask):
        """
        The irqs_cpu_mask is a coma-separated list of 32-bit hex values, e.g. 0xffff,0x0,0xffff
        We want to estimate if the whole mask is all-zeros.
        :param cpu_mask: hwloc-calc generated CPU mask
        :return: True if mask is zero, False otherwise
        """
        for cur_irqs_cpu_mask in cpu_mask.split(','):
            if int(cur_irqs_cpu_mask, 16) != 0:
                return False

        return True

    @staticmethod
    def compute_cpu_mask_for_mode(mq_mode, cpu_mask):
        mq_mode = PerfTunerBase.SupportedModes(mq_mode)
        irqs_cpu_mask = 0

        if mq_mode == PerfTunerBase.SupportedModes.sq:
            # all but CPU0
            irqs_cpu_mask = run_hwloc_calc([cpu_mask, '~PU:0'])
        elif mq_mode == PerfTunerBase.SupportedModes.sq_split:
            # all but CPU0 and its HT siblings
            irqs_cpu_mask = run_hwloc_calc([cpu_mask, '~core:0'])
        elif mq_mode == PerfTunerBase.SupportedModes.mq:
            # all available cores
            irqs_cpu_mask = cpu_mask
        else:
            raise Exception("Unsupported mode: {}".format(mq_mode))

        if PerfTunerBase.cpu_mask_is_zero(irqs_cpu_mask):
            raise Exception("Bad configuration mode ({}) and cpu-mask value ({}): this results in a zero-mask for "
                            "compute".format(mq_mode.name, cpu_mask))

        return irqs_cpu_mask

    @staticmethod
    def irqs_cpu_mask_for_mode(mq_mode, cpu_mask):
        mq_mode = PerfTunerBase.SupportedModes(mq_mode)
        irqs_cpu_mask = 0

        if mq_mode != PerfTunerBase.SupportedModes.mq:
            irqs_cpu_mask = run_hwloc_calc([cpu_mask, "~{}".format(PerfTunerBase.compute_cpu_mask_for_mode(mq_mode, cpu_mask))])
        else: # mq_mode == PerfTunerBase.SupportedModes.mq
            # distribute equally between all available cores
            irqs_cpu_mask = cpu_mask

        if PerfTunerBase.cpu_mask_is_zero(irqs_cpu_mask):
            raise Exception("Bad configuration mode ({}) and cpu-mask value ({}): this results in a zero-mask for "
                            "IRQs".format(mq_mode.name, cpu_mask))

        return irqs_cpu_mask

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
    def is_aws_i3_non_metal_instance(self):
        """
        :return: True if we are running on the AWS i3.nonmetal instance, e.g. i3.4xlarge
        """
        if self.__is_aws_i3_nonmetal_instance is None:
            self.__check_host_type()

        return self.__is_aws_i3_nonmetal_instance

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

    def __check_host_type(self):
        """
        Check if we are running on the AWS i3 nonmetal instance.
        If yes, set self.__is_aws_i3_nonmetal_instance to True, and to False otherwise.
        """
        try:
            aws_instance_type = urllib.request.urlopen("http://169.254.169.254/latest/meta-data/instance-type", timeout=0.1).read().decode()
            if re.match(r'^i3\.(\w(?!metal))+$', aws_instance_type):
                self.__is_aws_i3_nonmetal_instance = True
            else:
                self.__is_aws_i3_nonmetal_instance = False

            return
        except (urllib.error.URLError, ConnectionError, TimeoutError):
            # Non-AWS case
            pass
        except:
            logging.warning("Unexpected exception while attempting to access AWS meta server: {}".format(sys.exc_info()[0]))

        self.__is_aws_i3_nonmetal_instance = False

#################################################
class NetPerfTuner(PerfTunerBase):
    def __init__(self, args):
        super().__init__(args)

        self.__nic_is_bond_iface = self.__check_dev_is_bond_iface()
        self.__slaves = self.__learn_slaves()

        # check that self.nic is either a HW device or a bonding interface
        self.__check_nic()

        self.__irqs2procline = get_irqs2procline_map()
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
            msg = "Setting limit {} in {}".format(one_q_limit, rfs_limit_cnt)
            fwriteln(rfs_limit_cnt, "{}".format(one_q_limit), log_message=msg)

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

    def __intel_irq_to_queue_idx(self, irq):
        """
        Return the HW queue index for a given IRQ for Intel NICs in order to sort the IRQs' list by this index.

        Intel's fast path IRQs have the following name convention:
             <bla-bla>-TxRx-<queue index>

        Intel NICs also have the IRQ for Flow Director (which is not a regular fast path IRQ) which name looks like
        this:
             <bla-bla>:fdir-TxRx-<index>

        We want to put the Flow Director's IRQ at the end of the sorted list of IRQs.

        :param irq: IRQ number
        :return: HW queue index for Intel NICs and 0 for all other NICs
        """
        intel_fp_irq_re = re.compile("\-TxRx\-(\d+)")
        fdir_re = re.compile("fdir\-TxRx\-\d+")

        m = intel_fp_irq_re.search(self.__irqs2procline[irq])
        m1 = fdir_re.search(self.__irqs2procline[irq])
        if m and not m1:
            return int(m.group(1))
        else:
            return sys.maxsize

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
          - Mellanox: mlx<device model index>-<queue idx>@<bla-bla>

        So, we will try to filter the etries in /proc/interrupts for IRQs we've got from get_all_irqs_one()
        according to the patterns above.

        If as a result all IRQs are filtered out (if there are no IRQs with the names from the patterns above) then
        this means that the given NIC uses a different IRQs naming pattern. In this case we won't filter any IRQ.

        Otherwise, we will use only IRQs which names fit one of the patterns above.

        For NICs with a limited number of Rx queues the IRQs that handle Rx are going to be at the beginning of the
        list.
        """
        # filter 'all_irqs' to only reference valid keys from 'irqs2procline' and avoid an IndexError on the 'irqs' search below
        all_irqs = set(learn_all_irqs_one("/sys/class/net/{}/device".format(iface), self.__irqs2procline, iface)).intersection(self.__irqs2procline.keys())
        fp_irqs_re = re.compile("\-TxRx\-|\-fp\-|\-Tx\-Rx\-|mlx\d+\-\d+@")
        irqs = list(filter(lambda irq : fp_irqs_re.search(self.__irqs2procline[irq]), all_irqs))
        if irqs:
            irqs.sort(key=self.__intel_irq_to_queue_idx)
            return irqs
        else:
            return list(all_irqs)

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
        max_num_rx_queues = self.__max_rx_queue_count(iface)
        all_irqs = self.__get_irqs_one(iface)

        # Bind the NIC's IRQs according to the configuration mode
        #
        # If this NIC has a limited number of Rx queues then we want to distribute their IRQs separately.
        # For such NICs we've sorted IRQs list so that IRQs that handle Rx are all at the head of the list.
        if max_num_rx_queues < len(all_irqs):
            num_rx_queues = self.__get_rx_queue_count(iface)
            print("Distributing IRQs handling Rx:")
            distribute_irqs(all_irqs[0:num_rx_queues], self.irqs_cpu_mask)
            print("Distributing the rest of IRQs")
            distribute_irqs(all_irqs[num_rx_queues:], self.irqs_cpu_mask)
        else:
            print("Distributing all IRQs")
            distribute_irqs(all_irqs, self.irqs_cpu_mask)

        self.__setup_rps(iface, self.compute_cpu_mask)
        self.__setup_xps(iface)

    def __setup_bonding_iface(self):
        for slave in self.slaves:
            if self.__dev_is_hw_iface(slave):
                print("Setting up {}...".format(slave))
                self.__setup_one_hw_iface(slave)
            else:
                print("Skipping {} (not a physical slave device?)".format(slave))

    def __max_rx_queue_count(self, iface):
        """
        :param iface: Interface to check
        :return: The maximum number of RSS queues for the given interface if there is known limitation and sys.maxsize
        otherwise.

        Networking drivers serving HW with the known maximum RSS queue limitation (due to lack of RSS bits):

        ixgbe:   PF NICs support up to 16 RSS queues.
        ixgbevf: VF NICs support up to 4 RSS queues.
        i40e:    PF NICs support up to 64 RSS queues.
        i40evf:  VF NICs support up to 16 RSS queues.

        """
        driver_to_max_rss = {'ixgbe': 16, 'ixgbevf': 4, 'i40e': 64, 'i40evf': 16}

        driver_name = ''
        ethtool_i_lines = run_one_command(['ethtool', '-i', iface]).splitlines()
        driver_re = re.compile("driver:")
        driver_lines = list(filter(lambda one_line: driver_re.search(one_line), ethtool_i_lines))

        if driver_lines:
            if len(driver_lines) > 1:
                raise Exception("More than one 'driver:' entries in the 'ethtool -i {}' output. Unable to continue.".format(iface))

            driver_name = driver_lines[0].split()[1].strip()

        return driver_to_max_rss.get(driver_name, sys.maxsize)

    def __get_rx_queue_count(self, iface):
        """
        :return: the RSS Rx queues count for the given interface.
        """
        num_irqs = len(self.__get_irqs_one(iface))
        rx_queues_count = len(self.__get_rps_cpus(iface))

        if rx_queues_count == 0:
            rx_queues_count = num_irqs

        return min(self.__max_rx_queue_count(iface), rx_queues_count)

    def __get_hw_iface_def_mode(self, iface):
        """
        Returns the default configuration mode for the given interface.
        """
        rx_queues_count = self.__get_rx_queue_count(iface)

        num_cores = int(run_hwloc_calc(['--number-of', 'core', 'machine:0', '--restrict', self.args.cpu_mask]))
        num_PUs = int(run_hwloc_calc(['--number-of', 'PU', 'machine:0', '--restrict', self.args.cpu_mask]))

        if num_PUs <= 4 or rx_queues_count == num_PUs:
            return PerfTunerBase.SupportedModes.mq
        elif num_cores <= 4:
            return PerfTunerBase.SupportedModes.sq
        else:
            return PerfTunerBase.SupportedModes.sq_split

#################################################
class DiskPerfTuner(PerfTunerBase):
    class SupportedDiskTypes(enum.IntEnum):
        nvme = 0
        non_nvme = 1

    def __init__(self, args):
        super().__init__(args)

        if not (self.args.dirs or self.args.devs):
            raise Exception("'disks' tuning was requested but neither directories nor storage devices were given")

        self.__pyudev_ctx = pyudev.Context()
        self.__dir2disks = self.__learn_directories()
        self.__irqs2procline = get_irqs2procline_map()
        self.__disk2irqs = self.__learn_irqs()
        self.__type2diskinfo = self.__group_disks_info_by_type()

        # sets of devices that have already been tuned
        self.__io_scheduler_tuned_devs = set()
        self.__nomerges_tuned_devs = set()

#### Public methods #############################
    def tune(self):
        """
        Distribute IRQs according to the requested mode (args.mode):
           - Distribute NVMe disks' IRQs equally among all available CPUs.
           - Distribute non-NVMe disks' IRQs equally among designated CPUs or among
             all available CPUs in the 'mq' mode.
        """
        mode_cpu_mask = PerfTunerBase.irqs_cpu_mask_for_mode(self.mode, self.args.cpu_mask)

        non_nvme_disks, non_nvme_irqs = self.__disks_info_by_type(DiskPerfTuner.SupportedDiskTypes.non_nvme)
        if non_nvme_disks:
            print("Setting non-NVMe disks: {}...".format(", ".join(non_nvme_disks)))
            distribute_irqs(non_nvme_irqs, mode_cpu_mask)
            self.__tune_disks(non_nvme_disks)
        else:
            print("No non-NVMe disks to tune")

        nvme_disks, nvme_irqs = self.__disks_info_by_type(DiskPerfTuner.SupportedDiskTypes.nvme)
        if nvme_disks:
            print("Setting NVMe disks: {}...".format(", ".join(nvme_disks)))
            distribute_irqs(nvme_irqs, self.args.cpu_mask,
                            log_errors=(self.is_aws_i3_non_metal_instance or self.args.verbose))
            self.__tune_disks(nvme_disks)
        else:
            print("No NVMe disks to tune")

#### Protected methods ##########################
    def _get_def_mode(self):
        """
        Return a default configuration mode.
        """
        # if the only disks we are tuning are NVMe disks - return the MQ mode
        non_nvme_disks, non_nvme_irqs = self.__disks_info_by_type(DiskPerfTuner.SupportedDiskTypes.non_nvme)
        if not non_nvme_disks:
            return PerfTunerBase.SupportedModes.mq

        num_cores = int(run_hwloc_calc(['--number-of', 'core', 'machine:0', '--restrict', self.args.cpu_mask]))
        num_PUs = int(run_hwloc_calc(['--number-of', 'PU', 'machine:0', '--restrict', self.args.cpu_mask]))
        if num_PUs <= 4:
            return PerfTunerBase.SupportedModes.mq
        elif num_cores <= 4:
            return PerfTunerBase.SupportedModes.sq
        else:
            return PerfTunerBase.SupportedModes.sq_split

    def _get_irqs(self):
        return itertools.chain.from_iterable(irqs for disks, irqs in self.__type2diskinfo.values())

#### Private methods ############################
    @property
    def __io_schedulers(self):
        """
        :return: An ordered list of IO schedulers that we want to configure. Schedulers are ordered by their priority
        from the highest (left most) to the lowest.
        """
        return ["none", "noop"]

    @property
    def __nomerges(self):
        return '2'

    def __disks_info_by_type(self, disks_type):
        """
        Returns a tuple ( [<disks>], [<irqs>] ) for the given disks type.
        IRQs numbers in the second list are promised to be unique.
        """
        return self.__type2diskinfo[DiskPerfTuner.SupportedDiskTypes(disks_type)]

    def __nvme_fast_path_irq_filter(self, irq):
        """
        Return True for fast path NVMe IRQs.
        For NVMe device only queues 1-<number of CPUs> are going to do fast path work.

        NVMe IRQs have the following name convention:
             nvme<device index>q<queue index>, e.g. nvme0q7

        :param irq: IRQ number
        :return: True if this IRQ is an IRQ of a FP NVMe queue.
        """
        nvme_irq_re = re.compile(r'(\s|^)nvme\d+q(\d+)(\s|$)')

        # There may be more than an single HW queue bound to the same IRQ. In this case queue names are going to be
        # coma separated
        split_line = self.__irqs2procline[irq].split(",")

        for line in split_line:
            m = nvme_irq_re.search(line)
            if m and 0 < int(m.group(2)) <= multiprocessing.cpu_count():
                return True

        return False

    def __group_disks_info_by_type(self):
        """
        Return a map of tuples ( [<disks>], [<irqs>] ), where "disks" are all disks of the specific type
        and "irqs" are the corresponding IRQs.

        It's promised that every element is "disks" and "irqs" is unique.

        The disk types are 'nvme' and 'non-nvme'
        """
        disks_info_by_type = {}
        nvme_disks = set()
        nvme_irqs = set()
        non_nvme_disks = set()
        non_nvme_irqs = set()
        nvme_disk_name_pattern = re.compile('^nvme')

        for disk, irqs in self.__disk2irqs.items():
            if nvme_disk_name_pattern.search(disk):
                nvme_disks.add(disk)
                for irq in irqs:
                    nvme_irqs.add(irq)
            else:
                non_nvme_disks.add(disk)
                for irq in irqs:
                    non_nvme_irqs.add(irq)

        if not (nvme_disks or non_nvme_disks):
            raise Exception("'disks' tuning was requested but no disks were found")

        nvme_irqs = list(nvme_irqs)

        # There is a known issue with Xen hypervisor that exposes itself on AWS i3 instances where nvme module
        # over-allocates HW queues and uses only queues 1,2,3,..., <up to number of CPUs> for data transfer.
        # On these instances we will distribute only these queues.

        if self.is_aws_i3_non_metal_instance:
            nvme_irqs = list(filter(self.__nvme_fast_path_irq_filter, nvme_irqs))

        # Sort IRQs for easier verification
        nvme_irqs.sort(key=lambda irq_num_str: int(irq_num_str))

        disks_info_by_type[DiskPerfTuner.SupportedDiskTypes.nvme] = (list(nvme_disks), nvme_irqs)
        disks_info_by_type[DiskPerfTuner.SupportedDiskTypes.non_nvme] = ( list(non_nvme_disks), list(non_nvme_irqs) )

        return disks_info_by_type

    def __learn_directories(self):
        return { directory : self.__learn_directory(directory) for directory in self.args.dirs }

    def __learn_directory(self, directory, recur=False):
        """
        Returns a list of disks the given directory is mounted on (there will be more than one if
        the mount point is on the RAID volume)
        """
        if not os.path.exists(directory):
            if not recur:
                print("{} doesn't exist - skipping".format(directory))

            return []

        try:
            udev_obj = pyudev.Device.from_device_number(self.__pyudev_ctx, 'block', os.stat(directory).st_dev)
            return self.__get_phys_devices(udev_obj)
        except:
            # handle cases like ecryptfs where the directory is mounted to another directory and not to some block device
            filesystem = run_one_command(['df', '-P', directory]).splitlines()[-1].split()[0].strip()
            if not re.search(r'^/dev/', filesystem):
                devs = self.__learn_directory(filesystem, True)
            else:
                raise Exception("Logic error: failed to create a udev device while 'df -P' {} returns a {}".format(directory, filesystem))

            # log error only for the original directory
            if not recur and not devs:
                print("Can't get a block device for {} - skipping".format(directory))

            return devs

    def __get_phys_devices(self, udev_obj):
        # if device is a virtual device - the underlying physical devices are going to be its slaves
        if re.search(r'virtual', udev_obj.sys_path):
            return list(itertools.chain.from_iterable([ self.__get_phys_devices(pyudev.Device.from_device_file(self.__pyudev_ctx, "/dev/{}".format(slave))) for slave in os.listdir(os.path.join(udev_obj.sys_path, 'slaves')) ]))
        else:
            # device node is something like /dev/sda1 - we need only the part without /dev/
            return [ re.match(r'/dev/(\S+\d*)', udev_obj.device_node).group(1) ]

    def __learn_irqs(self):
        disk2irqs = {}

        for devices in list(self.__dir2disks.values()) + [ self.args.devs ]:
            for device in devices:
                # There could be that some of the given directories are on the same disk.
                # There is no need to rediscover IRQs of the disk we've already handled.
                if device in disk2irqs.keys():
                    continue

                udev_obj = pyudev.Device.from_device_file(self.__pyudev_ctx, "/dev/{}".format(device))
                dev_sys_path = udev_obj.sys_path
                split_sys_path = list(pathlib.PurePath(dev_sys_path).parts)

                # first part is always /sys/devices/pciXXX ...
                controller_path_parts = split_sys_path[0:4]

                # ...then there is a chain of one or more "domain:bus:device.function" followed by the storage device enumeration crap
                # e.g. /sys/devices/pci0000:00/0000:00:1f.2/ata2/host1/target1:0:0/1:0:0:0/block/sda/sda3 or
                #      /sys/devices/pci0000:00/0000:00:02.0/0000:02:00.0/host6/target6:2:0/6:2:0:0/block/sda/sda1
                # We want only the path till the last BDF including - it contains the IRQs information.

                patt = re.compile("^[0-9ABCDEFabcdef]{4}\:[0-9ABCDEFabcdef]{2}\:[0-9ABCDEFabcdef]{2}\.[0-9ABCDEFabcdef]$")
                for split_sys_path_branch in split_sys_path[4:]:
                    if patt.search(split_sys_path_branch):
                        controller_path_parts.append(split_sys_path_branch)
                    else:
                        break

                controler_path_str = functools.reduce(lambda x, y : os.path.join(x, y), controller_path_parts)
                disk2irqs[device] = learn_all_irqs_one(controler_path_str, self.__irqs2procline, 'blkif')

        return disk2irqs

    def __get_feature_file(self, dev_node, path_creator):
        """
        Find the closest ancestor with the given feature and return its ('feature file', 'device node') tuple.

        If there isn't such an ancestor - return (None, None) tuple.

        :param dev_node Device node file name, e.g. /dev/sda1
        :param path_creator A functor that creates a feature file name given a device system file name
        """
        udev = pyudev.Device.from_device_file(pyudev.Context(), dev_node)
        feature_file = path_creator(udev.sys_path)

        if os.path.exists(feature_file):
            return feature_file, dev_node
        elif udev.parent is not None:
            return self.__get_feature_file(udev.parent.device_node, path_creator)
        else:
            return None, None

    def __tune_one_feature(self, dev_node, path_creator, value, tuned_devs_set):
        """
        Find the closest ancestor that has the given feature, configure it and
        return True.

        If there isn't such ancestor - return False.

        :param dev_node Device node file name, e.g. /dev/sda1
        :param path_creator A functor that creates a feature file name given a device system file name
        """
        feature_file, feature_node = self.__get_feature_file(dev_node, path_creator)

        if feature_file is None:
            return False

        if feature_node not in tuned_devs_set:
            fwriteln_and_log(feature_file, value)
            tuned_devs_set.add(feature_node)

        return True

    def __tune_io_scheduler(self, dev_node, io_scheduler):
        return self.__tune_one_feature(dev_node, lambda p : os.path.join(p, 'queue', 'scheduler'), io_scheduler, self.__io_scheduler_tuned_devs)

    def __tune_nomerges(self, dev_node):
        return self.__tune_one_feature(dev_node, lambda p : os.path.join(p, 'queue', 'nomerges'), self.__nomerges, self.__nomerges_tuned_devs)

    def __get_io_scheduler(self, dev_node):
        """
        Return a supported scheduler that is also present in the required schedulers list (__io_schedulers).

        If there isn't such a supported scheduler - return None.
        """
        feature_file, feature_node = self.__get_feature_file(dev_node, lambda p : os.path.join(p, 'queue', 'scheduler'))

        lines = readlines(feature_file)
        if not lines:
            return None

        # Supported schedulers appear in the config file as a single line as follows:
        #
        # sched1 [sched2] sched3
        #
        # ...with one or more schedulers where currently selected scheduler is the one in brackets.
        #
        # Return the scheduler with the highest priority among those that are supported for the current device.
        supported_schedulers = frozenset([scheduler.lstrip("[").rstrip("]") for scheduler in lines[0].split(" ")])
        return next((scheduler for scheduler in self.__io_schedulers if scheduler in supported_schedulers), None)

    def __tune_disk(self, device):
        dev_node = "/dev/{}".format(device)
        io_scheduler = self.__get_io_scheduler(dev_node)

        if not io_scheduler:
            print("Not setting I/O Scheduler for {} - required schedulers ({}) are not supported".format(device, list(self.__io_schedulers)))
        elif not self.__tune_io_scheduler(dev_node, io_scheduler):
            print("Not setting I/O Scheduler for {} - feature not present".format(device))

        if not self.__tune_nomerges(dev_node):
            print("Not setting 'nomerges' for {} - feature not present".format(device))

    def __tune_disks(self, disks):
        for disk in disks:
            self.__tune_disk(disk)

################################################################################
class TuneModes(enum.Enum):
    disks = 0
    net = 1

    @staticmethod
    def names():
        return list(TuneModes.__members__.keys())

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
argp.add_argument('--nic', help='network interface name, by default uses \'eth0\'')
argp.add_argument('--get-cpu-mask', action='store_true', help="print the CPU mask to be used for compute")
argp.add_argument('--verbose', action='store_true', help="be more verbose about operations and their result")
argp.add_argument('--tune', choices=TuneModes.names(), help="components to configure (may be given more than once)", action='append', default=[])
argp.add_argument('--cpu-mask', help="mask of cores to use, by default use all available cores", metavar='MASK')
argp.add_argument('--dir', help="directory to optimize (may appear more than once)", action='append', dest='dirs', default=[])
argp.add_argument('--dev', help="device to optimize (may appear more than once), e.g. sda1", action='append', dest='devs', default=[])
argp.add_argument('--options-file', help="configuration YAML file")
argp.add_argument('--dump-options-file', action='store_true', help="Print the configuration YAML file containing the current configuration")

def parse_options_file(prog_args):
    if not prog_args.options_file:
        return

    y = yaml.load(open(prog_args.options_file))
    if y is None:
        return

    if 'mode' in y and not prog_args.mode:
        if not y['mode'] in PerfTunerBase.SupportedModes.names():
            raise Exception("Bad 'mode' value in {}: {}".format(prog_args.options_file, y['mode']))
        prog_args.mode = y['mode']

    if 'nic' in y and not prog_args.nic:
        prog_args.nic = y['nic']

    if 'tune' in y:
        if set(y['tune']) <= set(TuneModes.names()):
            prog_args.tune.extend(y['tune'])
        else:
            raise Exception("Bad 'tune' value in {}: {}".format(prog_args.options_file, y['tune']))

    if 'cpu_mask' in y and not prog_args.cpu_mask:
        hex_32bit_pattern='0x[0-9a-fA-F]{1,8}'
        mask_pattern = re.compile('^{}((,({})?)*,{})*$'.format(hex_32bit_pattern, hex_32bit_pattern, hex_32bit_pattern))
        if mask_pattern.match(str(y['cpu_mask'])):
            prog_args.cpu_mask = y['cpu_mask']
        else:
            raise Exception("Bad 'cpu_mask' value in {}: {}".format(prog_args.options_file, str(y['cpu_mask'])))

    if 'dir' in y:
        prog_args.dirs.extend(y['dir'])

    if 'dev' in y:
        prog_args.devs.extend(y['dev'])

def dump_config(prog_args):
    prog_options = {}

    if prog_args.mode:
        prog_options['mode'] = prog_args.mode

    if prog_args.nic:
        prog_options['nic'] = prog_args.nic

    if prog_args.tune:
        prog_options['tune'] = prog_args.tune

    if prog_args.cpu_mask:
        prog_options['cpu_mask'] = prog_args.cpu_mask

    if prog_args.dirs:
        prog_options['dir'] = prog_args.dirs

    if prog_args.devs:
        prog_options['dev'] = prog_args.devs

    print(yaml.dump(prog_options, default_flow_style=False))
################################################################################

args = argp.parse_args()
parse_options_file(args)

# if nothing needs to be configured - quit
if args.tune is None:
    sys.exit("ERROR: At least one tune mode MUST be given.")

# set default values #####################
if not args.nic:
    args.nic = 'eth0'

if not args.cpu_mask:
    args.cpu_mask = run_hwloc_calc(['all'])
##########################################

if args.dump_options_file:
    dump_config(args)
    sys.exit(0)

try:
    tuners = []

    if TuneModes.disks.name in args.tune:
        tuners.append(DiskPerfTuner(args))

    if TuneModes.net.name in args.tune:
        tuners.append(NetPerfTuner(args))

    # Set the minimum mode among all tuners
    mode = min([ tuner.mode for tuner in tuners ])
    for tuner in tuners:
        tuner.mode = mode

    if args.get_cpu_mask:
        # Print the compute mask from the first tuner - it's going to be the same in all of them
        print(tuners[0].compute_cpu_mask)
    else:
        # Tune the system
        restart_irqbalance(itertools.chain.from_iterable([ tuner.irqs for tuner in tuners ]))

        for tuner in tuners:
            tuner.tune()
except Exception as e:
    sys.exit("ERROR: {}. Your system can't be tuned until the issue is fixed.".format(e))

