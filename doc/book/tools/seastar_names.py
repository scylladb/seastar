#!/usr/bin/python3 -I
# Writes tools/seastar_names.txt, the list of Seastar's public names that
# tools/bookfilter.py indexes automatically when the text mentions them in a
# code span. The names come from Doxygen, run on Seastar's headers with the
# settings of Seastar's own API reference (doc/Doxyfile.in), which leave out
# the seastar::internal namespaces, with two differences: undocumented names
# are included (EXTRACT_ALL), and so are "\cond internal" sections
# (ENABLED_SECTIONS). Without them, the list would miss names that the API
# reference misses too: app_template has no documentation comment, and an
# unclosed "\cond internal" in log.hh hides the logger class. Run it again
# after moving the book to a newer Seastar; the book's build itself doesn't
# need Doxygen.
#
# Usage: seastar_names.py SEASTAR_SOURCE_DIR [SEASTAR_BUILD_DIR]
# (the build directory, such as build/release, adds the generated headers).
#
# Each line of the output is "KIND NAME": KIND is namespace, class, function,
# type (a typedef, alias or enum), variable or macro, and NAME is the full
# name, such as seastar::sharded or seastar::future::get. Members of classes
# are listed too, so that a code span such as `future::get()` is found.

import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

book = Path(__file__).resolve().parent.parent
out = book / 'tools' / 'seastar_names.txt'

def doxyfile(src, build, xml_dir):
    inputs = [src / 'include'] + ([build / 'gen' / 'include'] if build else [])
    return '\n'.join([
        'INPUT = ' + ' '.join(str(p) for p in inputs if p.exists()),
        'RECURSIVE = YES',
        'FILE_PATTERNS = *.hh',
        'EXTRACT_ALL = YES',
        'ENABLED_SECTIONS = internal',
        'EXTRACT_PRIVATE = NO',
        'EXTRACT_STATIC = YES',
        'HIDE_UNDOC_MEMBERS = NO',
        'HIDE_UNDOC_CLASSES = NO',
        'INTERNAL_DOCS = NO',
        'EXCLUDE_SYMBOLS = seastar::internal seastar::coroutine::internal',
        'ENABLE_PREPROCESSING = YES',
        'MACRO_EXPANSION = YES',
        'GENERATE_HTML = NO',
        'GENERATE_LATEX = NO',
        'GENERATE_XML = YES',
        f'XML_OUTPUT = {xml_dir}',
        'QUIET = YES',
        'WARNINGS = NO',
        'WARN_IF_UNDOCUMENTED = NO',
    ]) + '\n'

kinds = {'namespace': 'namespace', 'class': 'class', 'struct': 'class', 'union': 'class',
         'concept': 'class', 'function': 'function', 'typedef': 'type', 'enum': 'type',
         'variable': 'variable', 'define': 'macro'}

def public(name):
    parts = name.split('::')
    return (parts[0] == 'seastar'
            and not any(p in ('internal', 'detail', 'impl') or p.startswith('_') for p in parts)
            and not any(p.startswith('operator') or p.startswith('~') for p in parts))

def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__ if __doc__ else 'usage: seastar_names.py SEASTAR_SOURCE_DIR [SEASTAR_BUILD_DIR]')
    src = Path(sys.argv[1]).resolve()
    build = Path(sys.argv[2]).resolve() if len(sys.argv) == 3 else None
    names = {}
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        (tmp / 'Doxyfile').write_text(doxyfile(src, build, tmp / 'xml'))
        subprocess.run(['doxygen', str(tmp / 'Doxyfile')], check=True, cwd=tmp,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        index = ET.parse(tmp / 'xml' / 'index.xml').getroot()
        for compound in index.findall('compound'):
            kind, cname = compound.get('kind'), compound.findtext('name')
            if kind in kinds and public(cname):
                names[cname] = kinds[kind]
            for member in compound.findall('member'):
                mkind, mname = member.get('kind'), member.findtext('name')
                if mkind == 'define':
                    if mname.startswith('SEASTAR_') and not mname.endswith('_HH'):
                        names.setdefault(mname, 'macro')
                    continue
                if mkind not in kinds or kind not in ('namespace', 'class', 'struct', 'union'):
                    continue
                full = f'{cname}::{mname}'
                if public(full):
                    names.setdefault(full, kinds[mkind])
    commit = subprocess.run(['git', '-C', str(src), 'rev-parse', '--short', 'HEAD'],
                            capture_output=True, text=True).stdout.strip()
    lines = [f'# Seastar\'s public names, written by tools/seastar_names.py from Seastar {commit}.']
    lines += [f'{kind} {name}' for name, kind in sorted(names.items())]
    out.write_text('\n'.join(lines) + '\n')
    print(f'{out}: {len(names)} names')

if __name__ == '__main__':
    main()
