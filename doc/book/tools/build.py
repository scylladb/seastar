#!/usr/bin/python3 -I
# Build the book (or any Pandoc Markdown file) into PDF, HTML (a single page,
# or a page for each chapter), EPUB, AZW3, GitHub Markdown and plain text.
#
# Usage: build.py [--formats pdf,html,html-chapters,epub,azw3,md,txt]
#                 [--paper b5|a4|letter]
#                 [--gray] OUTDIR [INPUT.md...]
#
# Without INPUT files, builds the book itself, into OUTDIR/book.*: the chapters
# listed in metadata.yaml, in that order, with the parts and the switches
# between front matter, main matter, appendices and back matter between them,
# run through tools/bookfilter.py (which numbers references by label).
#
# The PDF uses LuaLaTeX with KOMA-Script's scrbook class and style/book.tex.
# Fonts are Libertinus Serif and Source Code Pro when installed (Fedora:
# texlive-libertinus-fonts, texlive-sourcecodepro); otherwise LaTeX's default
# Latin Modern is used.

import argparse
import datetime
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

import yaml

book = Path(__file__).resolve().parent.parent
style = book / 'style'

def have_tex_file(filename):
    return subprocess.run(['kpsewhich', filename], capture_output=True).returncode == 0

def pandoc(inputs, *args, reader='markdown+pandoc_title_block'):
    cmd = ['pandoc', '-f', reader, *map(str, inputs), *args, *book_args]
    subprocess.run(cmd, check=True)

def roman(n):
    return ['I', 'II', 'III', 'IV', 'V', 'VI', 'VII', 'VIII', 'IX', 'X'][n - 1]

def book_inputs(gen):
    """Lay out the book from metadata.yaml: returns the input files, in order,
    and writes the part and marker files and the labels file into gen."""
    structure = yaml.safe_load((book / 'metadata.yaml').read_text().split('---')[1])['book']
    gen.mkdir(parents=True, exist_ok=True)
    labels, inputs = {}, [book / 'metadata.yaml']

    def chapter(name, label=None):
        path = book / 'chapters' / f'{name}.md'
        if not path.exists():
            sys.exit(f'build.py: {path} is listed in metadata.yaml but does not exist')
        text = path.read_text()
        first = text.split('\n', 1)[0]
        if label and not re.match(r'^# .*\{#' + re.escape(label) + r'[ }]', first):
            sys.exit(f'build.py: {path} must start with a heading labeled {{#{label}}}')
        # Figures are numbered within their chapter or appendix, in order,
        # as LaTeX numbers them: "Figure 5.2", "Figure A.1".
        if label:
            for i, fig in enumerate(re.findall(r'\{#(fig:[\w-]+)', text), 1):
                labels[fig] = {'kind': 'figure', 'number': f'{labels[label]["number"]}.{i}'}
        inputs.append(path)

    def marker(name):
        path = gen / f'{name}.md'
        path.write_text(f'::: {{.{name}}}\n:::\n')
        inputs.append(path)

    for name in structure.get('front', []):
        chapter(name)
    marker('mainmatter')
    number = 0
    for i, part in enumerate(structure['parts'], 1):
        label = f'part:{part["id"]}'
        labels[label] = {'kind': 'part', 'number': roman(i), 'title': part['title']}
        path = gen / f'part-{part["id"]}.md'
        path.write_text(f'::: {{.part #{label}}}\n# {part["title"]}\n\n{part.get("intro", "")}\n:::\n')
        inputs.append(path)
        for name in part['chapters']:
            number += 1
            labels[f'ch:{name}'] = {'kind': 'chapter', 'number': str(number)}
            chapter(name, f'ch:{name}')
    if structure.get('appendices'):
        marker('appendices')
        for i, name in enumerate(structure['appendices']):
            labels[f'app:{name}'] = {'kind': 'appendix', 'number': chr(ord('A') + i)}
            chapter(name, f'app:{name}')
    marker('backmatter')
    for name in structure.get('back', []):
        chapter(name)
    labels_file = gen / 'labels.json'
    labels_file.write_text(json.dumps(labels, indent=1))
    return inputs, labels_file

