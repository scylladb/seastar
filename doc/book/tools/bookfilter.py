#!/usr/bin/python3 -I
# Pandoc filter for the book, run by tools/build.py. It
#  - replaces references by label with numbers: "[chapter](#ch:coroutines)"
#    becomes "chapter 4", "[appendix](#app:building-seastar)" becomes "appendix A" and
#    "[Part](#part:io)" becomes "Part IV" (an empty link text gives the
#    lowercase word); a link with any other text keeps its text;
#  - renders the part divisions that build.py inserts (a Div with class
#    "part"): a part page in LaTeX, a heading in the other formats;
#  - switches between front matter, main matter, appendices and back matter
#    at the marker Divs that build.py inserts;
#  - fills code blocks from the example programs, so that the book shows code
#    that really compiles and runs, labeled with the file's path (see
#    include_code() below);
#  - makes the PDF's index: it turns index markers into LaTeX's \index, and
#    indexes Seastar's names automatically (see "The index" below).
# The numbers come from the JSON file named by the BOOK_LABELS environment
# variable, which build.py writes from metadata.yaml.

import json
import os
import re
import sys
import tempfile
import textwrap
from pathlib import Path
from pandocfilters import (walk, stringify, RawBlock, RawInline, Header, Div, Str,
                           Space, Span, Plain, Code)

labels = json.load(open(os.environ['BOOK_LABELS']))
words = {'chapter': 'chapter', 'appendix': 'appendix', 'part': 'Part', 'figure': 'Figure'}
region = 'front'   # front, main, appendix or back

def latex_escape(s):
    for c in '\\&%$#_{}':
        s = s.replace(c, '\\' + c)
    return s

def reference(content, target):
    label = target[1:]
    if label not in labels:
        sys.exit(f'bookfilter: reference to unknown label "{label}"')
    entry = labels[label]
    text = stringify(content)
    if text and text.lower() != words[entry['kind']].lower():
        return None
    return [Str((text or words[entry['kind']]) + ' ' + entry['number'])]

book = Path(__file__).resolve().parent.parent
# Run with python3 -I, which leaves this directory out of the module path.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import shoal_patch  # noqa: E402
snippet_marker = re.compile(r'^\s*(?://|#) (end )?snippet: ([\w-]+)\s*$')

# The changed copies of Shoal made from patches, by patch, made once per run.
patched_copies = {}

def patched(patch):
    """A temporary copy of the Shoal that patch makes, removed at exit."""
    if patch not in patched_copies:
        tmp = tempfile.TemporaryDirectory()
        patched_copies[patch] = (tmp, shoal_patch.apply(patch, Path(tmp.name) / 'shoal'))
    return patched_copies[patch][1]

def include_code(attrs, text, fmt):
    """A code block such as
        ```{.cpp include="examples/tutorial-introduction/hello.cc"}
        ```
    shows that whole file (the path is relative to the book's directory), and
    with snippet="name" only the lines between "// snippet: name" and
    "// end snippet: name" in it, dedented (in files whose comments start
    with "#", such as CMakeLists.txt, the markers start with "#" too).
    Lines with these markers never appear in the book. The block is labeled
    with the file's path, so the reader knows which file the code belongs
    to, and where to find all of it.

    A solution that changes Shoal is a patch (see tools/shoal_patch.py), and
    its code is shown from the changed Shoal, with file="name":
        ```{.cpp include="examples/solutions/shoal-scan.patch" file="commands.cc"
             snippet="scan"}
        ```
    labeled "shoal/v8/commands.cc, changed by examples/solutions/shoal-scan.patch"."""
    kvs = dict(attrs[2])
    path = book / kvs['include']
    if not path.exists():
        sys.exit(f'bookfilter: included file {kvs["include"]} does not exist')
    if text.strip():
        sys.exit(f'bookfilter: a code block including {kvs["include"]} must be empty')
    shown = kvs['include']
    if path.suffix == '.patch':
        if 'file' not in kvs:
            sys.exit(f'bookfilter: a code block including {kvs["include"]} needs file="..."')
        base, _ = shoal_patch.header(path)
        name_in_base = f'{base.relative_to(book)}/{kvs["file"]}'
        if (base / kvs['file']).exists():
            shown = f'{name_in_base}, changed by {kvs["include"]}'
        else:
            shown = f'{kvs["file"]}, added by {kvs["include"]}'
        path = patched(path) / kvs['file']
        if not path.exists():
            sys.exit(f'bookfilter: {kvs["include"]} has no file {kvs["file"]}')
    lines = path.read_text().splitlines()
    name = kvs.get('snippet')
    if name:
        starts = [i for i, l in enumerate(lines) if snippet_marker.match(l)
                  and snippet_marker.match(l).groups() == (None, name)]
        ends = [i for i, l in enumerate(lines) if snippet_marker.match(l)
                and snippet_marker.match(l).groups() == ('end ', name)]
        if len(starts) != 1 or len(ends) != 1 or ends[0] < starts[0]:
            sys.exit(f'bookfilter: {kvs["include"]} needs exactly one snippet "{name}",'
                     f' between "// snippet: {name}" and "// end snippet: {name}"')
        lines = lines[starts[0] + 1:ends[0]]
    lines = [l for l in lines if not snippet_marker.match(l)]
    code = textwrap.dedent('\n'.join(lines)).strip('\n')
    kept = [[k, v] for k, v in attrs[2] if k not in ('include', 'snippet', 'file')]
    block = {'t': 'CodeBlock', 'c': [[attrs[0], attrs[1], kept], code]}
    if fmt == 'latex':
        label = RawBlock('latex', f'\\codefile{{{latex_escape(shown)}}}')
    else:
        label = Div(['', ['code-file'], []], [Plain([Code(['', [], []], shown)])])
    return [label, block]

