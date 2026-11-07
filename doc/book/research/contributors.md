# Seastar's most prolific contributors (for chapter 1's history)

Computed 2026-11-05 from `git log --no-merges` on Seastar master (a2ea73a0a),
with `.mailmap` applied (`%aN`). Lines = added + deleted, capping each commit
at 3,000 lines to discount imports and generated files.

Criterion (revised with Nadav): lines of code, not number of commits (which
favors many tiny fixes): at least about 4,700 changed lines, in at least 15
distinct months. The ranking by lines gives the same top 17 as the commit
criterion, under any per-commit cap (1,000, 3,000 or none); Nadav then lowered
the bar to include Raphael S. Carvalho (4,713 lines, 32 months), which also
admits Stephan Dollberg (4,939 lines, 21 months, Redpanda). The next ones,
Takuya ASADA (3,460) and Duarte Nunes (3,348), are well below. Redpanda
contributors on the list: Travis Downs (226 of 251 commits from redpanda.com)
and Stephan Dollberg.

Lines added only (ignoring deletions) give the same top 17, but no clean cut
below them: Raphael S. Carvalho 3,134, Takuya ASADA 3,094, Stephan Dollberg
2,309, Duarte Nunes 2,208, Piotr Sarna 2,197. Rafael Ávila de Espíndola added
6,833 and deleted 7,746 lines (much cleanup work). Decision (Nadav): keep the
changed-lines list of 19, worded "contributed thousands of lines of changes".

Previous criterion: more than 100 commits, in at least 15 distinct months.

| Name | Commits | Lines | Active months | First | Last |
|---|---|---|---|---|---|
| Avi Kivity | 1916 | 100154 | 136 | 2014-08 | 2026-10 |
| Pavel Emelyanov | 1307 | 40370 | 79 | 2019-10 | 2026-10 |
| Kefu Chai | 802 | 29176 | 70 | 2018-04 | 2026-10 |
| Benny Halevy | 706 | 24850 | 78 | 2018-10 | 2026-10 |
| Gleb Natapov | 465 | 23909 | 62 | 2014-09 | 2025-01 |
| Calle Wilund | 210 | 21988 | 70 | 2014-10 | 2026-06 |
| Glauber Costa | 292 | 16624 | 58 | 2014-10 | 2020-06 |
| Amnon Heiman | 228 | 16018 | 60 | 2015-01 | 2025-07 |
| Tomasz Grabiec | 430 | 14730 | 51 | 2014-08 | 2026-09 |
| Rafael Ávila de Espíndola | 496 | 14579 | 22 | 2018-12 | 2020-09 |
| Jesse Haber-Kucharsky | 155 | 14183 | 17 | 2017-06 | 2019-05 |
| Vlad Zolotarov | 266 | 13395 | 57 | 2014-11 | 2026-03 |
| Travis Downs | 251 | 13013 | 43 | 2020-12 | 2026-10 |
| Asias He | 182 | 12209 | 17 | 2014-09 | 2022-06 |
| Nadav Har'El | 188 | 9008 | 57 | 2014-10 | 2026-10 |
| Botond Dénes | 160 | 7369 | 49 | 2017-08 | 2026-02 |
| Paweł Dziepak | 136 | 7064 | 30 | 2015-03 | 2019-03 |
| Stephan Dollberg | 50 | 4939 | 21 | 2023-04 | 2026-09 |
| Raphael S. Carvalho | 80 | 4713 | 32 | 2014-09 | 2024-05 |

Just below the cut (fewer than 100 commits): Takuya ASADA (86), Raphael S.
Carvalho (80), Duarte Nunes (76), Piotr Sarna (76), Noah Watkins (53, 6.4K
lines), Stephan Dollberg (50).

## How many contributors (for chapter 1's history)

Computed 2026-11-06 with `research/contributors.py --end 2026-11-06`, whose
header explains the method. People are deduplicated: one person per GitHub
account, address or normalized name (after `.mailmap`), plus two pairs merged
by hand. Counting raw names or addresses overcounts: there are 282 distinct
addresses and 247 distinct names, but 229 people.

- All time: 9,559 changes by **229 people** (the chapter says "about 230";
  an earlier draft said 250, from counting names).
- 2025-11-06 to 2026-11-06, by the date a change entered master: **797
  changes by 63 people**. ScyllaDB 476 (60%), Redpanda 214 (27%), other 107
  (13%), so **87%** from the two companies (the chapter says "almost nine in
  ten"; an earlier draft said "three quarters", from email domains alone).
- Six engineers committed from personal addresses: Noah Watkins (42 changes),
  Nicolae Vartolomei (10) and Willem Kaufmann (5) of Redpanda, and Witold
  Formański (8), Dmitry Kropachev (4) and Jakub Czyszczoń (3) of ScyllaDB.
  Each also commits to his company's repository with a company address.
- The other 13%: Kefu Chai, now at Proxmox (36); Łukasz Kurowski, independent
  (25, mostly test conversions to coroutines and Boost removal); and about 25
  people with 1 to 8 changes each (students, one-time fixes, and one change
  from Matan Breizman of Ceph's Crimson team).
- The name variants merged here carry only a few changes each, so they
  don't affect the list of most prolific contributors above.
