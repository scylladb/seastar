# Asynchronous Programming with Seastar

This directory holds the source of the book *Asynchronous Programming with
Seastar*, the complete example programs that it shows, and the tools that
build it.

## Building the book

### Prerequisites

The build needs Pandoc, its Python filter library, PyYAML, and LuaLaTeX
with a few LaTeX packages and fonts. On Fedora:

    sudo dnf install pandoc-cli pandoc-pdf python3-pandocfilters python3-pyyaml \
        texlive-koma-script texlive-microtype \
        texlive-libertinus-fonts texlive-libertinus-otf texlive-sourcecodepro \
        texlive-framed texlive-tcolorbox texlive-upquote texlive-fvextra \
        texlive-selnolig librsvg2-tools poppler-utils ghostscript

`pandoc-pdf` pulls in LuaLaTeX and the basic LaTeX collection. The other
packages are:

| Package | Used for |
|---|---|
| `pandoc-cli` | Pandoc itself; the web edition needs Pandoc 3.0 or later (Fedora 44 has 3.7, Ubuntu 24.04 has 3.1.3) |
| `pandoc-pdf` | PDF output (requires LaTeX) |
| `python3-pandocfilters` | the book's Pandoc filters, written in Python |
| `python3-pyyaml` | reading the book's structure from `metadata.yaml` |
| `texlive-koma-script` | the `scrbook` document class and page headers |
| `texlive-microtype` | better justification and spacing |
| `texlive-libertinus-fonts`, `texlive-libertinus-otf` | the text font, Libertinus Serif |
| `texlive-sourcecodepro` | the code font, Source Code Pro |
| `texlive-framed` | shaded, highlighted code blocks |
| `texlive-tcolorbox` | note and warning boxes |
| `texlive-upquote` | straight quotes in code |
| `texlive-fvextra` | wrapping long code lines |
| `librsvg2-tools` | `rsvg-convert`, which turns the title page's drawing, and figures that come as SVG, into PDF |
| `texlive-selnolig` | suppressing ligatures that are wrong in English, such as the "fl" in "halflife" |
| `poppler-utils` | `pdftoppm`, which turns the PDF's title page into the EPUB's cover |
| `ghostscript` | `gs`, which turns figures that come as SVG into gray, for the grayscale edition |

Without the two fonts, the build falls back to LaTeX's default font,
Latin Modern. The other packages are required.

On Ubuntu 24.04, the same LaTeX packages come in a few larger
collections:

    sudo apt install pandoc python3-pandocfilters python3-yaml \
        texlive-luatex texlive-latex-recommended texlive-latex-extra \
        texlive-fonts-extra librsvg2-bin poppler-utils ghostscript

Here `texlive-latex-recommended` has KOMA-Script and microtype,
`texlive-latex-extra` has the code-block and box packages,
`texlive-luatex` has LuaLaTeX's support and selnolig, `librsvg2-bin` has
`rsvg-convert`, and `texlive-fonts-extra` has the two fonts.
`texlive-fonts-extra` is large (a 614 MB download, 1.7 GB installed),
and, as on Fedora, the fonts are optional. These packages were tested on
a fresh Ubuntu 24.04 (in a container), where they built every format.

For the Kindle format (`--formats azw3`, below), also install Calibre,
whose `ebook-convert` makes it from the EPUB: `sudo dnf install calibre`
on Fedora, or `sudo apt install calibre` on Ubuntu.

### Building

To build the book, give `tools/build.py` an output directory:

    tools/build.py build

This writes the PDF, `build/book.pdf`, the book's main edition. Its pages
are B5 (176×250 mm), close to the size of most printed technical books,
with code highlighted in color.

The book can be built in other formats too, on request: list them with
`--formats`, or ask for all of them except `azw3` with `--formats all`.
For example, `tools/build.py --formats html-chapters,epub build` builds
the web edition and the e-book. Each format writes into the output
directory:

| Format | Output | What it is |
|---|---|---|
| `pdf` | `book.pdf` | The PDF, as above. Built when `--formats` isn't given. |
| `html-chapters` | `book-html/` | The web edition: a directory with a page for each part and chapter. Open `book-html/index.html`, which has the title and the table of contents. Every page has links to the previous and next pages and to the contents, at its top and at its bottom, and the left and right arrow keys follow them too. References to other chapters link to their pages. The directory is self-contained, so it can be read from the disk, or copied as it is to any web server. |
| `html` | `book.html` | The whole book as a single web page, handy for searching the whole book with the browser's Find, or for saving it as one file. |
| `epub` | `book.epub` | An e-book, for e-readers and reading apps. Its cover is the PDF's title page, so building it also builds the PDF if it's missing. Kindles read EPUB too, through Amazon's "Send to Kindle", which converts it. |
| `azw3` | `book.azw3` | Amazon's Kindle format, for copying to a Kindle directly. Calibre converts it from the EPUB (see Prerequisites); `--formats all` leaves it out, because Calibre is a large package. |
| `md` | `book.md` | The whole book as a single file in GitHub's flavor of Markdown, which GitHub, editors and many other tools display. It contains everything: the code of the examples, chapter numbers in the headings, and links for the references between chapters and from the table of contents, which work when GitHub displays the file. It has no title-page drawing. |
| `txt` | `book.txt` | Plain text. |

The HTML editions follow the reader's choice of a light or dark theme.
The EPUB leaves fonts and colors to the e-reader's settings, except in
code.

### Options

| Option | Effect |
|---|---|
| `--formats LIST` | The formats to build, from the table above, separated by commas. `all` means all of them but `azw3`. The default is `pdf`. |
| `--paper a4` | PDF on A4 pages, for printing at home. It uses an 11 pt font and wide margins, so lines stay at a readable length. Output: `book-a4.pdf`. |
| `--paper letter` | The same on US Letter pages. Output: `book-letter.pdf`. |
| `--gray` | Grayscale PDF for black-and-white printing, or for a publisher. Keywords are bold, comments italic gray, links black, and the figures are typeset again in gray. Output: `book-gray.pdf`. |
| `--solutions` | The book of solutions to the exercises, from `solutions/`, instead of the book (see "Solutions" below). Output: `solutions.pdf`. |

`--paper` and `--gray` affect only the PDF, and they can be combined.
For example, to print the book at home on a black-and-white printer:

    tools/build.py --paper a4 --gray build

This writes `build/book-a4-gray.pdf`.

`tools/build.py` also builds a single Markdown document, such as a draft:
give it the files after the output directory, `tools/build.py build
draft.md`, and it writes `build/draft.pdf` (or, with `--formats`, other
formats). Such a document gets no title page, parts or chapter
references.

## Running the examples

Every program that the book shows is here, complete, so you can build it,
run it, and do the exercises:

- `examples/`: the example programs, one directory per chapter
  (`examples/tutorial-introduction/`).
- `shoal/`: the book's running example, Shoal, a Redis-compatible
  key-value server, in a directory for each of its versions (`shoal/v0/`,
  `shoal/v1/`, `shoal/v2/`, ...). Each is a complete CMake project, which
  the book's "Getting started" chapter explains how to build.

Shoal speaks the Redis protocol, so it is used with the standard Redis
tools. Fedora and Ubuntu package them as Valkey, the compatible
open-source fork of Redis. On Fedora:

    sudo dnf install valkey python3-redis

and on Ubuntu 24.04:

    sudo apt install valkey-server valkey-tools python3-redis

The packages, with Fedora's names:

| Package | Used for |
|---|---|
| `valkey` | `valkey-cli` to talk to Shoal, `valkey-benchmark` to load it, and `valkey-server` to compare against it |
| `python3-redis` | the Python Redis client (redis-py), used by Shoal's test suite |