# The index (PDF only; the other formats drop the markers).
#
# A Span with the class "index" indexes its text as it is (so a word that
# starts a sentence needs an "index" attribute to be indexed in lowercase),
# and one with an "index" attribute indexes that entry instead, in
# makeindex's syntax ("!" before a subentry, "|see{...}" and "|seealso{...}"
# for cross-references), except that text in backticks is set in code
# font, and a Seastar name in backticks, such as `seastar::future`, gives
# the same entry as the automatic index below, and a standard library name,
# such as `std::future`, sorts without its "std::". ("@", between a sort key
# and what is printed, can still be used, but Pandoc removes a backslash
# before "_" in an attribute, so LaTeX is best avoided there.) Both mark
# the page where the Span is, and keep its text. An empty Span marks the place without any text.
# The class "main" prints the page number in bold, for the place where the
# topic is mainly explained:
#
#     [preemption]{.index}           [stalls]{index="stall"}
#     [frames]{index="coroutine!frame"}    []{index="reactor" .main}
#
# A heading with an "index" attribute indexes its whole section, up to the
# next heading of the same or a higher level, as a range of pages:
#
#     ## Preemption {index="preemption"}
#
# Seastar's names are indexed automatically: a code span that names one of
# them, such as `sharded<T>`, `sleep()`, `seastar::sharded` or
# `future::get()`, is indexed under its full name (seastar::sharded), sorted
# by its name without "seastar::", at its first mention in each section. A
# member of a class is a subentry of its class. The names are in
# tools/seastar_names.txt, which tools/seastar_names.py writes. The standard
# library names listed in tools/std_names.txt are indexed the same way. A function
# is indexed only when written as a call (`sleep()`), and a namespace only
# with "seastar::"; a code span with the class "noindex", such as
# `program_options`{.noindex}, is never indexed. Seastar's headers, such as
# `<seastar/core/gate.hh>`, are indexed the same way, as subentries of
# "headers, Seastar's", so that a reader who meets a header in code can find
# where the book explains what it declares. Seastar's command-line options,
# listed in tools/seastar_options.txt, are indexed the same way too, under
# "command-line options": a code span that begins with one, such as
# `--cpuset` or `--cpuset 0-3`.

def load_seastar_names():
    names = {}
    for line in (book / 'tools' / 'seastar_names.txt').read_text().splitlines():
        if line and not line.startswith('#'):
            kind, name = line.split(' ', 1)
            names[name] = kind
    return names

seastar_names = load_seastar_names()

def load_std_names():
    names = {}
    for line in (book / 'tools' / 'std_names.txt').read_text().splitlines():
        if line and not line.startswith('#'):
            kind, name = line.split(' ', 1)
            names[name] = kind
    return names

std_names = load_std_names()
code_name = re.compile(r'^(?:seastar::)?([A-Za-z_]\w*(?:::[A-Za-z_]\w*)*)(?:<[^()]*>)?(?:\(.*\))?$')

def index_code(text):
    """A name in the index's code font (\\indexcode, in style/book.tex),
    allowed to break after each "::" but the one after "seastar", so that a
    long name fits in the index's narrow columns without leaving "seastar::"
    alone on a line. A name with
    a part too long for a column even so, such as
    seastar::make_backtraced_exception_ptr() or
    seastar::memory::scoped_large_allocation_warning_threshold, may also
    break after a "_"."""
    escaped = makeindex_escape(latex_escape(text))
    prefix = 'seastar::' if escaped.startswith('seastar::') else ''
    rest = escaped[len(prefix):].replace('::', '::\\allowbreak{}')
    # The pieces between the places where the name may break: the first
    # with "seastar::", and each with the "::" after it.
    parts = text[len(prefix):].split('::')
    pieces = [prefix + parts[0]] + parts[1:]
    if max(len(piece) + 2 for piece in pieces) > INDEX_CODE_CHARS:
        rest = rest.replace('\\_', '\\_\\allowbreak{}')
    return '\\indexcode{' + prefix + rest + '}'

# About how many characters of code fit across a column of the index.
INDEX_CODE_CHARS = 46

def makeindex_escape(s):
    return re.sub(r'([!@|"])', r'"\1', s)