def solutions_title(gen):
    """The LaTeX header that adds "solutions book" to the title page."""
    path = gen / 'solutions-title.tex'
    path.write_text('\\newcommand{\\titlepageextra}{solutions book}\n')
    return path

def exercises(chapter_text):
    """The exercises of a chapter, from its "## Exercises" section: a dict
    from each exercise's number to its Markdown text, not indented."""
    m = re.search(r'^## Exercises[^\n]*\n(.*?)(?=^#|\Z)', chapter_text, re.M | re.S)
    if not m:
        return {}
    items = re.split(r'^(\d+)\. ', m.group(1), flags=re.M)
    return {int(n): '\n'.join(line[3:] if line.startswith('   ') else line
                              for line in text.rstrip().split('\n'))
            for n, text in zip(items[1::2], items[2::2])}

def with_questions(solution, chapter_text, name):
    """A solution chapter with each exercise's question, from the book's
    chapter, inserted after its "## Exercise N" heading, as a sidebar."""
    questions = exercises(chapter_text)
    out = []
    for line in solution.split('\n'):
        out.append(line)
        m = re.match(r'^## Exercise (\d+)\s*$', line)
        if m:
            n = int(m.group(1))
            if n not in questions:
                sys.exit(f'build.py: solutions/{name}.md solves exercise {n}, '
                         f'which chapters/{name}.md does not have')
            text = questions[n].split('\n')
            out += ['', '> **The exercise.** ' + text[0]] + \
                   [('> ' + l) if l else '>' for l in text[1:]]
    return '\n'.join(out)

def solutions_inputs(gen):
    """The book of solutions (--solutions): solutions/NAME.md for each of the
    book's chapters or appendices that has one, in the book's order. Returns
    the input files and a labels file with all of the book's labels, so that
    references to the book's chapters, parts and figures work, and one for
    each solution chapter, "sol:NAME", with its chapter's number, which the
    filter gives the solution chapter too."""
    _, book_labels = book_inputs(gen)
    labels = json.loads(book_labels.read_text())
    structure = yaml.safe_load((book / 'metadata.yaml').read_text().split('---')[1])['book']
    # book_inputs() wrote the marker that starts the main matter, where
    # chapters are numbered.
    inputs = [book / 'solutions' / 'metadata.yaml', gen / 'mainmatter.md']
    names = [name for part in structure['parts'] for name in part['chapters']]
    names += structure.get('appendices', [])
    for name in names:
        path = book / 'solutions' / f'{name}.md'
        if not path.exists():
            continue
        label = f'sol:{name}'
        text = path.read_text()
        if not re.match(r'^# .*\{#' + re.escape(label) + r'[ }]', text.split('\n', 1)[0]):
            sys.exit(f'build.py: {path} must start with a heading labeled {{#{label}}}')
        book_label = f'ch:{name}' if f'ch:{name}' in labels else f'app:{name}'
        labels[label] = {'kind': 'solutions', 'of': book_label,
                         'number': labels[book_label]['number']}
        for i, fig in enumerate(re.findall(r'\{#(fig:[\w-]+)', text), 1):
            labels[fig] = {'kind': 'figure', 'number': f'{labels[label]["number"]}.S{i}'}
        full = gen / 'solutions' / f'{name}.md'
        full.parent.mkdir(exist_ok=True)
        full.write_text(with_questions(text, (book / 'chapters' / f'{name}.md').read_text(), name))
        inputs.append(full)
    labels_file = gen / 'labels-solutions.json'
    labels_file.write_text(json.dumps(labels, indent=1))
    return inputs, labels_file

# Page layouts: B5 (176x250mm) is close to the trim size of most printed
# technical books; A4 and US Letter are for printing at home, with a larger
# font and wide margins to keep lines at a readable length.
layouts = {
    'b5': ['-V', 'papersize=b5', '-V', 'fontsize=10pt', '-V', 'geometry=margin=22mm'],
    'a4': ['-V', 'papersize=a4', '-V', 'fontsize=11pt', '-V', 'geometry=margin=30mm'],
    'letter': ['-V', 'papersize=letter', '-V', 'fontsize=11pt', '-V', 'geometry=margin=30mm'],
}

