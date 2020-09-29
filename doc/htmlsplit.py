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

from xml.etree import ElementTree
import copy

# chapter number to chapter title
titles = {}
# section id => chapter number
sections = {}


def add_prologue(chap_num, body):
    p = ElementTree.SubElement(body, 'p')
    e = ElementTree.SubElement(p, 'a',
                               href='index.html')
    e.text = 'Back to table of contents'
    e.tail = '.'
    prev_index = chap_num - 1
    if prev_index > 0:
        e.tail += " Previous: "
        prev_title = titles[prev_index]
        e = ElementTree.SubElement(p, 'a',
                                   href=f'{prev_index}.html')
        e.text = f'{prev_index} {prev_title}'
        e.tail = '.'
    next_index = chap_num + 1
    if next_index < len(titles) - 1:
        e.tail += " Next: "
        next_title = titles[next_index]
        e = ElementTree.SubElement(p, 'a',
                                   href=f'{next_index}.html')
        e.text = f'{next_index} {next_title}'
        e.tail = '.'


def handle_toc(toc):
    for chap in toc.iterfind('./ul/li'):
        chap_href_elem = next(chap.iterfind('./a[@href]'))
        chap_num_elem = next(chap_href_elem.iterfind(
            './span[@class="toc-section-number"]'))
        # For chapters, remember the mapping from number to name in the
        # map "titles", so we can use them later in links to next and
        # previous chapter
        chap_num = int(chap_num_elem.text)
        titles[chap_num] = chap_num_elem.tail.strip()

        # For all sections, remember the mapping from name-with-dashes
        # to the chapter number they are in in "sections". We need this
        # to support links to other sections.
        href = chap_href_elem.get('href')
        sections[href] = chap_num
        for section in chap.iterfind('.//ul/li/a[@href]'):
            href = section.get('href')
            # replace the link to '#section' with number N.M to chapterN#section
            if href.startswith('#'):
                sections[href] = chap_num


def fix_links(e):
    for link in e.findall('.//a[@href]'):
        href = link.get('href')
        if href.startswith('#') and href in sections:
            # In a chapter we can have a link to a different subsection, which
            # looks like <a href="#some-title">Some title</A>. We need to
            # replace this to refer to the right file after the split.
            chap_num = sections[href]
            link.set('href', f'{chap_num}.html{href}')


def remove_ns_prefix(tree):
    prefix = '{http://www.w3.org/1999/xhtml}'
    for e in tree.iter():
        if e.tag.startswith(prefix):
            e.tag = e.tag[len(prefix):]


def get_chap_num(element):
    data_num = e.get('data-number')
    if data_num:
        return int(data_num)
    data_num = e.findtext('./span[@class="header-section-number"]')
    if data_num:
        return int(data_num)
    assert data_num, "section number not found"

tree = ElementTree.parse('tutorial.html')
for e in tree.iter():
    remove_ns_prefix(e)
template = copy.deepcopy(tree.getroot())
template_body = next(template.iterfind('./body'))
template_body.clear()

# iterate through the children elements in body
# body element is composed of
#  - header
#  - toc
#  - h1,h2,p,...
# h1 marks the beginning of a chapter

chap_num = 0
chap_tree = None
for e in next(tree.iterfind('./body')):
    if e.tag == 'header':
        template_body.append(e)
    elif e.get('id') == 'TOC':
        handle_toc(e)
        fix_links(e)
        toc_tree = ElementTree.ElementTree(copy.deepcopy(template))
        body = next(toc_tree.iterfind('./body'))
        body.append(e)
        toc_tree.write('split/index.html', method='html')
    elif e.tag == 'h1':
        assert titles
        assert sections
        if chap_num > 0:
            chap_tree.write(f'split/{chap_num}.html', method='html')
        chap_num = get_chap_num(e)
        chap_tree = ElementTree.ElementTree(copy.deepcopy(template))
        body = next(chap_tree.iterfind('./body'))
        add_prologue(chap_num, body)
        body.append(e)
    else:
        assert chap_tree is not None
        fix_links(e)
        body = next(chap_tree.iterfind('./body'))
        body.append(e)

chap_tree.write(f'split/{chap_num}.html', method='html')
