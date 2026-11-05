#!/usr/bin/python3 -I
# Count Seastar's contributors, for chapter 1's history: how many people
# contributed since 2014, and how many people and changes in the last twelve
# months, and which share of those changes came from ScyllaDB and Redpanda.
#
# Usage: contributors.py [--repo DIR] [--end YYYY-MM-DD]
#
# Counting people means merging the identities of each person, because many
# contributors used several names or addresses over the years. Two identities
# are the same person if they share any of:
#   - the GitHub account that GitHub links to the commit (fetched with `gh`,
#     and cached in build/gen/github_logins.jsonl, which isn't committed);
#   - the address, except shared ones such as the mailing list's
#     (seastar-dev@googlegroups.com, used by mails sent through the list);
#   - the name, after Seastar's .mailmap, removing accents (including Polish
#     letters such as ł, which Unicode doesn't decompose), case, dots and
#     dashes, and the "via seastar-dev" suffix of mails sent through the list;
#   - an entry in SAME_PERSON below, found by reading the list by hand.
# Merges and bots don't count, only authored commits on master. The
# window is by commit date, when the change entered master.
#
# A change counts for ScyllaDB or Redpanda by its author's address. Some of
# their engineers sometimes commit with a personal address; EMPLOYER lists
# those found by checking the authors of the remaining changes against the
# ScyllaDB and Redpanda repositories. The script prints the remaining authors
# of the window, so the next run can check them the same way.

import argparse
import collections
import datetime
import json
import re
import subprocess
import unicodedata
from pathlib import Path

book = Path(__file__).resolve().parent.parent

# Identities that the automatic rules can't merge, as pairs of names.
SAME_PERSON = [
    ('Niek Bouman', 'niekbouman'),            # two addresses, one linked to GitHub
    ('Alexander Solganik', 'Solganik Alexander'),  # reversed name, two accounts
]

# Personal addresses of ScyllaDB and Redpanda engineers, as checked on
# 2026-11-06 (each one also commits to that company's repository with its
# company address).
EMPLOYER = {
    'noahwatkins@gmail.com': 'Redpanda',
    'nv@nvartolomei.com': 'Redpanda',
    'willem.kaufmann@gmail.com': 'Redpanda',
    'witekformanski@gmail.com': 'ScyllaDB',
    'dmitry.kropachev@gmail.com': 'ScyllaDB',
    'czyszczon.kuba@gmail.com': 'ScyllaDB',
}

COMPANY_DOMAINS = {
    'scylladb.com': 'ScyllaDB', 'cloudius-systems.com': 'ScyllaDB',
    'redpanda.com': 'Redpanda', 'vectorized.io': 'Redpanda',
}

def company(email):
    if email in EMPLOYER:
        return EMPLOYER[email]
    return COMPANY_DOMAINS.get(email.split('@')[-1])

def shared_address(email):
    return ('googlegroups.com' in email or email in ('', 'you@example.com')
            or email.startswith('root@') or 'localdomain' in email)

def clean(name):
    return re.sub(r"'? via seastar-dev$", '', name).strip("'\"‘’ ")

LETTERS = str.maketrans({'ł': 'l', 'Ł': 'L', 'ø': 'o', 'Ø': 'O', 'đ': 'd', 'ß': 'ss'})

def norm(name):
    name = unicodedata.normalize('NFKD', clean(name).translate(LETTERS))
    name = name.encode('ascii', 'ignore').decode().lower()
    return ' '.join(re.sub(r'[._\-]', ' ', name).split())

def github_logins(repo, shas, cache):
    """The GitHub login of each commit's author ('' if none), from the cache,
    fetching from GitHub the commits it doesn't have yet."""
    logins = {}
    if cache.exists():
        for line in cache.read_text().splitlines():
            d = json.loads(line)
            logins[d['sha']] = d['login']
    missing = set(shas) - set(logins)
    page = 1
    while missing:
        out = subprocess.run(
            ['gh', 'api', f'repos/scylladb/seastar/commits?sha=master&per_page=100&page={page}',
             '-q', '.[] | {sha: .sha, login: (.author.login // "")} | @json'],
            capture_output=True, text=True, check=True).stdout
        if not out:
            break
        with cache.open('a') as f:
            for line in out.splitlines():
                d = json.loads(line)
                if d['sha'] not in logins:
                    logins[d['sha']] = d['login']
                    f.write(line + '\n')
                missing.discard(d['sha'])
        page += 1
    return logins

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--repo', type=Path, default=book.parent.parent)
    parser.add_argument('--end', default=datetime.date.today().isoformat(),
                        help='end of the twelve-month window (default: today)')
    args = parser.parse_args()
    log = subprocess.run(['git', '-C', args.repo, 'log', 'origin/master', '--no-merges',
                          '--format=%H|%aN|%aE|%cs'], capture_output=True, text=True,
                         check=True).stdout
    commits = [line.split('|') for line in log.splitlines()]
    cache = book / 'build' / 'gen' / 'github_logins.jsonl'
    cache.parent.mkdir(parents=True, exist_ok=True)
    logins = github_logins(args.repo, [c[0] for c in commits], cache)

    parent = {}
    def find(x):
        parent.setdefault(x, x)
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x
    def union(a, b):
        parent[find(a)] = find(b)
    for sha, name, email, _ in commits:
        key = 'n:' + norm(name)
        find(key)
        if not shared_address(email.lower()):
            union('e:' + email.lower(), key)
        if logins.get(sha):
            union('g:' + logins[sha].lower(), key)
    for a, b in SAME_PERSON:
        union('n:' + norm(a), 'n:' + norm(b))
    person = lambda name: find('n:' + norm(name))

    people = {person(name) for _, name, _, _ in commits}
    print(f'All time: {len(commits)} changes by {len(people)} people.')

    end = datetime.date.fromisoformat(args.end)
    start = end.replace(year=end.year - 1)
    window = [c for c in commits if start.isoformat() <= c[3] <= end.isoformat()]
    by_company = collections.Counter(company(c[2].lower()) or 'other' for c in window)
    n = len(window)
    print(f'From {start} to {end}: {n} changes by '
          f'{len({person(c[1]) for c in window})} people.')
    for org in ('ScyllaDB', 'Redpanda', 'other'):
        print(f'  {org:9} {by_company[org]:4} ({100 * by_company[org] / n:.0f}%)')
    print('Authors of the "other" changes (check them against the companies):')
    others = collections.Counter((c[1], c[2]) for c in window if not company(c[2].lower()))
    for (name, email), count in others.most_common():
        print(f'  {count:4}  {name} <{email}>')

main()