# Code highlighting themes. 'color' is a restrained traditional scheme: bold
# navy keywords, italic green comments, dark red strings, everything else
# black. 'gray' is for print: bold keywords, italic gray comments.
token_types = ['Alert', 'Annotation', 'Attribute', 'BaseN', 'BuiltIn', 'Char', 'Comment',
               'CommentVar', 'Constant', 'ControlFlow', 'DataType', 'DecVal', 'Documentation',
               'Error', 'Extension', 'Float', 'Function', 'Import', 'Information', 'Keyword',
               'Operator', 'Other', 'Preprocessor', 'RegionMarker', 'SpecialChar',
               'SpecialString', 'String', 'Variable', 'VerbatimString', 'Warning']
keyword_types = ['Keyword', 'ControlFlow', 'DataType']
comment_types = ['Comment', 'CommentVar', 'Documentation', 'Annotation', 'Information', 'Warning']
string_types = ['String', 'Char', 'SpecialChar', 'SpecialString', 'VerbatimString']
themes = {
    'color': {**{k: ('#1a3c8c', True, False) for k in keyword_types},
              **{k: ('#3f7f5f', False, True) for k in comment_types},
              **{k: ('#a31515', False, False) for k in string_types},
              'Preprocessor': ('#1a3c8c', False, False), 'Import': ('#1a3c8c', False, False)},
    'gray': {**{k: (None, True, False) for k in keyword_types},
             **{k: ('#606060', False, True) for k in comment_types}},
}

def theme_file(outdir, name):
    styles = {k: {'text-color': None, 'background-color': None,
                  'bold': False, 'italic': False, 'underline': False} for k in token_types}
    for k, (color, bold, italic) in themes[name].items():
        styles[k].update({'text-color': color, 'bold': bold, 'italic': italic})
    path = outdir / f'{name}.theme'
    path.write_text(json.dumps({'text-color': None, 'background-color': '#f2f2f2',
                                'line-number-color': '#aaaaaa',
                                'line-number-background-color': None,
                                'text-styles': styles}, indent=1))
    return path

def title_art(outdir):
    """The title page's sea star (art/seastar.svg) as SVG, for the HTML, and
    as PDF, for LaTeX (rsvg-convert, Fedora: librsvg2-tools)."""
    svg = outdir / 'seastar.svg'
    shutil.copy(book / 'art' / 'seastar.svg', svg)
    pdf = outdir / 'gen' / 'seastar.pdf'
    subprocess.run(['rsvg-convert', '-f', 'pdf', '-o', pdf, svg], check=True)
    tex = outdir / 'gen' / 'titleart.tex'
    tex.write_text('\\usepackage{graphicx}\n'
                   f'\\newcommand{{\\titleart}}{{\\includegraphics[width=0.5\\linewidth]{{{pdf.resolve()}}}}}\n')
    html = outdir / 'gen' / 'titleart.html'
    html.write_text('<p class="title-art"><img src="seastar.svg" alt="A sea star"></p>\n')
    return tex, html