def seastar_entry(text, explicit=False):
    """The index entry for a code span, or None if it doesn't name one of
    Seastar's public names. With explicit, from an index marker, a bare
    function or namespace name is accepted too."""
    m = code_name.match(text)
    if not m:
        return None
    name = m.group(1)
    full = name if name in seastar_names and name.startswith('SEASTAR_') else 'seastar::' + name
    kind = seastar_names.get(full)
    if not kind:
        return None
    # The book writes Seastar's functions as calls, `sleep()`, so a bare
    # name, such as `yield` naming a benchmark's variant, is something
    # else; and a bare namespace name, such as Boost's `program_options`,
    # often is too. Written with "seastar::", both are indexed.
    qualified = text.startswith('seastar::') or explicit
    if kind == 'function' and '(' not in text and not qualified:
        return None
    if kind == 'namespace' and not qualified:
        return None
    def item(full, kind, shown):
        key = full.removeprefix('seastar::')
        text = shown + ('()' if kind == 'function' else '')
        return f'{makeindex_escape(key)}@{index_code(text)}'
    parent = full.rsplit('::', 1)[0]
    if '::' in full and seastar_names.get(parent) == 'class':
        member = full.rsplit('::', 1)[1]
        return (item(parent, 'class', parent) + '!'
                + f'{makeindex_escape(member)}@{index_code(member + ("()" if kind == "function" else ""))}')
    return item(full, kind, full)

def std_entry(text):
    """The index entry for a code span that names one of the standard
    library names in tools/std_names.txt, or None. Like a Seastar name, it
    sorts without its namespace, so std::future stands next to
    seastar::future, and a function is shown as a call."""
    m = code_name.match(text)
    if not m or m.group(1) not in std_names:
        return None
    name = m.group(1)
    shown = name + ('()' if std_names[name] == 'function' else '')
    return f'{makeindex_escape(name.removeprefix("std::"))}@{index_code(shown)}'

header_path = re.compile(r'<(seastar/[\w.+-]+(?:/[\w.+-]+)*)>')

def header_entry(text):
    """The index entry for a code span naming a Seastar header, such as
    <seastar/core/gate.hh>, or None: a subentry of "headers, Seastar's",
    sorted by its path."""
    m = header_path.fullmatch(text)
    if not m:
        return None
    shown = '/\\allowbreak{}'.join(makeindex_escape(latex_escape(part))
                                    for part in text.split('/'))
    return f"headers@headers, Seastar's!{makeindex_escape(m.group(1))}@\\indexcode{{{shown}}}"

def load_seastar_options():
    return {line.strip() for line in (book / 'tools' / 'seastar_options.txt').read_text().splitlines()
            if line.strip() and not line.startswith('#')}

seastar_options = load_seastar_options()

def option_entry(text):
    """The index entry for a code span that begins with one of Seastar's
    command-line options, such as `--cpuset 0-3`, or None: the same entry
    as an index marker "command-line options!`--cpuset`" gives. The short
    forms -c and -m share an entry with --smp and --memory; written with
    their values, as in `-c2 -m1G`, they are part of a command line rather
    than the subject, and aren't indexed."""
    words = text.split()
    if not words:
        return None
    first = words[0].split('=')[0]
    if first in ('-c', '--smp'):
        shown = '`-c`, `--smp`'
    elif first in ('-m', '--memory'):
        shown = '`-m`, `--memory`'
    elif first in seastar_options:
        shown = f'`{first}`'
    else:
        return None
    return index_attribute(f'command-line options!{shown}')

def code_entry(text, explicit=False):
    """The index entry for a code span naming a Seastar name, a listed
    standard library name, a Seastar header, or one of Seastar's
    command-line options, or None."""
    return seastar_entry(text, explicit) or std_entry(text) or header_entry(text) or option_entry(text)

def index_part(part, top):
    """One level of an index marker's entry, in makeindex's syntax: kept as
    it is if it has an "@" (an explicit sort key); otherwise, text in
    backticks is set in code font, and a Seastar name in backticks at the
    top level gives the same entry as the automatic index does."""
    if '@' in part:
        return part
    code = re.fullmatch(r'`([^`]+)`', part)
    if code and top:
        entry = code_entry(code.group(1), explicit=True)
        if entry:
            return entry
    if '`' not in part:
        return makeindex_escape(latex_escape(part))
    # A standard library name sorts by its name without "std::", like a
    # Seastar name, so `std::future` stands next to `seastar::future`.
    key = re.sub(r'^-+', '', part.replace('`', '')).removeprefix('std::')
    # A function written as a call, `write()`, sorts as the automatic index
    # sorts it, by its bare name, so that the two give one entry.
    if re.fullmatch(r'`[^`]+\(\)`', part):
        key = key.removesuffix('()')
    shown = ''.join(index_code(t) if i % 2 else makeindex_escape(latex_escape(t))
                    for i, t in enumerate(part.split('`')))
    return f'{makeindex_escape(key)}@{shown}'

def index_attribute(value):
    """An index marker's "index" attribute, in makeindex's syntax ("!"
    between levels, "|see{...}" at the end) but with backticks for code."""
    value, bar, suffix = value.partition('|')
    # A cross-reference's target may have code in backticks too.
    see = re.fullmatch(r'(see|seealso)\{(.*)\}', suffix)
    if see and '`' in see.group(2):
        target = ''.join(index_code(t) if i % 2 else makeindex_escape(latex_escape(t))
                         for i, t in enumerate(see.group(2).split('`')))
        suffix = f'{see.group(1)}{{{target}}}'
    return '!'.join(index_part(p, i == 0) for i, p in enumerate(value.split('!'))) + bar + suffix

def index_command(entry, main=False):
    # \leavevmode first, so that an \index at the start of a paragraph is in
    # the paragraph, not before it. In a table cell, an \index before the
    # paragraph pushes the cell's text a line below the rest of its row.
    return RawInline('latex', f'\\leavevmode\\index{{{entry}{"|textbf" if main else ""}}}')

