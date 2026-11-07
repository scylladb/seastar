#!/usr/bin/python3 -I
# Pandoc filter for the book, run by tools/build.py. It
#  - replaces references by label with numbers: "[chapter](#ch:coroutines)"
#    becomes "chapter 4", "[appendix](#app:glossary)" becomes "appendix E" and
#    "[Part](#part:io)" becomes "Part IV" (an empty link text gives the
#    lowercase word); a link with any other text keeps its text;
#  - renders the part divisions that build.py inserts (a Div with class
#    "part"): a part page in LaTeX, a heading in the other formats;
#  - switches between front matter, main matter, appendices and back matter
#    at the marker Divs that build.py inserts;
#  - fills code blocks from the example programs, so that the book shows code
#    that really compiles and runs (see include_code() below).
# The numbers come from the JSON file named by the BOOK_LABELS environment
# variable, which build.py writes from metadata.yaml.

import json
import os
import re
import sys
import textwrap
from pathlib import Path
from pandocfilters import (walk, stringify, RawBlock, RawInline, Header, Div, Str,
                           Space, Span)

labels = json.load(open(os.environ['BOOK_LABELS']))
words = {'chapter': 'chapter', 'appendix': 'appendix', 'part': 'Part'}
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
snippet_marker = re.compile(r'^\s*(?://|#) (end )?snippet: ([\w-]+)\s*$')

def include_code(attrs, text):
    """A code block such as
        ```{.cpp include="examples/tutorial-introduction/hello.cc"}
        ```
    shows that whole file (the path is relative to the book's directory), and
    with snippet="name" only the lines between "// snippet: name" and
    "// end snippet: name" in it, dedented (in files whose comments start
    with "#", such as CMakeLists.txt, the markers start with "#" too).
    Lines with these markers never appear in the book."""
    kvs = dict(attrs[2])
    path = book / kvs['include']
    if not path.exists():
        sys.exit(f'bookfilter: included file {kvs["include"]} does not exist')
    if text.strip():
        sys.exit(f'bookfilter: a code block including {kvs["include"]} must be empty')
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
    kept = [[k, v] for k, v in attrs[2] if k not in ('include', 'snippet')]
    return {'t': 'CodeBlock', 'c': [[attrs[0], attrs[1], kept], code]}

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

def action(key, value, fmt, meta):
    global region
    if key == 'Header' and fmt == 'gfm' and value[0] == 1 and value[1][0] in labels:
        # Markdown doesn't number headings: name chapters and appendices in
        # their headings, as the PDF does, and give them anchors.
        level, (label, classes, kvs), content = value
        entry = labels[label]
        if entry['kind'] in ('chapter', 'appendix'):
            content = [Str(f'{entry["kind"].capitalize()} {entry["number"]}:'), Space()] + content
        ident = page_id(label, fmt)
        return [gfm_anchor(ident), Header(level, [ident, classes, kvs], content)]
    if key == 'Header' and value[0] == 1:
        value[1][0] = page_id(value[1][0], fmt)
    if key == 'CodeBlock' and any(k == 'include' for k, _ in value[0][2]):
        return include_code(value[0], value[1])
    if key == 'Link':
        attr, content, (target, title) = value
        if target.startswith(('#ch:', '#app:', '#part:')):
            new = reference(content, target)
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

if __name__ == '__main__':
    doc = json.loads(sys.stdin.read())
    fmt = sys.argv[1] if len(sys.argv) > 1 else ''
    doc = walk(doc, action, fmt, doc.get('meta', {}))
    if fmt == 'latex':
        doc['blocks'] = place_section_marks(doc['blocks'])
    json.dump(doc, sys.stdout)
