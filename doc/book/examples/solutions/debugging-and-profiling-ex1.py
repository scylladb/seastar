# The solutions of chapter 30's exercises 1 and 2: two more gdb commands
# for Seastar programs, to load after seastar.py, with "source".
import gdb

PAGE_SIZE = 4096


def shards():
    """Yields (shard, gdb thread) for each of the program's shards, once."""
    selected = gdb.selected_thread()
    seen = set()
    try:
        for thread in sorted(gdb.selected_inferior().threads(), key=lambda t: t.num):
            thread.switch()
            try:
                engine = gdb.parse_and_eval('seastar::local_engine')
            except gdb.error:
                continue
            if not engine or int(engine['_id']) in seen:
                continue
            seen.add(int(engine['_id']))
            yield int(engine['_id']), thread
    finally:
        selected.switch()


# snippet: memory
class memory(gdb.Command):
    """seastar-memory: the memory of every shard, and how much is free."""

    def __init__(self):
        super().__init__('seastar-memory', gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        for shard, thread in shards():
            # Each shard's allocator, in the thread-local variable cpu_mem.
            mem = gdb.parse_and_eval("'seastar::memory::cpu_mem'")
            total = int(mem['nr_pages']) * PAGE_SIZE
            free = int(mem['nr_free_pages']) * PAGE_SIZE
            print(f'shard {shard}: {total >> 20} MB at {int(mem["memory"]):#x}, '
                  f'{free >> 20} MB free')
# end snippet: memory


# snippet: shard-of
class shard_of(gdb.Command):
    """seastar-shard-of ADDRESS: which shard's memory ADDRESS is in."""

    def __init__(self):
        super().__init__('seastar-shard-of', gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        address = int(gdb.parse_and_eval(arg).cast(gdb.lookup_type('unsigned long')))
        for shard, thread in shards():
            mem = gdb.parse_and_eval("'seastar::memory::cpu_mem'")
            start = int(mem['memory'])
            if start <= address < start + int(mem['nr_pages']) * PAGE_SIZE:
                print(f'{address:#x} is in shard {shard}\'s memory')
                return
        print(f'{address:#x} is in no shard\'s memory: a stack, a global, '
              'or memory from outside Seastar\'s allocator')
# end snippet: shard-of


memory()
shard_of()
