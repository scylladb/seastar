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

import argparse
import collections
import re
import sys
import subprocess
from enum import Enum

class Addr2Line:
    def __init__(self, binary, concise=False):
        self._binary = binary

        # Print warning if binary has no debug info according to `file`.
        # Note: no message is printed for system errors as they will be
        # printed also by addr2line later on.
        output = subprocess.check_output(["file", self._binary])
        s = output.decode("utf-8")
        if s.find('ELF') >= 0 and s.find('debug_info', len(self._binary)) < 0:
            print('{}'.format(s))

        options = f"-{'C' if not concise else ''}fpia"
        self._input = subprocess.Popen(["addr2line", options, "-e", self._binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE, universal_newlines=True)
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
        dummy = '0x0000000000000000: ?? ??:0\n'
        line = ''
        while line != dummy:
            res += line
            line = self._output.stdout.readline()
        return res

    def __call__(self, address):
        if self._missing:
            return " ".join([self._binary, address, '\n'])
        # print two lines to force addr2line to output a dummy
        # line which we can look for in _read_address
        self._input.stdin.write(address + '\n\n')
        self._input.stdin.flush()
        return self._read_resolved_address()

class BacktraceResolver(object):

    class BacktraceParser(object):
        class Type(Enum):
            ADDRESS = 1
            SEPARATOR = 2

        def __init__(self):
            addr = "0x[0-9a-f]+"
            path = "\S+"
            token = f"(?:{path}\+)?{addr}"
            full_addr_match = f"(?:({path})\+)?({addr})"
            self.oneline_re = re.compile(f"^((?:.*(?:(?:at|backtrace):?|:))?(?:\s+))?({token}(?:\s+{token})*)(?:\).*|\s*)$", flags=re.IGNORECASE)
            self.address_re = re.compile(full_addr_match, flags=re.IGNORECASE)
            self.asan_re = re.compile(f"^(?:.*\s+)\(({full_addr_match})\)\s*$", flags=re.IGNORECASE)
            self.separator_re = re.compile('^\W*-+\W*$')

        def __call__(self, line):
            def get_prefix(s):
                if s is not None:
                    s = s.strip()
                return s or None

            m = re.match(self.oneline_re, line)
            if m:
                #print(f">>> '{line}': oneline {m.groups()}")
                ret = {'type': self.Type.ADDRESS}
                ret['prefix'] = get_prefix(m.group(1))
                addresses = []
                for obj in m.group(2).split():
                    m = re.match(self.address_re, obj)
                    #print(f"  >>> '{obj}': address {m.groups()}")
                    addresses.append({'path': m.group(1), 'addr': m.group(2)})
                ret['addresses'] = addresses
                return ret

            m = re.match(self.asan_re, line)
            if m:
                #print(f">>> '{line}': asan {m.groups()}")
                ret = {'type': self.Type.ADDRESS}
                ret['prefix'] = None
                ret['addresses'] = [{'path': m.group(2), 'addr': m.group(3)}]
                return ret

            match = re.match(self.separator_re, line)
            if match:
                return {'type': self.Type.SEPARATOR}

            #print(f">>> '{line}': None")
            return None

    def __init__(self, executable, before_lines=1, context_re='', verbose=False, concise=False):
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
        self._known_modules = {self._executable: Addr2Line(self._executable, concise)}
        self.parser = self.BacktraceParser()

    def _get_resolver_for_module(self, module):
        if not module in self._known_modules:
            self._known_modules[module] = Addr2Line(module, self._concise)
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