On Fedora, the optional `valkey-compat-redis` package adds the traditional names
(`redis-cli`, `redis-benchmark`). The `memtier_benchmark` load generator,
used in a few benchmarks, is not packaged by Fedora. To run those
benchmarks, build it from source (https://github.com/RedisLabs/memtier_benchmark).

Shoal's tests, written with pytest and redis-py, are in `shoal/test/`,
one file for each version, `test_vN.py`, with the tests of what that
version added, and a script that runs them: `shoal/test/run 4` tests the
Shoal built in `shoal/v4/build` with the tests of versions 1 to 4, and
`shoal/test/run 4 --valkey` runs the same tests against a
`valkey-server`, to check that the tests expect what Redis really does.
The script chooses the server's options by version, since each version
of Shoal has only the options it has.

## Working on the book

This part is for the book's authors.

### Files

- `metadata.yaml`: the title page's text, and the book's structure: which
  chapters it has, in which order, grouped in parts, followed by the
  appendices and the back matter.
- `chapters/`: one Markdown file per chapter, named after its subject
  (`coroutines.md`), never its number.
- `examples/` and `shoal/`: the code that chapters show (see above).
- `figures/`: the figures, one TikZ picture each, and their common style
  (see "Figures" below). A figure drawn from measurements may have its
  generator next to it, `figures/NAME.py`, which writes `figures/NAME.tex`
  from the data (and, for `backlog-burst`, the program that measured
  it); when the measurement is redone, run the generator again rather
  than edit the picture.
- `tools/build.py`: builds the book.
- `tools/bookfilter.py`: the Pandoc filter that numbers chapters, renders the
  parts, resolves references, copies the code from the examples into
  the book, and makes the PDF's index.
- `tools/seastar_options.txt`: Seastar's command-line options, which the
  index lists under "command-line options" wherever a code span begins
  with one; written from a Seastar program's `--help-seastar`, so update
  it when Seastar adds options.
- `tools/seastar_names.py`: writes `tools/seastar_names.txt`, the list of
  Seastar's public names that the index recognizes, from Seastar's
  headers, with Doxygen. Run it after moving the book to a newer Seastar.
- `tools/index_entries.py`: prints the PDF's index as plain text, from the
  last build, to review its entries.
- `tools/check_examples.py`: compiles every example and every version of
  Shoal with each compiler and C++ standard the book promises, runs it, and
  checks what it does.
- `tools/check_overlap.py`: reports text copied word for word from other
  documents.
- `tools/cross_references.py`: lists every reference from one chapter to
  another, with its sentence, grouped by the chapter referred to. A
  reference often promises something ("chapter 21 shows how to ..."), so
  before calling a chapter done, check that it keeps the promises that
  other chapters make about it.
- `tools/header_mentions.py`: lists the Seastar classes, functions and
  macros whose header the book doesn't name where it introduces them (see
  "Code" below).
- `style/`: the LaTeX preamble (`book.tex`), the title page and part pages
  (`titlepage.tex`, `structure.tex`), the HTML stylesheet (`book.css`),
  the template of the web edition's pages (`chapters.html`), the EPUB's
  stylesheet (`epub.css`), and the template of the Markdown edition
  (`book.gfm`). The colors of highlighted code are defined in
  `tools/build.py`: a deliberately restrained theme of bold navy keywords,
  italic green comments and dark red strings, with everything else black,
  for the PDF and both HTML editions and the EPUB, and a grayscale one
  for `--gray`. The Markdown edition's code is highlighted by whatever
  displays it.
- `art/seastar.svg`: the sea star on the title page, in a stippled
  pen-and-ink style. It was drawn by `art/seastar.py`, which the build does
  not run; run it only to make a new version deliberately.
- `solutions/`: the book of solutions to the exercises, one file per
  chapter, named like the chapter's file (see "Solutions" below), and its
  `metadata.yaml`.

### Writing

Chapters are numbered by their position in `metadata.yaml`, so adding,
removing or moving a chapter means editing that list, and nothing else.

