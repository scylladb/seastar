import importlib
import os
import sys
import types

# Load perftune.py as a module without executing main().
# Because the script is now guarded by ``if __name__ == '__main__'``,
# a plain exec_module is safe and needs no SystemExit swallowing.
_perftune_path = os.path.join(os.path.dirname(__file__), '..', '..', 'perftune.py')
loader = importlib.machinery.SourceFileLoader('perftune', _perftune_path)
_mod = types.ModuleType(loader.name)
sys.modules['perftune'] = _mod
loader.exec_module(_mod)
