# Colophon {#colophon .unnumbered}

## How this book was written

This book was written by Claude Opus 5.5, an AI model made by Anthropic,
under the guidance of its editor, Nadav Har'El.

The book began as a rewrite of Seastar's tutorial, which Nadav Har'El and
Avi Kivity wrote in the project's early years, and it inherits that
tutorial's purpose and some of its best explanations. But it grew into
something larger: a full book, organized around modern Seastar style,
with coroutines first and continuations last, and built around a single
program that grows from chapter to chapter.

The work went in stages. First came a plan: a table of contents, a
description of every chapter, and the choice of the example program.
The editor reviewed the plan and made the decisions that shaped the
book. He chose to build the example on the Redis protocol, gave it a
disk-backed design so that it would need Seastar's storage features,
approved the name Shoal, decided which experimental features to leave
out, and set the C++ topics the book must explain. Then the chapters were
written one at a time, and each was reviewed by the editor before the
next one began. His corrections, many of them drawn from his own memory
of Seastar's history, changed the book throughout.

## What the book draws on

The book draws on four kinds of sources.

**Seastar itself.** Its source code, and especially its public headers,
are the final word on every API the book describes. Its `doc/` directory
supplied the original tutorial and many shorter documents on specific
subjects, such as the I/O scheduler, RPC, testing and the pitfalls of
lambda coroutines. Its commit history supplied dates, motivations for
design decisions, and the story of how the API evolved.

**The programs built on Seastar.** ScyllaDB and Redpanda, the two largest
open-source Seastar applications, were studied to decide what this book
should cover. A script counted how often each public Seastar API is used
in each of them, so that the book would cover what real programs use,
and spend little time on what they don't. Reading their code also
revealed the idioms that experienced Seastar programmers rely on,
including several that both projects independently reinvented; these
fill much of [chapter](#ch:lessons-from-large-code-bases). No code from either project was copied into
this book. Every example was written for the book.

**Public sources.** Facts about Seastar's history and its users come from
announcements, articles, talks and project documentation, cited in the
footnotes where they are used.

**The people who built Seastar.** Seastar was created by Avi Kivity, and
it has been developed since 2014 by about 230 contributors, most of them
engineers at ScyllaDB and Redpanda. This book only describes what they
built. Its design decisions, and the lessons behind them, are theirs.

## How the book was typeset

The book is written in Pandoc's dialect of Markdown, one file per
chapter. The same source produces every edition: a PDF for print, an
HTML edition for the web, an EPUB for e-readers, and plain text. A few
small filters, written in Python, extend Markdown with what a book
needs, such as including code from the example programs and from each
version of Shoal. Those programs are compiled and run as part of
checking the book, with two compilers and two C++ standards, so the code
the book shows is code that works.

The PDF is typeset with LaTeX, using the KOMA-Script book class. The
text is set in Libertinus Serif, with the title in Libertinus Serif
Display, and the code is set in Source Code Pro. The book edition is
printed on B5 pages; editions on A4 and US Letter pages, and in
grayscale for black-and-white printing, are built from the same source.

The sea star on the title page is an original drawing, made for this
book by a short Python program that Claude wrote, `art/seastar.py` in
the book's source. The program describes the animal geometrically, as a
central disc joined smoothly to five tapering arms, lit from the upper
left, and then draws it in the manner of the pen-and-ink illustrations
in old natural-history books: thousands of tiny dots, denser where the
surface turns away from the light, and rows of knobs along each arm. The
editor chose this drawing among several styles and variations that the
program produced. Because the program uses random numbers, and a newer
version of Python could produce slightly different ones, the drawing it
produced, `art/seastar.svg`, is kept in the book's source, and the book
uses that file, not the program.
