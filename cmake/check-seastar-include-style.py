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

import fileinput
import os.path
import re
import sys


def check_includes(files, dirname):
    # Check for include directives with quotes for specified dirname
    incorrect_include = re.compile(rf'#include\s+"({dirname}/[^\"]+)"')
    num_errors = 0
    for line in fileinput.input(files=files, encoding="utf-8"):
        # Look for #include \"seastar/...\" pattern
        if matched := incorrect_include.match(line):
            location = f"{fileinput.filename()}:{fileinput.lineno()}"
            header = matched.group(1)
            print(f"{location}: warning: please include seastar headers using: #include <{header}>")
            num_errors += 1
    return num_errors


def main():
    # If any incorrect includes are found, fail the check
    files = [fn for fn in sys.argv[1:] if os.path.exists(fn)]
    if check_includes(files, "seastar") > 0:
        sys.exit(1)


if __name__ == '__main__':
    main()
