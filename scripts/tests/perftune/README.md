# perftune.py — unit tests

Unit tests for `seastar/scripts/perftune.py`.

## Requirements

| Package      | Purpose                                         |
|--------------|-------------------------------------------------|
| `pyudev`     | udev device enumeration (used by `perftune.py`) |
| `psutil`     | system memory stats (used by `perftune.py`)     |
| `pyyaml`     | YAML config parsing (used by `perftune.py`)     |
| `setuptools` | `distutils` shim for newer Python versions      |
| `pytest-cov` | coverage reporting                              |

## Setup

Install dependencies into the active Python environment:

```bash
pip install -r tests/perftune/requirements.txt
```

The tests are run with `python3 -m pytest` rather than the bare `pytest`
shim to ensure the same Python interpreter and installed packages are used
regardless of what `$PATH` resolves `pytest` to.

## Running the tests

All commands are run from `seastar/scripts/`.

```bash
# Run all tests with coverage (default — configured in pyproject.toml)
python3 -m pytest

# Verbose output
python3 -m pytest -v

# Filter by test name
python3 -m pytest -k net

# Stop on first failure
python3 -m pytest -x
```

Coverage is enabled automatically via `pyproject.toml`:

```toml
[tool.pytest.ini_options]
testpaths = ["tests/perftune"]
addopts = "--cov=perftune --cov-report=term-missing"
```

A passing run looks like:

```
410 passed in 0.31s

Name          Stmts   Miss  Cover   Missing
-------------------------------------------
perftune.py    1057      2    99%   442, 2128
-------------------------------------------
TOTAL          1057      2    99%
```

## Test file layout

| File                              | What it covers                                                                               |
|-----------------------------------|----------------------------------------------------------------------------------------------|
| `test_command_helpers.py`         | Module-level helpers: `perftune_print`, `run_*_command`, `fwriteln`, `restart_irqbalance`, … |
| `test_config.py`                  | Options-file loading (`load_config` / `dump_config`)                                         |
| `test_cpu_topology.py`            | `auto_detect_irq_mask` — all CPU-count branches and asymmetric-NUMA error                    |
| `test_perf_tuner_base.py`         | `PerfTunerBase` init paths, properties, mask helpers, AWS host detection                     |
| `test_net_perf_tuner.py`          | `NetPerfTuner` end-to-end (with real `__init__`)                                             |
| `test_net_perf_tuner_instance.py` | `NetPerfTuner` internal helpers via bypassed `__init__`                                      |
| `test_net_perf_tuner_methods.py`  | All private `NetPerfTuner` methods in isolation                                              |
| `test_disk_perf_tuner.py`         | All `DiskPerfTuner` methods and `__init__`                                                   |
| `test_system_perf_tuner.py`       | `SystemPerfTuner` and clocksource management                                                 |
| `test_clocksource.py`             | `ClocksourceManager` in isolation                                                            |
| `test_dataclasses.py`             | `perftune` dataclasses and their defaults                                                    |
| `test_pure_functions.py`          | Pure utility functions with no I/O                                                           |
| `test_main.py`                    | `main()` entry point: argument validation, tuner creation, output modes, exception handling  |

## Note on the pytest executable

`/opt/homebrew/bin/pytest` (or wherever your distro puts it) may belong to a
different Python version than `python3`.  Always prefer:

```bash
python3 -m pytest
```

For full isolation a virtualenv is an option:

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r tests/perftune/requirements.txt pytest pytest-cov
python3 -m pytest
```