Each chapter file starts with a heading labeled after the file name:
`# Coroutines {#ch:coroutines}` in `chapters/coroutines.md`, or
`{#app:building-seastar}` for an appendix. Parts are labeled `part:` followed by
their `id` in `metadata.yaml`.

Never write a chapter number by hand. Refer to a chapter by its label, and
the build supplies the number:

| You write | The book shows |
|---|---|
| `[chapter](#ch:coroutines)` | chapter 4 |
| `[Chapter](#ch:coroutines)` | Chapter 4 |
| `[appendix](#app:building-seastar)` | appendix A |
| `[Part](#part:io)` | Part IV |
| `[the next chapter](#ch:time)` | the next chapter (a link, without a number) |

A reference to a label that doesn't exist stops the build.

All headings, including the titles of parts, chapters and appendices, are
in sentence case: only the first word and names are capitalized, as in
"Concurrency within a shard" or "The case for Seastar". Only the book's
own title is in title case.

A *C++ corner* explains a part of C++ that the reader may not know, and
that a reader who knows C++ well can skip without missing anything about
Seastar. Write it as a quotation whose first paragraph starts with
`**C++ corner: title.**`, followed by an index entry
`[]{index="C++ corner!title"}`: the index is where the reader finds all
the corners.
The build sets a corner apart in a pale blue box with a blue rule on its
left (`cppcorner` in `style/book.tex`, `.corner` in the CSS), so that it
doesn't look like a quotation.

Any other quotation whose first paragraph starts with a bold title, such
as `**About the name.**`, is a *sidebar*: the same box in gray, with a gray rule
instead of the blue one (`sidebar` in `style/book.tex`, `.sidebar` in
the CSS). Draft notes, which start with `**Draft note, for the
editors:**`, and real quotations keep the inset quotation style. Code in
a corner or a sidebar has no shading of its own.

### Solutions

The answers to the exercises are in a separate book, not in the book
itself, so that a reader who wants to try an exercise doesn't see its
answer. `tools/build.py --solutions build` typesets it, with the same
filter and styles, into `build/solutions.pdf`.