def index_span(attr, content, fmt):
    """An index marker: its text, preceded by \\index in LaTeX."""
    ident, classes, kvs = attr
    kvs = dict(kvs)
    if 'index' not in classes and 'index' not in kvs:
        return None
    if fmt != 'latex':
        return content
    entry = kvs.get('index')
    if entry is None:
        entry = makeindex_escape(latex_escape(stringify(content)))
    else:
        entry = index_attribute(entry)
    # The class "indexed" tells auto_index() that this text is indexed.
    return [index_command(entry, 'main' in classes), Span(['', ['indexed'], []], content)]

def section_ranges(blocks):
    """Indexes the sections whose heading has an "index" attribute as page
    ranges: \\index{entry|(} after the heading, \\index{entry|)} before the
    next heading of the same or a higher level."""
    out, open_ranges = [], []
    def close(level):
        while open_ranges and open_ranges[-1][0] >= level:
            out.append(RawBlock('latex', f'\\index{{{open_ranges.pop()[1]}|)}}'))
    for b in blocks:
        if b['t'] == 'Header':
            level, (ident, classes, kvs), content = b['c']
            close(level)
            out.append(b)
            entry = dict(kvs).get('index')
            if entry:
                entry = index_attribute(entry)
                out.append(RawBlock('latex', f'\\index{{{entry}|(}}'))
                open_ranges.append((level, entry))
            continue
        if b['t'] == 'Div' and any(c in b['c'][0][1] for c in ('part', 'mainmatter', 'appendices', 'backmatter')):
            close(0)
        out.append(b)
    close(0)
    return out

def auto_index(x, seen):
    """Indexes Seastar's names in code spans, in place, at their first
    mention in each section: inserts \\index before the code span, or, for a
    code span in a heading, after the heading."""
    if isinstance(x, dict):
        # Not in a footnote: LaTeX writes an \index there differently
        # ("\texttt  {"), so makeindex would list it as a separate entry.
        if x.get('t') not in ('CodeBlock', 'RawBlock', 'RawInline', 'Note'):
            auto_index(x.get('c'), seen)
        return
    if not isinstance(x, list):
        return
    out = []
    for e in x:
        if isinstance(e, dict) and e.get('t') == 'Header':
            seen.clear()
            entries = []
            def collect(y):
                if isinstance(y, dict):
                    if y.get('t') == 'Code':
                        entry = None if 'noindex' in y['c'][0][1] else code_entry(y['c'][1])
                        if entry and entry not in seen:
                            seen.add(entry)
                            entries.append(entry)
                    else:
                        collect(y.get('c'))
                elif isinstance(y, list):
                    for z in y:
                        collect(z)
            collect(e['c'][2])
            out.append(e)
            if entries:
                out.append(RawBlock('latex', ''.join(f'\\index{{{en}}}' for en in entries)))
            continue
        if isinstance(e, dict) and e.get('t') == 'Code':
            entry = None if 'noindex' in e['c'][0][1] else code_entry(e['c'][1])
            if entry and entry not in seen:
                seen.add(entry)
                out.append(index_command(entry))
        elif isinstance(e, dict) and e.get('t') == 'Span' and is_index_span(e['c'][0]):
            # A span indexed by hand, such as [`sstring`]{index="..." .main},
            # already marks this page: don't index the names in it again,
            # here or later in the section.
            mark_seen(e['c'][1], seen)
        else:
            auto_index(e, seen)
        out.append(e)
    x[:] = out

def is_index_span(attr):
    """Whether a Span is the text of an index marker (see index_span())."""
    return 'indexed' in attr[1]

def mark_seen(x, seen):
    """Records the Seastar names in the code spans of x as indexed."""
    if isinstance(x, dict):
        if x.get('t') == 'Code':
            entry = code_entry(x['c'][1])
            if entry:
                seen.add(entry)
        else:
            mark_seen(x.get('c'), seen)
    elif isinstance(x, list):
        for y in x:
            mark_seen(y, seen)

def drop_heading_index(key, value, fmt, meta):
    """Outside LaTeX, a heading's "index" attribute means nothing, so it
    doesn't go into the output (HTML would keep it as data-index)."""
    if key == 'Header' and any(k == 'index' for k, _ in value[1][2]):
        value[1][2] = [[k, v] for k, v in value[1][2] if k != 'index']

# Figures: SVG files in figures/, which build.py also converts to PDF for
# LaTeX, in gen/figures next to the labels file. A figure is written
#
#     ![Its caption.](figures/sharing.svg){#fig:sharing}
#
# and referred to as [Figure](#fig:sharing), which becomes "Figure 1.2".
# LaTeX numbers the figures itself; in the other formats, the caption gets
# the number.

# The grayscale edition uses the gray figures that build.py --gray makes.
figures_pdf = Path(os.environ['BOOK_LABELS']).parent / ('figures-gray' if os.environ.get('BOOK_GRAY') else 'figures')

def latex_figure_image(value):
    attr, alt, (src, title) = value
    if src.startswith('figures/') and src.endswith('.svg'):
        pdf = figures_pdf / (Path(src).stem + '.pdf')
        return {'t': 'Image', 'c': [attr, alt, [str(pdf.resolve()), title]]}
    return None

