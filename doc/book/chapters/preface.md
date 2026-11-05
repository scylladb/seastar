# Preface {#preface .unnumbered}

This book teaches you to write server applications with Seastar, the C++
framework behind ScyllaDB, Redpanda and other systems that get the most out
of modern hardware. It starts from the reasons Seastar exists and from your
first Seastar program, and it ends with the techniques that large Seastar
applications rely on in production: isolating their components from each
other, making sure no long task holds up all the others, and finding the bugs
that asynchronous code is prone to.

It is written for programmers who know C++ and want to write fast server
applications. "Fast" has two meanings here, and Seastar is about both:
high throughput, serving as many requests per second as the hardware
allows, and low latency, answering each request quickly and predictably,
even under heavy load. You don't need any experience with asynchronous
programming, with Seastar, or with high-performance programming in
general; the book explains each idea when it is needed.

Seastar relies on parts of C++ that many programmers rarely use, and
on a few that hold surprises even for experienced ones. Where the book
needs such a topic, it explains it in a short section marked as a *C++
corner*. If you already know the subject, skip the corner; if you don't,
it tells you just enough to follow the rest of the chapter.
[Appendix](#app:cpp-corners) collects all of them.

To build and run the programs in this book, you need a Linux computer and
a recent compiler. Seastar supports the last two major versions of GCC and
of Clang, and the last two C++ standards, which at the time of writing are
C++23 and C++26; everything in this book works with C++23. Any computer
with a few cores will do for learning. Some experiments in the later
chapters are more interesting with more cores and a fast NVMe drive, but
none of them require it. Getting Seastar, and building your first program
with it, is the subject of [chapter](#ch:getting-started).

Rather than a collection of unrelated examples, the book develops one
program from chapter to chapter, adding to it whatever each chapter
teaches. The program, called Shoal, is a key-value server compatible with
Redis, so the standard Redis tools and client libraries can talk to it,
and standard benchmarks can measure it. Shoal starts as a small single-
core server in [chapter](#ch:coroutines), and by the end of the book it is
sharded across all cores, stores its data on disk, replicates it to
another server, and reports metrics.

Every version of Shoal in the book is a complete program that builds,
runs and passes its tests. The text shows only the code that is new in
each chapter; the full source of every version is available online.
*(The address will be given here before the book is published.)*

The book has eight parts. The first four are best read in order; each
builds on the ones before it. After that, you can read the parts in
whatever order your work requires.

- [Part](#part:introduction), *Getting to know Seastar*, explains the
  ideas behind Seastar, its history and its uses, takes you on a quick
  tour of Seastar programming, and shows how to set up Seastar for
  programs of your own.
- [Part](#part:one-shard), *Coroutines on one shard*, teaches asynchronous
  programming on a single core: how to write asynchronous code, how Seastar
  schedules it, concurrency, the lifetime of objects, time, coordination
  and error handling. This is the foundation for everything else.
- [Part](#part:many-cores), *Many cores*, shows how Seastar divides the
  machine into shards that share nothing, how shards communicate, and how
  to design an application around them.
- [Part](#part:io), *I/O*, covers networking and storage in depth, and the
  I/O scheduler that keeps latency low while the disks are busy.
- [Part](#part:performance), *Isolation and performance*, is about keeping
  an application fast and responsive under load: scheduling groups,
  memory, code that isn't asynchronous, and stalls.
- [Part](#part:more-seastar), *More Seastar*, covers another way of
  writing asynchronous code, how coroutines work underneath, and two of
  Seastar's biggest subsystems: HTTP and RPC.
- [Part](#part:production), *From development to production*, is about
  observing, testing, debugging and deploying Seastar applications.
- [Part](#part:world), *Seastar in the world*, surveys the projects built
  on Seastar, and the lessons that their large code bases teach.

The appendices are for reference: building Seastar, its command-line
options, a map of its API, Shoal's commands, a glossary, a catalog of
Seastar's smaller utilities, and the C++ corners gathered in one place.

Most chapters end with exercises. Some are short; some ask you to extend
Shoal in a way that a later chapter also takes up, so that you can compare
your solution with the book's.

Despite every effort, this book surely contains mistakes, and Seastar will
keep changing after it is written. If you find a mistake, or something
that is out of date, please report it on Seastar's issue tracker,
<https://github.com/scylladb/seastar/issues>.
