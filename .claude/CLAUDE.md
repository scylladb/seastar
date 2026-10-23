# Seastar Project Instructions

## Building

The project uses CMake with ninja as the build system. Build directories are located in `build/<mode>`.

### Build Commands

**Build specific target:**
```bash
ninja -C build/<mode> <target>
```

Where `<mode>` is typically:
- `dev` - development build
- `debug` - debug build
- `release` - release build
- `sanitize` - sanitizer build (ASan/UBSan)

**List available targets:**
```bash
ninja -C build/<mode> -t targets
```

**List all targets (including test targets):**
```bash
ninja -C build/<mode> -t targets all
```

### Common Build Examples

```bash
# Build ioinfo tool in dev mode
ninja -C build/dev apps/io_tester/ioinfo

# Build io_tester in dev mode
ninja -C build/dev apps/io_tester/io_tester

# Build all in release mode
ninja -C build/release

# List all available targets
ninja -C build/dev -t targets all
```

### Important Notes

- **Check for existing build directories first** - if `build/<mode>` exists, use it directly; otherwise run `./configure.py --mode=<mode>` to create it
- **Always use `ninja -C build/<mode>`** from the repository root
- **Do NOT use** plain `ninja` from the root directory - it will fail with "Execute Ninja in a build directory"
- The working directory during builds should typically be the repository root
- After code changes to specific components, rebuild only that target for faster iteration

## Testing

The project uses CTest for running tests. Test executables are built in `build/<mode>/tests/unit/` and `build/<mode>/tests/perf/`.

### Test Runners

**`./test.py`** (preferred) - Python wrapper with useful options:
```bash
./test.py --mode dev --name <pattern>  # Run specific test in dev mode
./test.py --mode dev --fast            # Run only fast tests
./test.py --mode dev -v                # Verbose output
```

Options:
- `--mode <mode>` - Run for specific build mode (otherwise runs ALL modes)
- `--name <pattern>` - Filter tests by name
- `--fast` - Run only fast tests
- `--timeout <secs>` - Set timeout (default 300s)
- `--smp <n>` / `-c <n>` - Number of threads for multi-core tests (default 2)
- `--verbose` / `-v` - Verbose output
- `--offline` - Disable tests that access the internet

**`ninja -C build/<mode> test`** - Basic ctest invocation for a single mode. Less flexible, no `--output-on-failure` by default.

### Running Specific Tests

```bash
# Using test.py (preferred)
./test.py --mode dev --name circular_buffer

# Using ctest directly
ctest --test-dir build/dev -R circular_buffer --output-on-failure

# Run test executable directly
./build/dev/tests/unit/circular_buffer_test
```

**List all available tests:**
```bash
ctest --test-dir build/dev -N
```

### Test Types

- **Unit tests**: Located in `tests/unit/`, use Boost.Test framework
- **Performance tests**: Located in `tests/perf/`, benchmark various components

### Important Notes

- **When iterating on code changes**, choose an appropriate test that covers the modified code and run only that test
- **Running all tests is slow** - only run the full test suite when explicitly requested
- Most unit tests use Boost.Test and accept standard Boost.Test command line options
- Some tests (like allocator tests) may run for the default duration (5 seconds) unless interrupted

## Making Changes

- Make only the functional changes the task needs, and motivate each one in the commit message.
- Keep the existing logic intact unless the change requires otherwise. Do not reorder statements,
  restructure code or rename things that work, as this hides the real change and risks regressions.
- Do not reformat or restyle code you are not otherwise changing. Cosmetic changes are made only
  when asked for, and then as separate commits.
- Adding or updating comments that explain the change is welcome.
- Prefix the commit subject with the area it touches, e.g. `core: ...`, `rpc: ...`, `tests: ...`
  (see `doc/contributing.md`), and follow `coding-style.md`.

## Git and GitHub Workflow

- Never push a branch, open a pull request or force-push without being asked to.
- Push to the contributor's fork, never to `scylladb/seastar` directly. Pull requests target
  `master`. Changes may also be sent as patches to the mailing list, see `CONTRIBUTING.md`.
- After a force-push, comment on the pull request describing what changed since the previous
  push and what verification was run locally.

## External Communication

- When an AI agent posts a comment on GitHub, the mailing list or an issue tracker, it says so
  and names the model, and states on whose behalf it comments, e.g.
  `(<model name>, on behalf of @<user>)`.
- Reference commits and issues by their full URLs, which the trackers render as links with the
  live title, rather than by hand-copied titles that go stale.

## Style

Minimum C++ version is C++20. Use C++20 API where possible.

Follow existing style.

Comment all public methods carefully following the style of the same source file (e.g., doxygen if existing comments use that style).