def placed_figure(value):
    """A figure written with placement="tb" (or other LaTeX placement
    letters), as in ![...](figures/x.svg){#fig:x placement="tb"}, gets that
    placement in the PDF, instead of the default, htbp: for a figure that
    must come before the text that refers to it (as when a long table or
    listing follows the reference), and so must not appear "here".
    Returns None for a figure without it."""
    attr, caption, body = value
    for block in body:
        for inline in block['c'] if block['t'] in ('Plain', 'Para') else []:
            if inline['t'] == 'Image':
                kvs = inline['c'][0][2]
                placement = dict(kvs).get('placement')
                if placement:
                    inline['c'][0][2] = [[k, v] for k, v in kvs if k != 'placement']
                    return [RawBlock('latex', f'\\makeatletter\\def\\fps@figure{{{placement}}}\\makeatother'),
                            {'t': 'Figure', 'c': [attr, caption, body]},
                            RawBlock('latex', '\\makeatletter\\def\\fps@figure{htbp}\\makeatother')]
    return None

def sideways_figure(value):
    """A figure written with sideways="true", as in
    ![...](figures/x.svg){#fig:x sideways="true"}, is turned on its side in
    the PDF, on a page of its own, as wide as the page is tall: for a wide
    picture whose labels would be too small across the page, such as a
    flame graph. Returns None for a figure without it."""
    attr, (short, blocks), body = value
    for block in body:
        for inline in block['c'] if block['t'] in ('Plain', 'Para') else []:
            if inline['t'] == 'Image':
                kvs = inline['c'][0][2]
                if dict(kvs).get('sideways') != 'true':
                    return None
                src = inline['c'][2][0]
                pdf = (figures_pdf / (Path(src).stem + '.pdf')).resolve()
                caption = blocks[0]['c'] if blocks and blocks[0]['t'] in ('Plain', 'Para') else []
                return [RawBlock('latex', '\\begin{sidewaysfigure}\\centering\n'
                                 f'\\includegraphics[width=\\textheight,height=0.8\\textwidth,keepaspectratio]{{{pdf}}}'),
                        {'t': 'Plain', 'c': [RawInline('latex', '\\caption{'), *caption,
                                             RawInline('latex', f'}}\\label{{{attr[0]}}}')]},
                        RawBlock('latex', '\\end{sidewaysfigure}')]
    return None

def drop_sideways(value):
    """The sideways attribute means nothing outside the PDF."""
    for block in value[2]:
        for inline in block['c'] if block['t'] in ('Plain', 'Para') else []:
            if inline['t'] == 'Image':
                inline['c'][0][2] = [[k, v] for k, v in inline['c'][0][2] if k != 'sideways']

def number_figure(value):
    attr, (short, blocks), body = value
    entry = labels.get(attr[0])
    if entry and blocks and blocks[0]['t'] in ('Plain', 'Para'):
        blocks[0]['c'][:0] = [Str(f'Figure {entry["number"]}:'), Space()]
    return {'t': 'Figure', 'c': [attr, [short, blocks], body]}

def page_id(label, fmt):
    """The identifier to give a chapter, appendix or part. In the HTML edition
    with a page for each chapter (Pandoc's chunkedhtml), it names the page's
    file, so "ch:stalls" becomes "ch-stalls": a colon in a link would look
    like a URL scheme. (Keep the prefix: without it, the chapter "stalls"
    would clash with a section called "Stalls".)"""
    return label.replace(':', '-', 1) if fmt in ('chunkedhtml', 'gfm') else label

def gfm_anchor(ident):
    """GitHub ignores the identifiers that Pandoc gives headings, and makes
    its own from their text, so references to chapters in the Markdown
    edition (Pandoc's gfm) need an anchor of their own."""
    return RawBlock('html', f'<a id="{ident}"></a>')

def part(attr, blocks, fmt):
    label = attr[0]
    entry = labels[label]
    title = stringify(blocks[0]['c'][2]) if blocks and blocks[0]['t'] == 'Header' else entry['title']
    intro = [b for b in blocks if b['t'] != 'Header']
    if fmt == 'latex':
        return ([RawBlock('latex', '\\setpartpreamble[uc][0.72\\linewidth]{\\partintro{}')]
                + intro
                + [RawBlock('latex', '}'),
                   RawBlock('latex', f'\\part{{{latex_escape(title)}}}\\label{{{label}}}')])
    if fmt == 'gfm':
        heading = Header(1, [page_id(label, fmt), ['unnumbered'], []],
                         [Str(f'Part {entry["number"]}: {title}')])
        return [gfm_anchor(page_id(label, fmt)), heading] + intro
    number = Span(['', ['part-number'], []], [Str('Part ' + entry['number'])])
    if fmt.startswith(('html', 'epub', 'chunkedhtml')):
        heading = Header(1, [page_id(label, fmt), ['unnumbered', 'part'], []],
                         [number, Space(), Str(title)])
    else:
        heading = Header(1, [label, ['unnumbered'], []], [Str(f'Part {entry["number"]}: {title}')])
    return [Div(['', ['part-intro'], []], [heading] + intro)]

