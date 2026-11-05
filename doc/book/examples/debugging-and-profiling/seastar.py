# A small gdb extension for Seastar programs: a pretty printer for sstring,
# and a command that lists the task queues of every shard. Load it into gdb
# with "source seastar.py", in a running program or in a core dump.
import gdb


# snippet: printer
# Prints a seastar::sstring as the string that it holds. A short string is
# kept inside the object, in u.internal, whose size is then 0 or more; a
# long one is allocated, and u.internal.size is negative.
class sstring_printer:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        u = self.val['u']
        size = int(u['internal']['size'])
        if size >= 0:
            return u['internal']['str'].string(length=size)
        return u['external']['str'].string(length=int(u['external']['size']))

    def display_hint(self):
        return 'string'


def lookup_printer(val):
    tag = val.type.strip_typedefs().tag
    if tag and tag.startswith('seastar::basic_sstring<'):
        return sstring_printer(val)
    return None


gdb.pretty_printers.append(lookup_printer)
# end snippet: printer


# snippet: command
def unique_ptr_target(p):
    """What a std::unique_ptr with the default deleter points to, or None:
    such a unique_ptr holds nothing but the pointer."""
    target = p.type.template_argument(0)
    ptr = p.address.cast(target.pointer().pointer()).dereference()
    return ptr.dereference() if ptr else None


class task_queues(gdb.Command):
    """seastar-task-queues: for every shard, its task queues, one for each
    scheduling group, with their shares and how many tasks are ready."""

    def __init__(self):
        super().__init__('seastar-task-queues', gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        selected = gdb.selected_thread()
        seen = set()
        try:
            for thread in sorted(gdb.selected_inferior().threads(), key=lambda t: t.num):
                # Each shard's thread has its own reactor, in the
                # thread-local variable local_engine; other threads don't.
                # io_uring's worker threads, "iou-wrk-...", have no
                # variables of their own: in a running program gdb shows
                # them with those of the shard that started them, and in a
                # core dump, with none.
                thread.switch()
                try:
                    engine = gdb.parse_and_eval('seastar::local_engine')
                except gdb.error:
                    continue
                if not engine or int(engine['_id']) in seen:
                    continue
                seen.add(int(engine['_id']))
                # A core dump doesn't keep the threads' names.
                name = f', "{thread.name}"' if thread.name else ''
                print(f'shard {int(engine["_id"])} (gdb thread {thread.num}{name}):')
                queues = engine['_task_queues']['_M_elems']
                for i in range(queues.type.strip_typedefs().range()[1] + 1):
                    q = unique_ptr_target(queues[i])
                    if q is None:
                        continue
                    impl = q['_q']['_impl']
                    ready = int(impl['end']) - int(impl['begin'])
                    print(f'  {q["_name"]}: shares {float(q["_shares"]):g}, '
                          f'{ready} tasks ready')
        finally:
            selected.switch()


task_queues()
# end snippet: command
