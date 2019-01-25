#!/usr/bin/env python3

# This script takes the single-page HTML output from pandoc - tutorial.html -
# and splits it into many pages in split/: one page index.html for the table
# of contents, and an additional page for each chapter. We make sure that
# links from the TOC to each chapter, and also links across chapters,
# continue to work correctly, and also had links from each chapter back to
# the TOC, as well as to the next and previous chapters.


# Copyright (C) 2018 ScyllaDB.
#
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

import re
titles = {}
sections = {}
def links(out, chapter):
    if chapter == 0:
        return
    out.write('<A HREF="index.html">Back to table of contents</A>. ')
    try:
        out.write('Previous: <A HREF="' + str(chapter-1) +'.html">' + str(chapter-1) + '. ' + titles[chapter-1] + '</A>. ')
    except:
        pass
    try:
        out.write('Next: <A HREF="' + str(chapter+1) +'.html">' + str(chapter+1) + '. ' + titles[chapter+1] + '</A>. ')
    except:
        pass
def flush(chapter, header, chunk):
    fn = 'index.html' if chapter == 0 else str(chapter) + '.html'
    with open('split/' + fn, 'w') as out:
        out.write(header)
        links(out, chapter)
        out.write(chunk)
        links(out, chapter)
        out.write('</body></html>')
with open("tutorial.html") as f:
    chunk = ""
    # Chapter currently being read. Set to 0 while reading the TOC, or
    # numbers > 0 while reading a chapter
    chapter = None
    for line in f:
        if line == '<div id="TOC">\n' or line =='<nav id="TOC">\n':
            header = chunk
            chapter = 0
            chunk = ""
        elif line.startswith('<h1 id="'):
            flush(chapter, header, chunk)
            chunk = ""
            chapter += 1
        elif chapter == 0 and line.startswith('<li><a href="#'):
            # For all sections, remember the mapping from name-with-dashes
            # to the chapter number they are in in "sections". We need this
            # to support links to other sections.
            match = re.search('href="#([^"]*)".*>([0-9]+)[.<]', line)
            if match:
                sections[match.group(1)] = match.group(2)
            # replace the link to '#section' with number N.M to chapterN#section
            match = re.match('^(.*href=")(#.*>)([0-9]+)([.<].*)$', line)
            line = match.group(1) + match.group(3) + '.html' + match.group(2) + match.group(3) + match.group(4) + '\n'
            # For chapters, remember the mapping from number to name in the
            # map "titles", so we can use them later in links to next and
            # previous chapter
            match = re.search('>([0-9]+)</span> (.*)</a>', line)
            if match:
                titles[int(match.group(1))] = match.group(2)
        elif chapter != 0:
            # In a chapter we can have a link to a different subsection, which
            # looks like <a href="#some-title">Some title</A>. We need to
            # replace this to refer to the right file after the split.
            line = re.sub('<a href="#([^"]*)">([^<]*)</a>', lambda m: '<a href="' + sections[m.group(1)] + '.html#' + m.group(1) + '">' + m.group(2) + '</a>', line)
        chunk += line
    flush(chapter, header, chunk)