def box_kind(blocks):
    """A quotation whose first paragraph starts with a bold title is not a
    quotation but a box: a C++ corner, if the title starts with "C++
    corner:", or else a sidebar, such as "About the name". Draft notes,
    which start with a bold "Draft note", stay quotations. Returns 'corner',
    'sidebar' or None."""
    if not blocks or blocks[0]['t'] != 'Para' or not blocks[0]['c']:
        return None
    first = blocks[0]['c'][0]
    if first['t'] != 'Strong':
        return None
    title = stringify(first)
    if title.startswith('Draft note'):
        return None
    return 'corner' if title.startswith('C++ corner') else 'sidebar'

def box(kind, blocks, fmt):
    """Sets a C++ corner or a sidebar apart from the text in a box of its own
    (the environments cppcorner and sidebar in style/book.tex, the classes
    corner and sidebar in CSS), so that it doesn't look like a quotation."""
    if fmt == 'latex':
        env = 'cppcorner' if kind == 'corner' else 'sidebar'
        return ([RawBlock('latex', f'\\begin{{{env}}}')] + blocks
                + [RawBlock('latex', f'\\end{{{env}}}')])
    return [Div(['', [kind], []], blocks)]

def table_cells(value, with_head=False):
    """Every cell of a table, as a (column, blocks) pair, or with
    with_head, a (column, blocks, in_head) triple."""
    attr, caption, colspecs, head, bodies, foot = value
    rows = [(row, True) for row in head[1]] + [(row, False) for row in foot[1]]
    for body in bodies:
        rows += [(row, True) for row in body[2]] + [(row, False) for row in body[3]]
    for row, in_head in rows:
        column = 0
        for cell in row[1]:
            _, _, _, colspan, blocks = cell
            yield (column, blocks, in_head) if with_head else (column, blocks)
            column += colspan

def text_width(x):
    """The width of the text in a list of blocks or inlines, in characters
    of text; code is wider, by about a quarter."""
    if isinstance(x, list):
        return sum(text_width(y) for y in x)
    if not isinstance(x, dict):
        return 0
    if x.get('t') == 'Str':
        return len(x['c'])
    if x.get('t') in ('Space', 'SoftBreak'):
        return 1
    if x.get('t') == 'Code':
        return len(x['c'][1]) * 1.25
    return text_width(x.get('c'))

def fit_table(value):
    """LaTeX: makes every table span the width of the text, with columns in
    proportion to their longest cells. Pandoc sizes a table whose Markdown
    lines are long by the dashes under its header, so a column of long code
    can get too little room and overflow into the next, and it sets a table
    of short lines at its natural width, narrower than the text."""
    colspecs = value[2]
    widths = [0] * len(colspecs)
    words = [0] * len(colspecs)
    body = [0] * len(colspecs)
    for column, blocks, in_head in table_cells(value, with_head=True):
        widths[column] = max(widths[column], text_width(blocks))
        words[column] = max(words[column], longest_word(blocks))
        if not in_head:
            body[column] = max(body[column], text_width(blocks))
    padded = [w + 3 for w in widths]
    shares = [w / sum(padded) for w in padded]
    # A column must still be wide enough for its longest word, which can't
    # break, such as `without_preemption_check`. Give it that, taken from
    # the columns that have more room than their longest cell below the
    # header needs (a header can wrap), so that their cells don't wrap.
    needed = [(w + 3) / LINE_CHARS for w in words]
    room = [max(n, (b + 3) / LINE_CHARS) for n, b in zip(needed, body)]
    short = sum(max(0, n - s) for s, n in zip(shares, needed))
    spare = sum(max(0, s - r) for s, r in zip(shares, room))
    if short and spare > short:
        shares = [n if s < n else s - max(0, s - r) * short / spare
                  for s, n, r in zip(shares, needed, room)]
    elif short:
        # Not enough spare room: take it anyway, from the columns wider than
        # their longest word, whose cells will wrap to more lines. Code that
        # overflows into the next column is worse.
        excess = sum(max(0, s - n) for s, n in zip(shares, needed))
        if excess > short:
            shares = [n if s < n else s - (s - n) * short / excess
                      for s, n in zip(shares, needed)]
    for spec, share in zip(colspecs, shares):
        spec[1] = {'t': 'ColWidth', 'c': share}
    return {'t': 'Table', 'c': value}

# About how many characters of text fit across the PDF's text width.
LINE_CHARS = 95

def longest_word(x):
    """The width of the widest word in a list of blocks or inlines, which
    a line can't break, in the units of text_width()."""
    if isinstance(x, list):
        return max((longest_word(y) for y in x), default=0)
    if not isinstance(x, dict):
        return 0
    if x.get('t') == 'Str':
        return len(x['c'])
    if x.get('t') == 'Code':
        # A word that must fit is measured more closely than text_width()
        # measures code: Source Code Pro is about 1.4 times as wide.
        return max(len(w) for w in re.split(r'(?<=::)|(?<=/)| ', x['c'][1])) * 1.4
    return longest_word(x.get('c'))

def header_name(text):
    """A header's name in code font, such as <seastar/core/when_all.hh>,
    allowed to break after each "/", because such a name is too long to
    leave unbroken at the end of a line."""
    parts = [latex_escape(p) for p in text.split('/')]
    return RawInline('latex', '\\texttt{' + '/\\allowbreak{}'.join(parts) + '}')

def header_names(key, value, fmt, meta):
    """Sets headers' names, such as <seastar/core/when_all.hh>, so that
    they may break after each "/". It runs after auto_index(), which needs
    them still as code spans."""
    if key == 'Code' and re.fullmatch(r'<[\w.+-]+(/[\w.+-]+)+>', value[1]):
        return header_name(value[1])

