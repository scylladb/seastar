# Seastar — AI Agent Instructions

This file routes you to the relevant instruction files.
Do NOT load all files at once — read only what applies to your current task.

## Instruction Files

- `.claude/CLAUDE.md` — build system, test runner, making changes, Git/GitHub workflow, external communication
- `coding-style.md` — C++ coding style for all source files
- `doc/compatibility.md` — source-level backward compatibility policy for the public API
- `.agents/skills/seastar-review/SKILL.md` — review rules: compatibility, C++ module exports, internal namespaces, Doxygen
- `HACKING.md` — configuring, building, installing, development tools
- `doc/testing.md` — test macros, test organization, adding tests, fuzz tests
- `CONTRIBUTING.md` and `doc/contributing.md` — how to submit changes, commit subject convention

## Which files to read

- **Always read** `.claude/CLAUDE.md` for build/test commands and project workflow
- **If editing C++ files**: also read `coding-style.md`
- **If changing a public header** (under `include/seastar/`): also read `doc/compatibility.md` and the review skill
- **If adding or changing tests**: also read `doc/testing.md`
