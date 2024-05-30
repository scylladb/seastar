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

import os

SUPPORTED_MODES = ['release', 'debug', 'dev', 'sanitize']

ROOT_PATH = os.path.realpath(os.path.dirname(__file__))

DEFAULT_BUILD_ROOT = 'build'

COOKING_BASIC_ARGS = ['./cooking.sh']

SUPPORTED_SSL_PROVIDERS = ['GnuTLS', 'OpenSSL']

def build_path(mode, build_root):
    """Return the absolute path to the build directory for the given mode,
    i.e., seastar_dir/<build_root>/<mode>"""
    assert mode in SUPPORTED_MODES, f'Unsupported build mode: {mode}'
    return os.path.join(ROOT_PATH, build_root, mode)

def is_release_mode(mode):
    return mode == 'release'

def convert_strings_to_cmake_list(*args):
    """Converts a sequence of whitespace-separated strings of tokens into a semicolon-separated
    string of tokens for CMake.

    """
    return ';'.join(' '.join(args).split())

def translate_arg(arg, new_name, value_when_none='no'):
    """
    Translate a value populated from the command-line into a name to pass to the invocation of CMake.
    """
    if arg is None:
        value = value_when_none
    elif type(arg) is bool:
        value = 'yes' if arg else 'no'
    else:
        value = arg

    if value is None:
        return ''
    else:
        return '-DSeastar_{}={}'.format(new_name, value)