def action(key, value, fmt, meta):
    global region
    if key == 'Span' and 'text' in dict(value[0][2]):
        # [$$...$$]{text="..."}: math that plain text can't draw (Pandoc
        # would show its TeX), with a form for plain text to use instead.
        if fmt == 'plain':
            return Str(dict(value[0][2])['text'])
        return value[1]
    if key == 'Table' and fmt == 'latex':
        return fit_table(value)
    if key == 'Header' and fmt == 'gfm' and value[0] == 1 and value[1][0] in labels:
        # Markdown doesn't number headings: name chapters and appendices in
        # their headings, as the PDF does, and give them anchors.
        level, (label, classes, kvs), content = value
        entry = labels[label]
        if entry['kind'] in ('chapter', 'appendix'):
            content = [Str(f'{entry["kind"].capitalize()} {entry["number"]}:'), Space()] + content
        ident = page_id(label, fmt)
        return [gfm_anchor(ident), Header(level, [ident, classes, kvs], content)]
    if key == 'Header' and value[0] == 1 and value[1][0].startswith('sol:') and fmt != 'latex':
        # Elsewhere, Pandoc would number the solution chapters 1, 2, 3...:
        # name the book's chapter instead.
        level, (ident, classes, kvs), content = value
        entry = labels[ident]
        kind = 'Chapter' if entry['of'].startswith('ch:') else 'Appendix'
        return Header(level, [page_id(ident, fmt), classes + ['unnumbered'], kvs],
                      [Str(f'{kind} {entry["number"]}:'), Space()] + content)
    if key == 'Header' and value[0] == 1 and value[1][0].startswith('sol:') and fmt == 'latex':
        # A chapter of the book of solutions has the number of the book's
        # chapter whose exercises it solves: chapter 13's solutions are
        # chapter 13 there too.
        number = labels[value[1][0]]['number']
        if number.isdigit():
            return [RawBlock('latex', f'\\setcounter{{chapter}}{{{int(number) - 1}}}'),
                    {'t': 'Header', 'c': value}]
    if key == 'Header' and value[0] == 1:
        value[1][0] = page_id(value[1][0], fmt)
    if key == 'CodeBlock' and any(k == 'include' for k, _ in value[0][2]):
        return include_code(value[0], value[1], fmt)
    if key == 'Span':
        return index_span(value[0], value[1], fmt)
    if key == 'BlockQuote' and box_kind(value):
        if fmt == 'latex':
            # The box's blocks are returned at the top level of a list,
            # where walk() doesn't call this function on them, so a code
            # block that includes a file would stay empty: process them
            # first. (Elsewhere they go in a Div, whose contents walk()
            # processes.)
            value = walk(value, action, fmt, meta)
        return box(box_kind(value), value, fmt)
    if key == 'Image' and fmt == 'latex':
        return latex_figure_image(value)
    if key == 'Figure' and fmt == 'latex':
        return sideways_figure(value) or placed_figure(value)
    if key == 'Figure' and fmt != 'latex':
        drop_sideways(value)
        return number_figure(value)
    if key == 'Link':
        attr, content, (target, title) = value
        if target.startswith(('#ch:', '#app:', '#part:', '#fig:')):
            new = reference(content, target)
            if not target.startswith('#fig:'):
                target = '#' + page_id(target[1:], fmt)
            return {'t': 'Link', 'c': [attr, new if new is not None else content, [target, title]]}
    elif key == 'Div':
        attr, blocks = value
        classes = attr[1]
        if 'part' in classes:
            return part(attr, blocks, fmt)
        for marker, latex in (('mainmatter', '\\realmainmatter'), ('appendices', '\\appendix'),
                              ('backmatter', '\\backmatter')):
            if marker in classes:
                region = {'mainmatter': 'main', 'appendices': 'appendix', 'backmatter': 'back'}[marker]
                return [RawBlock('latex', latex)] if fmt == 'latex' else []
    elif key == 'Header' and fmt == 'latex' and value[0] == 1 and 'unnumbered' in value[1][1] \
            and 'part' not in value[1][1]:
        # An unnumbered chapter, such as the Preface, becomes \chapter*, which
        # doesn't set the running headers, so its pages would show the
        # previous chapter's title. Set them to this chapter's title.
        title = latex_escape(stringify(value[2]))
        return [{'t': 'Header', 'c': value}, RawBlock('latex', f'\\markboth{{{title}}}{{{title}}}')]
    elif key == 'Header' and fmt != 'latex' and value[0] >= 2 and 'unnumbered' not in value[1][1]:
        # Like the PDF, number only the chapters, not the sections in them.
        level, (ident, classes, kvs), content = value
        return Header(level, [ident, classes + ['unnumbered'], kvs], content)
    elif key == 'Header' and fmt == 'plain' and value[0] == 1 and value[1][0] in labels \
            and labels[value[1][0]]['kind'] == 'chapter':
        # Pandoc's plain-text writer doesn't number headings.
        level, (ident, classes, kvs), content = value
        number = labels[ident]['number']
        return Header(level, [ident, classes, kvs], [Str(f'Chapter {number}:'), Space()] + content)
    elif key == 'Header' and fmt != 'latex' and region != 'main':
        # Outside the main matter, LaTeX numbers (or doesn't number) headings
        # by itself; elsewhere, number nothing, and label appendices by letter.
        level, (ident, classes, kvs), content = value
        if 'unnumbered' not in classes:
            classes = classes + ['unnumbered']
            label = ident if ident in labels else ident.replace('-', ':', 1)
            if level == 1 and label in labels and labels[label]['kind'] == 'appendix':
                content = [Str(f'Appendix {labels[label]["number"]}:'), Space()] + content
            return Header(level, [ident, classes, kvs], content)

