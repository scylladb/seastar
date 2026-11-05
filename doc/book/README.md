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
        texlive-selnolig librsvg2-tools poppler-utils

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
| `librsvg2-tools` | `rsvg-convert`, which turns the title page's drawing into PDF |
| `texlive-selnolig` | suppressing ligatures that are wrong in English, such as the "fl" in "halflife" |
| `poppler-utils` | `pdftoppm`, which turns the PDF's title page into the EPUB's cover |

Without the two fonts, the build falls back to LaTeX's default font,
Latin Modern. The other packages are required.

On Ubuntu 24.04, the same LaTeX packages come in a few larger
collections:

    sudo apt install pandoc python3-pandocfilters python3-yaml \
        texlive-luatex texlive-latex-recommended texlive-latex-extra \
        texlive-fonts-extra librsvg2-bin poppler-utils

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
| `--gray` | Grayscale PDF for black-and-white printing, or for a publisher. Keywords are bold, comments italic gray, and links black. Output: `book-gray.pdf`. |

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

From version 1, each version of Shoal has a test suite in its `test/`
directory, written with pytest and redis-py, and a script that runs it:
`shoal/v1/test/run` tests the Shoal built in `shoal/v1/build`, and
`shoal/v1/test/run --valkey` runs the same tests against a
`valkey-server`, to check that the tests expect what Redis really does.

## Working on the book

This part is for the book's authors.

### Files

- `metadata.yaml`: the title page's text, and the book's structure: which
  chapters it has, in which order, grouped in parts, followed by the
  appendices and the back matter.
- `chapters/`: one Markdown file per chapter, named after its subject
  (`coroutines.md`), never its number.
- `examples/` and `shoal/`: the code that chapters show (see above).
- `tools/build.py`: builds the book.
- `tools/bookfilter.py`: the Pandoc filter that numbers chapters, renders the
  parts, resolves references, and copies the code from the examples into
  the book.
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
- `research/`: the API coverage audit used to plan the book, and notes
  backing facts used in the text, such as how each exercise and
  measurement was done.
- `PLAN.md`: the book's plan (table of contents, decisions, open
  questions).

### Writing

Chapters are numbered by their position in `metadata.yaml`, so adding,
removing or moving a chapter means editing that list, and nothing else.

Each chapter file starts with a heading labeled after the file name:
`# Coroutines {#ch:coroutines}` in `chapters/coroutines.md`, or
`{#app:glossary}` for an appendix. Parts are labeled `part:` followed by
their `id` in `metadata.yaml`.

Never write a chapter number by hand. Refer to a chapter by its label, and
the build supplies the number:

| You write | The book shows |
|---|---|
| `[chapter](#ch:coroutines)` | chapter 4 |
| `[Chapter](#ch:coroutines)` | Chapter 4 |
| `[appendix](#app:glossary)` | appendix E |
| `[Part](#part:io)` | Part IV |
| `[the next chapter](#ch:time)` | the next chapter (a link, without a number) |

A reference to a label that doesn't exist stops the build.

All headings, including the titles of parts, chapters and appendices, are
in sentence case: only the first word and names are capitalized, as in
"Concurrency within a shard" or "The case for Seastar". Only the book's
own title is in title case.

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

### Checking the EPUB

To check the EPUB against the standard, use W3C's `epubcheck` (not
packaged by Fedora, but a Java program): download its release from
<https://github.com/w3c/epubcheck/releases>, unzip it, and run
`java -jar epubcheck-VERSION/epubcheck.jar build/book.epub`. The book's
EPUB passed epubcheck 5.4.0 with no errors or warnings.
