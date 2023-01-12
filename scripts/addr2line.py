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
# Copyright (C) 2017 ScyllaDB

import bisect
import collections
import re
import sys
import subprocess
from enum import Enum
from typing import Any

# special binary path/module indicating that the address is from the kernel
KERNEL_MODULE = '<kernel>'

class Addr2Line:

    # Matcher for a line that appears at the end a single decoded
    # address, which we force by adding a dummy 0x0 address. The
    # pattern varies between binutils addr2line and llvm-addr2line
    # so we match both.
    dummy_pattern = re.compile(
        r"(.*0x0000000000000000: \?\? \?\?:0\n)" # addr2line pattern
        r"|"
        r"(.*0x0: \?\? at \?\?:0\n)"  # llvm-addr2line pattern
        )

    def __init__(self, binary, concise=False, cmd_path="addr2line"):
        self._binary = binary

        # Print warning if binary has no debug info according to `file`.
        # Note: no message is printed for system errors as they will be
        # printed also by addr2line later on.
        output = subprocess.check_output(["file", self._binary])
        s = output.decode("utf-8")
        if s.find('ELF') >= 0 and s.find('debug_info', len(self._binary)) < 0:
            print('{}'.format(s))

        options = f"-{'C' if not concise else ''}fpia"
        self._input = subprocess.Popen([cmd_path, options, "-e", self._binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE, universal_newlines=True)
        if concise:
            self._output = subprocess.Popen(["c++filt", "-p"], stdin=self._input.stdout, stdout=subprocess.PIPE, universal_newlines=True)
        else:
            self._output = self._input

        # If a library doesn't exist in a particular path, addr2line
        # will just exit.  We need to be robust against that.  We
        # can't just wait on self._addr2line since there is no
        # guarantee on what timeout is sufficient.
        self._input.stdin.write('\n')
        self._input.stdin.flush()
        res = self._output.stdout.readline()
        self._missing = res == ''

    def _read_resolved_address(self):
        res = self._output.stdout.readline()
        # remove the address
        res = res.split(': ', 1)[1]
        line = ''
        while Addr2Line.dummy_pattern.fullmatch(line) is None:
            res += line
            line = self._output.stdout.readline()
        return res

    def __call__(self, address):
        if self._missing:
            return " ".join([self._binary, address, '\n'])
        # We print a dummy 0x0 address after the address we are interested in
        # which we can look for in _read_address
        self._input.stdin.write(address + '\n0x0\n')
        self._input.stdin.flush()
        return self._read_resolved_address()

class KernelResolver:
    """A resolver for kernel addresses which tries to read from /proc/kallsyms."""

    LAST_SYMBOL_MAX_SIZE = 1024

    def __init__(self):
        syms : list[tuple[int, str]] = []
        ksym_re = re.compile(r'(?P<addr>[0-9a-f]+) (?P<type>.+) (?P<name>\S+)')
        warnings_left = 10

        self.error = None

        try:
            f = open('/proc/kallsyms', 'r')
        except OSError as e:
            self.error = f'Cannot open /proc/kallsyms: {e}'
            print(self.error)
            return

        try:
            for line in f:
                m = ksym_re.match(line)
                if not m:
                    if warnings_left > 0: # don't spam too much
                        print(f'WARNING: /proc/kallsyms regex match failure: {line.strip()}', file=sys.stdout)
                        warnings_left -= 1
                else:
                    syms.append((int(m.group('addr'), 16), m.group('name')))
        finally:
            f.close()

        if not syms:
            # make empty kallsyms (?) an error so we can assum len >= 1 below
            self.error = 'kallsyms was empty'
            print(self.error)
            return

        syms.sort()

        if syms[-1][0] == 0:
            # zero values for all symbols means that kptr_restrict blocked you
            # from seeing the kernel symbol addresses
            print('kallsyms is restricted, set /proc/sys/kernel/kptr_restrict to 0 to decode')
            self.error = 'kallsyms is restricted'
            return

        # split because bisect can't take a key func before 3.10
        self.sym_addrs : tuple[int]
        self.sym_names : tuple[str]
        self.sym_addrs, self.sym_names = zip(*syms) # type: ignore


    def __call__(self, addrstr):
        if self.error:
            return addrstr + '\n'

        sa = self.sym_addrs
        sn = self.sym_names
        slen = len(sa)
        address = int(addrstr, 16)
        idx = bisect.bisect_right(sa, address) - 1
        assert -1 <= idx < slen
        if idx == -1:
            return f'{addrstr} ({sa[0] - address} bytes before first symbol)\n'
        if idx == slen - 1:
            # We can easily detect symbol addresses which are too small: they fall before
            # the first symbol in kallsyms, but for too large it is harder: we can't really
            # distinguish between an address that is in the *very last* function in the symbol map
            # and one which is beyond that, since kallsyms doesn't include symbol size. Instead
            # we use a bit of a quick and dirty heuristic: if the symbol is *far enough* beyond
            # the last symbol we assume it is not valid. Most likely, the overwhelming majority
            # of cases are invalid (e.g., due to KASLR) as the final symbol in the map is usually
            # something obscure.
            lastsym = sa[-1]
            if address - lastsym > self.LAST_SYMBOL_MAX_SIZE:
                return f'{addrstr} ({address - lastsym} bytes after last symbol)\n'
        saddr = sa[idx]
        assert saddr <= address
        return f'{sn[idx]}+0x{address - saddr:x}\n'


class BacktraceResolver(object):

    class BacktraceParser(object):
        class Type(Enum):
            ADDRESS = 1
            SEPARATOR = 2

        def __init__(self):
            addr = "0x[0-9a-f]+"
            path = "\S+"
            token = f"(?:{path}\+)?{addr}"
            full_addr_match = f"(?:(?P<path>{path})\s*\+\s*)?(?P<addr>{addr})"
            ignore_addr_match = f"(?:(?P<path>{path})\s*\+\s*)?(?:{addr})"
            self.oneline_re = re.compile(f"^((?:.*(?:(?:at|backtrace):?|:))?(?:\s+))?({token}(?:\s+{token})*)(?:\).*|\s*)$", flags=re.IGNORECASE)
            self.address_re = re.compile(full_addr_match, flags=re.IGNORECASE)
            self.syslog_re = re.compile(f"^(?:#\d+\s+)(?P<addr>{addr})(?:.*\s+)\({ignore_addr_match}\)\s*$", flags=re.IGNORECASE)
            self.kernel_re = re.compile(fr'^kernel callstack: (?P<addrs>(?:{addr}\s*)+)$')
            self.asan_re = re.compile(f"^(?:.*\s+)\({full_addr_match}\)(\s+\(BuildId: [0-9a-fA-F]+\))?$", flags=re.IGNORECASE)
            self.asan_ignore_re = re.compile(f"^=.*$", flags=re.IGNORECASE)
            self.generic_re = re.compile(f"^(?:.*\s+){full_addr_match}\s*$", flags=re.IGNORECASE)
            self.separator_re = re.compile('^\W*-+\W*$')


        def split_addresses(self, addrstring: str, default_path=None):
            addresses : list[dict[str, Any]] = []
            for obj in addrstring.split():
                m = re.match(self.address_re, obj)
                assert m, f'addr did not match address regex: {obj}'
                #print(f"  >>> '{obj}': address {m.groups()}")
                addresses.append({'path': m.group(1) or default_path, 'addr': m.group(2)})
            return addresses

        def __call__(self, line):
            def get_prefix(s):
                if s is not None:
                    s = s.strip()
                return s or None

            # order here is important: the kernel callstack regex
            # needs to come first since it is more specific and would
            # otherwise be matched by the online regex which comes next
            m = self.kernel_re.match(line)
            if m:
                return {
                    'type': self.Type.ADDRESS,
                    'prefix': 'kernel callstack: ',
                    'addresses' : self.split_addresses(m.group('addrs'), KERNEL_MODULE)
                }

            m = re.match(self.oneline_re, line)
            if m:
                #print(f">>> '{line}': oneline {m.groups()}")
                return {
                    'type': self.Type.ADDRESS,
                    'prefix': get_prefix(m.group(1)),
                    'addresses': self.split_addresses(m.group(2))
                }

            m = re.match(self.syslog_re, line)
            if m:
                #print(f">>> '{line}': syslog {m.groups()}")
                ret = {'type': self.Type.ADDRESS}
                ret['prefix'] = None
                ret['addresses'] = [{'path': m.group('path'), 'addr': m.group('addr')}]
                return ret

            m = re.match(self.asan_ignore_re, line)
            if m:
                #print(f">>> '{line}': asan ignore")
                return None

            m = re.match(self.asan_re, line)
            if m:
                #print(f">>> '{line}': asan {m.groups()}")
                ret = {'type': self.Type.ADDRESS}
                ret['prefix'] = None
                ret['addresses'] = [{'path': m.group('path'), 'addr': m.group('addr')}]
                return ret

            m = re.match(self.generic_re, line)
            if m:
                #print(f">>> '{line}': generic {m.groups()}")
                ret = {'type': self.Type.ADDRESS}
                ret['prefix'] = None
                ret['addresses'] = [{'path': m.group('path'), 'addr': m.group('addr')}]
                return ret

            match = re.match(self.separator_re, line)
            if match:
                return {'type': self.Type.SEPARATOR}

            #print(f">>> '{line}': None")
            return None

    def __init__(self, executable, before_lines=1, context_re='', verbose=False, concise=False, cmd_path='addr2line'):
        self._executable = executable
        self._current_backtrace = []
        self._prefix = None
        self._before_lines = before_lines
        self._before_lines_queue = collections.deque(maxlen=before_lines)
        self._i = 0
        self._known_backtraces = {}
        if context_re is not None:
            self._context_re = re.compile(context_re)
        else:
            self._context_re = None
        self._verbose = verbose
        self._concise = concise
        self._cmd_path = cmd_path
        self._known_modules = {}
        self._get_resolver_for_module(self._executable) # fail fast if there is something wrong with the exe resolver
        self.parser = self.BacktraceParser()

    def _get_resolver_for_module(self, module):
        if not module in self._known_modules:
            if module == KERNEL_MODULE:
                resolver = KernelResolver()
            else:
                resolver = Addr2Line(module, self._concise, self._cmd_path)
            self._known_modules[module] = resolver
        return self._known_modules[module]

    def __enter__(self):
        return self

    def __exit__(self, type, value, tb):
        self._print_current_backtrace()

    def resolve_address(self, address, module=None, verbose=None):
        if module is None:
            module = self._executable
        if verbose is None:
            verbose = self._verbose
        resolved_address = self._get_resolver_for_module(module)(address)
        if verbose:
            resolved_address = '{{{}}} {}: {}'.format(module, address, resolved_address)
        return resolved_address

    def _print_resolved_address(self, module, address):
        sys.stdout.write(self.resolve_address(address, module))

    def _backtrace_context_matches(self):
        if self._context_re is None:
            return True

        if any(map(lambda x: self._context_re.search(x) is not None, self._before_lines_queue)):
            return True

        if (not self._prefix is None) and self._context_re.search(self._prefix):
            return True

        return False

    def _print_current_backtrace(self):
        if len(self._current_backtrace) == 0:
            return

        if not self._backtrace_context_matches():
            self._current_backtrace = []
            return

        for line in self._before_lines_queue:
            sys.stdout.write(line)

        if not self._prefix is None:
            print(self._prefix)
            self._prefix = None

        backtrace = "".join(map(str, self._current_backtrace))
        if backtrace in self._known_backtraces:
            print("[Backtrace #{}] Already seen, not resolving again.".format(self._known_backtraces[backtrace]))
            print("") # To separate traces with an empty line
            self._current_backtrace = []
            return

        self._known_backtraces[backtrace] = self._i

        print("[Backtrace #{}]".format(self._i))

        for module, addr in self._current_backtrace:
            self._print_resolved_address(module, addr)

        print("") # To separate traces with an empty line

        self._current_backtrace = []
        self._i += 1

    def __call__(self, line):
        res = self.parser(line)

        if not res:
            self._print_current_backtrace()
            if self._before_lines > 0:
                self._before_lines_queue.append(line)
            elif self._before_lines < 0:
                sys.stdout.write(line) # line already has a trailing newline
            else:
                pass # when == 0 no non-backtrace lines are printed
        elif res['type'] == self.BacktraceParser.Type.SEPARATOR:
            pass
        elif res['type'] == self.BacktraceParser.Type.ADDRESS:
            addresses = res['addresses']
            if len(addresses) > 1:
                self._print_current_backtrace()
            if len(self._current_backtrace) == 0:
                self._prefix = res['prefix']
            for r in addresses:
                if r['path']:
                    self._current_backtrace.append((r['path'], r['addr']))
                else:
                    self._current_backtrace.append((self._executable, r['addr']))
            if len(addresses) > 1:
                self._print_current_backtrace()
        else:
            print(f"Unknown '{line}': {res}")
            raise RuntimeError("Unknown result type {res}")

