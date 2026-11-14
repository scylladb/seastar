# The solutions to exercises that change Shoal are patches, such as
# examples/solutions/shoal-scan.patch: each is a unified diff against the
# version of Shoal it changes, which its header names, in a line such as
#
#   # base: shoal/v8
#
# and may also give the options that the tests' servers need:
#
#   # server options: --timeout 2
#
# A patch may add a file of tests of its own, test_solutions.py. This module
# makes a complete copy of the changed Shoal, for tools/check_examples.py,
# which builds and tests it, and tools/bookfilter.py, which shows its code.
import re
import shutil
import subprocess
from pathlib import Path

book = Path(__file__).resolve().parent.parent


def header(patch):
    """The base version's directory, such as Path('.../shoal/v8'), and the
    list of options for the tests' servers, from a patch's header."""
    text = Path(patch).read_text()
    base = re.search(r'^# base: (\S+)$', text, re.M)
    if not base:
        raise ValueError(f'{patch}: no "# base: shoal/vN" line')
    options = re.search(r'^# server options: (.+)$', text, re.M)
    return book / base.group(1), [options.group(1)] if options else []


def apply(patch, dest):
    """Copies the patch's base version into the directory dest (which must not
    exist), without its build directory, and applies the patch there.
    Returns dest."""
    base, _ = header(patch)
    shutil.copytree(base, dest, ignore=shutil.ignore_patterns('build', '__pycache__'))
    p = subprocess.run(['patch', '-p1', '-s', '--no-backup-if-mismatch', '-d', str(dest),
                        '-i', str(Path(patch).resolve())],
                       capture_output=True, text=True)
    if p.returncode != 0:
        raise RuntimeError(f'{patch} does not apply to {base.relative_to(book)}:\n'
                           f'{p.stdout}{p.stderr}')
    return Path(dest)