def figures(outdir, gray=False):
    """Typesets the book's figures, figures/NAME.tex (TikZ pictures, in the
    style of figures/style.tex), each in a document of its own, with the
    book's fonts: into outdir/gen/figures/NAME.pdf for LaTeX, where
    tools/bookfilter.py looks for it, and from that, with pdftocairo
    (poppler-utils), into outdir/figures/NAME.svg for the HTML editions.
    A figure is typeset again only when it, or the style, changed."""
    src = book / 'figures'
    if not src.is_dir():
        return
    # The grayscale edition (--gray) gets figures of its own, in gray, as
    # xcolor converts the colors, like the rest of that edition; only the
    # PDFs, since the HTML editions use the SVGs of the color ones.
    work = outdir / 'gen' / ('figures-gray' if gray else 'figures')
    work.mkdir(parents=True, exist_ok=True)
    (outdir / 'figures').mkdir(parents=True, exist_ok=True)
    style_file = src / 'style.tex'
    fonts = ''
    if have_tex_file('LibertinusSerif-Regular.otf'):
        fonts += '\\setmainfont{Libertinus Serif}\n\\setsansfont{Libertinus Sans}\n'
    if have_tex_file('SourceCodePro-Regular.otf'):
        fonts += '\\setmonofont{Source Code Pro}[Scale=MatchLowercase]\n'
    for fig in sorted(src.glob('*.tex')):
        if fig == style_file:
            continue
        pdf = work / f'{fig.stem}.pdf'
        svg = outdir / 'figures' / f'{fig.stem}.svg'
        newest = max(fig.stat().st_mtime, style_file.stat().st_mtime)
        if pdf.exists() and (gray or svg.exists()) and pdf.stat().st_mtime >= newest:
            continue
        doc = work / f'{fig.stem}.doc.tex'
        doc.write_text('\\documentclass{article}\n'
                       '\\usepackage{tikz}\n'
                       '\\usepackage[active,tightpage]{preview}\n'
                       '\\PreviewEnvironment{tikzpicture}\n'
                       '\\setlength\\PreviewBorder{1pt}\n'
                       '\\usepackage{fontspec}\n' + fonts +
                       f'\\input{{{style_file.resolve()}}}\n'
                       + ('\\selectcolormodel{gray}\n' if gray else '') +
                       '\\begin{document}\n'
                       f'\\input{{{fig.resolve()}}}\n'
                       '\\end{document}\n')
        r = subprocess.run(['lualatex', '-interaction=nonstopmode', '-halt-on-error',
                            f'-jobname={fig.stem}', doc.name],
                           cwd=work, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if r.returncode != 0:
            lines = (work / f'{fig.stem}.log').read_text(errors='replace').splitlines()
            errors = [i for i, l in enumerate(lines) if l.startswith('!')]
            first = errors[0] if errors else max(0, len(lines) - 20)
            sys.exit(f'build.py: figure {fig.name} failed:\n' + '\n'.join(lines[first:first + 12]))
        if not gray:
            subprocess.run(['pdftocairo', '-svg', str(pdf), str(svg)], check=True)
    # Ready-made pictures, such as flame graphs, come as figures/NAME.svg:
    # copied for the HTML editions, and converted to PDF for LaTeX with
    # rsvg-convert (librsvg), and for the gray edition to gray with
    # Ghostscript.
    for fig in sorted(src.glob('*.svg')):
        pdf = work / f'{fig.stem}.pdf'
        svg = outdir / 'figures' / fig.name
        if pdf.exists() and (gray or svg.exists()) and pdf.stat().st_mtime >= fig.stat().st_mtime:
            continue
        if gray:
            color = work / f'{fig.stem}.color.pdf'
            subprocess.run(['rsvg-convert', '-f', 'pdf', '-o', str(color), str(fig)], check=True)
            subprocess.run(['gs', '-q', '-o', str(pdf), '-sDEVICE=pdfwrite',
                            '-sColorConversionStrategy=Gray',
                            '-dProcessColorModel=/DeviceGray', str(color)], check=True)
        else:
            shutil.copy(fig, svg)
            subprocess.run(['rsvg-convert', '-f', 'pdf', '-o', str(pdf), str(fig)], check=True)

def build_pdf(inputs, out, paper='b5', gray=False, titlepage=False):
    fonts = []
    if have_tex_file('LibertinusSerif-Regular.otf'):
        fonts += ['-V', 'mainfont=Libertinus Serif']
        # Math, such as chapter 15's formula, in the matching font.
        if have_tex_file('LibertinusMath-Regular.otf'):
            fonts += ['-V', 'mathfont=Libertinus Math']
    else:
        print('note: Libertinus Serif not installed, using Latin Modern', file=sys.stderr)
    if have_tex_file('SourceCodePro-Regular.otf'):
        fonts += ['-V', 'monofont=Source Code Pro', '-V', 'monofontoptions=Scale=MatchLowercase']
    # With a document language set, Pandoc's LaTeX template loads selnolig
    # (Fedora: texlive-selnolig), which suppresses wrong ligatures.
    if not have_tex_file('selnolig.sty'):
        print('note: selnolig.sty not installed, building without a document language',
              file=sys.stderr)
        fonts += ['-M', 'lang=']
    # Number chapters ("Chapter 4"), but not the sections inside them.
    # Pandoc writes the LaTeX, and latex_to_pdf() runs LaTeX, because Pandoc
    # can't run makeindex between LaTeX runs.
    tex = out.parent / 'gen' / f'{out.stem}.tex'
    pandoc(inputs, '-s', '-o', tex, '--toc', '--toc-depth=2', '--top-level-division=chapter',
           '--number-sections', '-V', 'secnumdepth=0',
           '--highlight-style', theme_file(out.parent, 'gray' if gray else 'color'),
           '-H', style / 'book.tex', *(['-H', style / 'gray.tex'] if gray else []),
           *(['-H', art_tex, '-H', style / 'titlepage.tex', '-H', style / 'structure.tex']
             if titlepage else []),
           *(['-H', solutions_title(out.parent / 'gen')] if args.solutions else []),
           '-V', 'documentclass=scrbook', '-V', 'classoption=open=any,numbers=noenddot,index=totoc',
           *layouts[paper], '-V', 'colorlinks=true',
           *[arg for kind in ('link', 'url', 'file', 'cite')
             for arg in ('-V', f"{kind}color={'black' if gray else '[rgb]{0.1,0.35,0.6}'}")],
           *fonts)
    tex.write_text(breakable_code(tex.read_text()))
    latex_to_pdf(tex, out)

def normalize_index_code(idx):
    """An \\index inside another command's argument, such as a list item's
    label or a box, is written to the .idx file with \\indexcode expanded,
    as {\\indexcodefont NAME}, where elsewhere it stays \\indexcode{NAME};
    makeindex would list the two as separate entries. Writes both the same
    way."""
    text = idx.read_text()
    fixed = re.sub(r'\{\\indexcodefont ([^{}]*)\}', r'\\indexcode{\1}', text)
    if fixed != text:
        idx.write_text(fixed)

def drop_pages_also_main(idx):
    """Drops from the index file the plain mentions of an entry on a page
    where the entry also has its main mention, in bold: makeindex would
    list that page twice, once in bold and once not, since their formats
    differ. A plain mention is a page (format hyperpage, with hyperref), or
    a range, of a section about the entry, that begins and ends on that
    page (|( and |), which makeindex shows as a plain page; a bold one's
    format is hyperxindexformat{\\textbf}."""
    entry = re.compile(r'^\\indexentry\{(.*)\|([()]?)'
                       r'(hyperpage|hyperxindexformat\{\\textbf\})\}\{(\d+)\}$')
    lines = idx.read_text().splitlines()
    matches = [entry.match(line) for line in lines]
    main = {(m[1], m[4]) for m in matches if m and m[3] != 'hyperpage'}
    drop = set()
    opened = {}         # each entry's ranges that are open: (line, page)
    for i, m in enumerate(matches):
        if not m or (m[1], m[4]) not in main:
            if m and m[2] == '(':
                opened.setdefault(m[1], []).append((i, m[4]))
            elif m and m[2] == ')' and opened.get(m[1]):
                opened[m[1]].pop()
            continue
        if m[2] == '' and m[3] == 'hyperpage':
            drop.add(i)
        elif m[2] == '(':
            opened.setdefault(m[1], []).append((i, m[4]))
        elif m[2] == ')' and opened.get(m[1]):
            start, page = opened[m[1]].pop()
            if page == m[4]:
                drop.update((start, i))
    idx.write_text(''.join(line + '\n' for i, line in enumerate(lines)
                           if i not in drop))

def breakable_code(latex):
    """Lets a line break inside inline code after each "::" and each "/",
    so that a long name, such as coroutine::parallel_for_each, or a path
    doesn't stick out into the margin at the end of a line. Code blocks are
    left alone, and so is the code in index entries (after "@"), which
    bookfilter.py makes breakable in its own way."""
    out = []
    i = 0
    while (j := latex.find('\\texttt{', i)) >= 0:
        k = j + len('\\texttt{')
        depth = 1
        while depth:
            if latex[k] == '\\':
                k += 2
                continue
            depth += {'{': 1, '}': -1}.get(latex[k], 0)
            k += 1
        out.append(latex[i:j])
        code = latex[j:k]
        if latex[j - 1:j] != '@':
            code = re.sub(r'::(?!\\allowbreak)', r'::\\allowbreak{}', code)
            code = re.sub(r'(?<!\\)/(?!\\allowbreak)', r'/\\allowbreak{}', code)
        out.append(code)
        i = k
    out.append(latex[i:])
    return ''.join(out)

def latex_to_pdf(tex, out):
    """Runs LuaLaTeX on tex until its cross-references settle, with
    makeindex after the first run if the document has an index, and
    copies the PDF to out."""
    work = tex.parent / f'latex-{tex.stem}'
    work.mkdir(parents=True, exist_ok=True)
    cmd = ['lualatex', '-interaction=nonstopmode', '-halt-on-error',
           f'-output-directory={work}', str(tex)]
    log = work / f'{tex.stem}.log'
    def latex():
        r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if r.returncode != 0:
            lines = log.read_text(errors='replace').splitlines()
            errors = [i for i, l in enumerate(lines) if l.startswith('!')]
            first = errors[0] if errors else max(0, len(lines) - 20)
            sys.exit('build.py: LaTeX failed:\n' + '\n'.join(lines[first:first + 20]))
    # The first run reads the index that makeindex wrote last time. Start
    # without it, so that an index broken by an earlier build can't break
    # this one.
    (work / f'{tex.stem}.ind').unlink(missing_ok=True)
    latex()
    idx = work / f'{tex.stem}.idx'
    if idx.exists() and idx.stat().st_size:
        normalize_index_code(idx)
        drop_pages_also_main(idx)
        # Run makeindex in the work directory: TeX Live's makeindex refuses to
        # write files given by an absolute path (openout_any = p).
        subprocess.run(['makeindex', '-q', '-s', str(style / 'book.ist'), idx.name],
                       check=True, cwd=work)
    for _ in range(3):
        latex()
        if not re.search(r'Rerun to get|Label\(s\) may have changed|Rerun LaTeX',
                         log.read_text(errors='replace')):
            break
    shutil.copy(work / f'{tex.stem}.pdf', out)

def build_html(inputs, out):
    shutil.copy(style / 'book.css', out.parent / 'book.css')
    pandoc(inputs, '-o', out, '-s', '--toc', '--toc-depth=2', '--css', 'book.css',
           '--number-sections', '--mathml', '--highlight-style', theme_file(out.parent, 'color'))
    if building_book:
        # Put the sea star below the title. (Pandoc's --include-before-body
        # would put it above the title block.)
        page = out.read_text()
        out.write_text(page.replace('</header>', '</header>\n' + art_html.read_text(), 1))

def build_html_chapters(inputs, out):
    """The HTML edition as a directory of pages, one for each part and
    chapter, with links between them (style/chapters.html); out/index.html
    is the title page, with the table of contents."""
    shutil.rmtree(out, ignore_errors=True)
    pandoc(inputs, '-o', out, '-t', 'chunkedhtml', '--split-level=1', '--chunk-template=%i.html',
           '--template', style / 'chapters.html', '--toc', '--toc-depth=2',
           '--css', 'book.css', '--number-sections', '--mathml',
           '--highlight-style', theme_file(out.parent, 'color'))
    shutil.copy(style / 'book.css', out / 'book.css')
    if (out.parent / 'figures').is_dir():
        shutil.copytree(out.parent / 'figures', out / 'figures', dirs_exist_ok=True)
    # Name each page as the book does ("Chapter 4: Coroutines"), in its
    # <title> and in the links to it from the neighboring pages.
    sitemap = json.loads((out / 'sitemap.json').read_text())
    book_title = sitemap['section']['title']
    labels = json.loads(Path(os.environ['BOOK_LABELS']).read_text()) if building_book else {}
    names = {}
    for entry in sitemap['subsections']:
        page = entry['section']['path'].split('#')[0]
        label = entry['section']['id'].replace('-', ':', 1)
        info = labels.get(label, {})
        if info.get('kind') == 'chapter':
            names[page] = f'Chapter {info["number"]}: {entry["section"]["title"]}'
        elif info.get('kind') == 'part':
            names[page] = f'Part {info["number"]}: {info["title"]}'
        else:
            names[page] = entry['section']['title']
    nav_link = re.compile(r'(<span class="pagenav-(?:prev|next)"><a href="([^"#]*)[^"]*"[^>]*>)(.*?)(</a>)')
    for page in out.glob('*.html'):
        html = page.read_text()
        html = nav_link.sub(lambda m: m[1] + names.get(m[2], m[3]) + m[4], html)
        if page.name == 'index.html':
            title = book_title or 'Contents'
        else:
            title = f'{names[page.name]} – {book_title}' if book_title else names[page.name]
        html = re.sub(r'<title>.*?</title>', lambda m: f'<title>{title}</title>', html, count=1,
                      flags=re.S)
        if page.name == 'index.html' and building_book:
            html = html.replace('</header>', '</header>\n' + art_html.read_text(), 1)
        page.write_text(html)
    if building_book:
        shutil.copy(args.outdir / 'seastar.svg', out / 'seastar.svg')

def build_md(inputs, out):
    """The whole book as a single Markdown file, in GitHub's flavor of
    Markdown, with the code of the examples in it. Pandoc names the sections
    as GitHub does (gfm_auto_identifiers), so that the table of contents
    links to them; chapters get anchors of their own (tools/bookfilter.py)."""
    pandoc(inputs, '-o', out, '-t', 'gfm', '-s', '--template', style / 'book.gfm',
           '--toc', '--toc-depth=2', '--wrap=none',
           reader='markdown+pandoc_title_block+gfm_auto_identifiers')

def build_txt(inputs, out):
    pandoc(inputs, '-o', out, '-t', 'plain', '--columns=78', '--number-sections')

def epub_cover(outdir):
    """The cover of the EPUB: the PDF's title page as an image, building the
    PDF first if needed (pdftoppm, Fedora: poppler-utils)."""
    pdf = outdir / 'book.pdf'
    if not pdf.exists():
        build_pdf(args.inputs, pdf, titlepage=True)
    subprocess.run(['pdftoppm', '-f', '1', '-l', '1', '-r', '200', '-png', '-singlefile',
                    pdf, outdir / 'gen' / 'cover'], check=True)
    return outdir / 'gen' / 'cover.png'

def epub_date(outdir):
    """EPUB requires an ISO date (such as 2026-11), which Pandoc can't make
    from the book's date ("November 2026"), so give it one in Dublin Core
    metadata; the title page still shows the book's date as written."""
    meta = yaml.safe_load((book / 'metadata.yaml').read_text().split('---')[1])
    try:
        date = datetime.datetime.strptime(str(meta.get('date')), '%B %Y').strftime('%Y-%m')
    except ValueError:
        date = datetime.date.today().isoformat()
    path = outdir / 'gen' / 'epub-metadata.xml'
    path.write_text(f'<dc:date>{date}</dc:date>\n')
    return path

def epub_metadata(outdir):
    """A copy of metadata.yaml for the EPUB, in which "author: Edited by X"
    becomes a creator X whose role is editor, so that e-book libraries list
    the book under X, not under "Edited by". (epub.css puts "Edited by" back
    on the EPUB's title page.)"""
    text = (book / 'metadata.yaml').read_text()
    text = re.sub(r'(?m)^author: Edited by (.+)$',
                  lambda m: f'creator:\n  - role: edt\n    text: "{m.group(1)}"', text)
    path = outdir / 'gen' / 'metadata-epub.yaml'
    path.write_text(text)
    return path

def build_epub(inputs, out):
    book_only = []
    if building_book:
        book_only = ['--epub-cover-image', epub_cover(out.parent),
                     '--epub-metadata', epub_date(out.parent)]
        meta = epub_metadata(out.parent)
        inputs = [meta if Path(i) == book / 'metadata.yaml' else i for i in inputs]
    pandoc(inputs, '-o', out, '-t', 'epub3', '--toc', '--toc-depth=2', '--number-sections',
           '--mathml', '--css', style / 'epub.css', '--split-level=1',
           '--highlight-style', theme_file(out.parent, 'color'), *book_only)

def build_azw3(epub, out):
    """The Kindle format, converted from the EPUB by Calibre's ebook-convert
    (Fedora: calibre). Kindles also accept the EPUB itself, through Amazon's
    Send to Kindle, which converts it."""
    if not shutil.which('ebook-convert'):
        sys.exit('build.py: azw3 needs ebook-convert, from Calibre (Fedora: dnf install calibre)')
    if not epub.exists():
        build_epub(args.inputs, epub)
    subprocess.run(['ebook-convert', epub, out], check=True, stdout=subprocess.DEVNULL)

parser = argparse.ArgumentParser()
parser.add_argument('--formats', default='pdf',
                    help='comma-separated, from: pdf,html,html-chapters,epub,azw3,md,txt,all '
                         '(default pdf; all means all but azw3)')
parser.add_argument('--paper', choices=layouts, default='b5', help='PDF page size (default b5)')
parser.add_argument('--gray', action='store_true', help='grayscale PDF, for print')
parser.add_argument('--solutions', action='store_true',
                    help='the book of solutions to the exercises (solutions/), instead of the book')
parser.add_argument('outdir', type=Path)
parser.add_argument('inputs', nargs='*', type=Path)
args = parser.parse_args()
args.outdir.mkdir(parents=True, exist_ok=True)
building_book = not args.inputs
book_args = []
if args.inputs:
    stem = args.inputs[0].stem
else:
    if args.solutions:
        args.inputs, labels_file = solutions_inputs(args.outdir / 'gen')
        os.environ['BOOK_SOLUTIONS'] = '1'
    else:
        args.inputs, labels_file = book_inputs(args.outdir / 'gen')
    os.environ['BOOK_LABELS'] = str(labels_file)
    book_args = ['--filter', str(book / 'tools' / 'bookfilter.py'),
                 '--resource-path', f'{args.outdir}:{book}']
    art_tex, art_html = title_art(args.outdir)
    figures(args.outdir)
    if args.gray:
        figures(args.outdir, gray=True)
        os.environ['BOOK_GRAY'] = '1'
    stem = 'solutions' if args.solutions else 'book'
formats = args.formats.split(',')
if 'all' in formats:
    formats = ['pdf', 'html', 'html-chapters', 'epub', 'md', 'txt'] + \
              [f for f in formats if f not in ('all', 'pdf', 'html', 'html-chapters', 'epub', 'md', 'txt')]
for fmt in formats:
    if fmt == 'pdf':
        variant = ('' if args.paper == 'b5' else f'-{args.paper}') + ('-gray' if args.gray else '')
        out = args.outdir / f'{stem}{variant}.pdf'
        build_pdf(args.inputs, out, args.paper, args.gray, titlepage=building_book)
    elif fmt == 'html':
        out = args.outdir / f'{stem}.html'
        build_html(args.inputs, out)
    elif fmt == 'html-chapters':
        out = args.outdir / f'{stem}-html'
        build_html_chapters(args.inputs, out)
    elif fmt == 'md':
        out = args.outdir / f'{stem}.md'
        build_md(args.inputs, out)
    elif fmt == 'txt':
        out = args.outdir / f'{stem}.txt'
        build_txt(args.inputs, out)
    elif fmt == 'epub':
        out = args.outdir / f'{stem}.epub'
        build_epub(args.inputs, out)
    elif fmt == 'azw3':
        out = args.outdir / f'{stem}.azw3'
        build_azw3(args.outdir / f'{stem}.epub', out)
    else:
        parser.error(f'unknown format {fmt}')
    print(out)