def mark_inline(block, mark):
    """Appends the inline mark to the last line of text in the block, if it
    has one, looking into the last item of a list or a quotation; returns
    whether it did."""
    if block['t'] in ('Para', 'Plain'):
        block['c'].append(mark)
        return True
    if block['t'] in ('BulletList', 'OrderedList'):
        items = block['c'] if block['t'] == 'BulletList' else block['c'][1]
        return bool(items) and bool(items[-1]) and mark_inline(items[-1][-1], mark)
    if block['t'] in ('BlockQuote', 'Div'):
        blocks = block['c'] if block['t'] == 'BlockQuote' else block['c'][1]
        return bool(blocks) and mark_inline(blocks[-1], mark)
    return False

def place_section_marks(blocks):
    """The running header of a right page in the PDF shows the section in
    force at the top of the page (style/book.tex). For that to work when
    the page breaks right before a section's heading, the section's mark
    must come before the point where the page breaks, not after it, as the
    heading's own mark does. So put it at the end of the last line of text
    before the heading: TeX puts a mark from a paragraph right after the
    line that contains it, before the space and break point that follow
    the paragraph. When there is no such line (for example, after a code
    block), put the mark just before the heading instead."""
    out = []
    for block in blocks:
        if block['t'] == 'Header' and block['c'][0] == 2:
            title = latex_escape(stringify(block['c'][2]))
            command = f'\\markright{{{title}}}'
            if not (out and mark_inline(out[-1], RawInline('latex', command))):
                out.append(RawBlock('latex', command))
        out.append(block)
    return out

def split_top(text, seps):
    """Splits a makeindex entry at the separators in seps that aren't
    inside braces or quoted with '"'."""
    parts, depth, cur, quoted = [], 0, '', False
    for c in text:
        if quoted:
            cur += c
            quoted = False
        elif c == '"':
            cur += c
            quoted = True
        elif c == '{':
            depth += 1
            cur += c
        elif c == '}':
            depth -= 1
            cur += c
        elif c in seps and depth == 0:
            parts.append(cur)
            cur = ''
        else:
            cur += c
    return parts + [cur]

def index_entries(x, out):
    """Appends to out the entries of all the \\index commands in x."""
    if isinstance(x, dict):
        if x.get('t') in ('RawBlock', 'RawInline') and x['c'][0] == 'latex':
            text = x['c'][1]
            for m in re.finditer(r'\\index\{', text):
                depth, i = 1, m.end()
                while depth and i < len(text):
                    depth += {'{': 1, '}': -1}.get(text[i], 0)
                    i += 1
                out.append(text[m.end():i - 1])
        else:
            index_entries(x.get('c'), out)
    elif isinstance(x, list):
        for y in x:
            index_entries(y, out)

def member_see_entries(blocks):
    """For each member of a Seastar class in the index, such as
    seastar::file's dma_read(), an entry for the member's bare name that
    refers to the classes it's listed under, "dma_read(), see
    seastar::file", for a reader who remembers the name but not the class.
    Returns them as a RawBlock of \\index commands."""
    entries = []
    index_entries(blocks, entries)
    members = {}
    for entry in entries:
        levels = split_top(split_top(entry, '|')[0], '!')
        if len(levels) != 2 or '@' not in levels[0] or '@' not in levels[1]:
            continue
        cls_key, cls_shown = split_top(levels[0], '@')[:2]
        sub_key, sub_shown = split_top(levels[1], '@')[:2]
        if cls_shown.startswith('\\indexcode{seastar::') and sub_shown.startswith('\\indexcode{'):
            members.setdefault((sub_key, sub_shown), {})[cls_key] = cls_shown
    commands = ''.join(
        f'\\index{{{key}@{shown}|see{{{", ".join(c for _, c in sorted(classes.items()))}}}}}'
        for (key, shown), classes in sorted(members.items()))
    return RawBlock('latex', commands)

if __name__ == '__main__':
    doc = json.loads(sys.stdin.read())
    fmt = sys.argv[1] if len(sys.argv) > 1 else ''
    if fmt != 'latex':
        doc = walk(doc, drop_heading_index, fmt, doc.get('meta', {}))
    doc = walk(doc, action, fmt, doc.get('meta', {}))
    if fmt == 'latex':
        doc['blocks'] = section_ranges(doc['blocks'])
        auto_index(doc['blocks'], set())
        doc = walk(doc, header_names, fmt, doc.get('meta', {}))
        doc['blocks'] = place_section_marks(doc['blocks'])
        # The book of solutions has no index: the book's covers it.
        if not os.environ.get('BOOK_SOLUTIONS'):
            doc['blocks'].append(member_see_entries(doc['blocks']))
            doc['blocks'].append(RawBlock('latex', '\\printindex'))
    json.dump(doc, sys.stdout)