`solutions/NAME.md` answers the exercises of `chapters/NAME.md`. It
starts with the chapter's title, labeled `sol:NAME`, as in `# Shared
nothing {#sol:shared-nothing}`, and has a section for each exercise it
answers, headed exactly `## Exercise N`. The build gives the solution
chapter its book chapter's number, and inserts each exercise's text,
taken from the chapter's "Exercises" section, after its heading, so the
question is always the book's current one. References to the book's
chapters and figures work as in the book.

A solution should do the exercise, as a reader would, and say what
happened, why, and what it teaches, with code where the exercise asks
for code. A solution's programs go in `examples/solutions/`, named after
the chapter and the exercise (`shared-nothing-ex1.cc`), with a check in
`tools/check_examples.py`, like the book's own examples. Show a program
whole, so that a reader can see and run all of it; keep its lines
within 77 characters, which fit the PDF's code without wrapping. A
solution that changes Shoal is a patch, `examples/solutions/shoal-NAME.patch`:
a unified diff against the Shoal version it changes, whose header names
that version (`# base: shoal/v8`), and may give options for the tests'
servers (`# server options: --timeout 2`). It may add tests of its own,
in a new file, `test_solutions.py`. `check_examples.py` applies it to a
copy of that version (with `tools/shoal_patch.py`), builds it, and runs
that version's tests and the patch's own, against Valkey too. Make a
patch with `diff -u`, from a copy of the version that you changed (and
make its empty context lines empty, rather than a single space, which
`patch` accepts, to keep trailing whitespace out of the repository), so
that a fix to the version also reaches the solutions that change it (or,
if it touches the same lines, makes the patch fail to apply, loudly).
The book of solutions shows code from the changed Shoal with
`include="examples/solutions/shoal-NAME.patch" file="commands.cc"`, and a
snippet if needed; show every function that it adds or changes, whole,
and say which version the rest comes from. The book of solutions has no index:
the book's covers its names.

### Code

Code that the book shows should come from a program that really compiles
and runs. Put the program in `examples/CHAPTER/`, and include it in the
chapter with an empty code block:

    ```{.cpp include="examples/tutorial-introduction/hello.cc"}
    ```

This shows the whole file. To show only part of it, mark the part in the
source with `// snippet: NAME` and `// end snippet: NAME` lines (or `#`
instead of `//`, in files such as `CMakeLists.txt`), and add
`snippet="NAME"` to the code block. The marker lines never appear in the
book, and an unknown file or snippet stops the build. Then add a check of
what the program does to `tools/check_examples.py`, and run it. Shoal's
versions are included the same way.

A snippet doesn't show the file's `#include` lines, so where the text
mainly explains a Seastar class, function or macro (where its index entry
has `.main`), it names the header that declares it, as in
"`handle_signal()`, from `<seastar/core/signal.hh>`". Several names from
one header need it named only once, at the first of them.
`tools/header_mentions.py` lists the places that are missing it. In the
PDF, a header's name may break after each `/`, so that a long one
doesn't stick out into the margin.

The book promises that all its code works with GCC and Clang, in C++23
and C++26. So check it against four builds of Seastar, one for each
compiler and standard, because `pkg-config`'s flags and the libraries are
specific to the compiler that built Seastar, and Seastar's CMake files
warn when a program's standard isn't the one Seastar was built with:

    tools/check_examples.py \
      --seastar g++:23=$GCC23/install:$GCC23/build/release/_cooking/installed \
      --seastar g++:26=$GCC26/install:$GCC26/build/release/_cooking/installed \
      --seastar clang++:23=$CLANG23/install:$CLANG23/build/release/_cooking/installed \
      --seastar clang++:26=$CLANG26/install:$CLANG26/build/release/_cooking/installed

where each of the four Seastar source trees was configured with the
matching `--compiler` and `--c++-standard`, with `--prefix` pointing to its
`install` directory (and `--cook c-ares` on Fedora 44), then built and
installed. The second directory on each line holds the dependencies that
`--cook` built, which installing Seastar doesn't install.

### Figures

A figure is a TikZ picture in `figures/NAME.tex`, drawn in the style that
`figures/style.tex` defines for all of them: its fonts, its colors, and
styles such as `box`, `coolbox`, `flow` and `message`. It's included like
an image, with a caption and a label:

    ![Threads that share data (left) take turns at a lock, and ...
    messages.](figures/sharing.svg){#fig:sharing}

Refer to it by its label, and the build supplies the number:
`[Figure](#fig:sharing)` shows "Figure 1.1". Figures are numbered within
each chapter or appendix, in order, as LaTeX numbers them.
`tools/build.py` typesets each figure in a document of its own, with the
book's fonts, into a PDF that the PDF edition includes, and converts that
into the SVG file named in the Markdown, for the HTML and EPUB editions,
with `pdftocairo` (Fedora: poppler-utils). A picture made by another
tool, such as a flame graph, comes as `figures/NAME.svg` instead: the
HTML and EPUB editions use it as it is, and the build converts it into
PDF with `rsvg-convert`. In the PDF, captions are set a
size smaller than the text, inset on both sides. A figure may fill up to
85% of a page with text (`style/book.tex` changes LaTeX's float
fractions), since by default anything over half a page goes to a page of
its own. Put a figure right after the paragraph that first refers to it:
LaTeX then places it right there, if it fits, or at the top or bottom of
that page or the next. But LaTeX never places a figure before the point
where it appears in the source, nor on the pages of a long table, so
when a long listing or table follows the reference, the figure may land
two pages later. Then put the figure before them, even before the
paragraph that refers to it, and keep it out of the middle of the text,
where it would come before its reference, with `placement="tb"`:
`{#fig:fiasco placement="tb"}` (the filter turns it into LaTeX's
placement for that figure). Check where each figure landed after such
changes.

A wide picture whose labels would be too small across the page, such as
a flame graph, can be turned on its side, on a page of its own, as wide
as the page is tall, with `sideways="true"`:
`{#fig:flame-shoal sideways="true"}` (LaTeX's `sidewaysfigure`, from
the `rotating` package; the other editions ignore it).

Draw figures so that they work in print:

- Every figure must read well in grayscale, not only in color: let
  shapes, positions, line styles and labels carry the meaning, and use
  colors that differ in lightness, not only in hue (`style.tex`'s colors
  do). Before calling a figure done, look at its page rendered in gray,
  for example with `pdftoppm -gray`.
- Draw at the final size, at most the width of the text (about 130 mm on
  a B5 page), so that the figure isn't scaled, and its labels stay a size
  or two smaller than the text: in sans serif, black, `\small` for labels
  and `\footnotesize` for notes, which `style.tex` sets to 8.8 and 7.7
  points, and 9.5 point bold for titles. Lines are 0.66 to 0.77 points
  wide, with solid triangular arrowheads; thinner lines look faint in
  print.
- Place things by their relations, not by computed coordinates: `right=of`,
  `below=of`, anchors such as `box.north`, midpoints such as
  `($(a)!0.5!(b)$)`, and `|-` to take one point's x and another's y. Then
  moving one thing moves what depends on it.

### The index

The PDF has an index; the other editions don't, since their readers can
search. Seastar's names are indexed automatically: a code span that names
one, such as `sharded<T>`, `sleep()` or `future::get()`, is indexed under
its full name (`seastar::sharded`), sorted by its name without
`seastar::`, at its first mention in each section. A function counts
only when written as a call (`sleep()`), and a namespace only with
`seastar::`; add `{.noindex}` to a code span that only looks like a
Seastar name. Standard library names are indexed the same way, but only
those listed in `tools/std_names.txt`: the ones the book discusses, not
every `std::vector` it uses. Add a name there when a chapter starts
discussing it. Seastar's headers, such as `<seastar/core/gate.hh>`, are
indexed the same way, under "headers, Seastar's", so that a reader who
meets a header in code can find where the book explains it. So are Seastar's command-line options, such as
`--cpuset` or `--cpuset 0-3`, under "command-line options". Index everything else by hand, where the book explains it
rather than wherever it is mentioned. A standard library name in an
index entry, such as ``[]{index="`std::future`"}``, is sorted by its name
without `std::`, so that it stands next to its Seastar namesake:

| You write | The index shows |
|---|---|
| `[preemption]{.index}` | preemption |
| `[stalls]{index="stall"}` | stall |
| `[*task quota*]{.index .main}` | task quota, with this page in bold: use it where a term is defined |
| `[frames]{index="coroutine!frame"}` | frame, under coroutine |
| `` [ready]{index="`seastar::future`!ready"} `` | ready, under `seastar::future`, the same entry as the automatic one |
| `` []{index="`co_await`"} `` | `co_await`, in code font, with no text in the book |
| `[]{index="LTO\|see{link-time optimization}"}` | LTO, *see* link-time optimization |
| `## Polling {index="polling"}` | polling, with the pages of the whole section |

Write entries in lowercase, except names, and in the singular. Before
calling a chapter done, add its entries, rebuild, and read
`tools/index_entries.py`'s list for near-duplicates. After writing or changing a chapter,
`tools/unindexed_terms.py` lists its terms in italics that have no index
marker; most are just emphasis, but a term the text defines should be
indexed where it is defined.

### Checking the EPUB

To check the EPUB against the standard, use W3C's `epubcheck` (not
packaged by Fedora, but a Java program): download its release from
<https://github.com/w3c/epubcheck/releases>, unzip it, and run
`java -jar epubcheck-VERSION/epubcheck.jar build/book.epub`. The book's
EPUB passed epubcheck 5.4.0 with no errors or warnings.
