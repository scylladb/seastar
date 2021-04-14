#!/usr/bin/env python

import argparse
import sys
import re

import addr2line

parser = argparse.ArgumentParser(formatter_class=argparse.RawDescriptionHelpFormatter,
    description='A reactor stall backtrace graph analyser.',
    epilog="""
stall-analyser helps analyze a series of reactor-stall backtraces using a graph.
Each node in the graph includes:
  `addr` - a program address
Each link in the graph includes:
  `total` - the total sum of stalls, in milliseconds
            of all reactor stalls that pass via this caller/callee link.
  `count` - number of backtraces going through the link.

When printed, the graph is traversed in descending `total` order
to emphasize stall backtraces that are frequent and long.

Each node in the printed output is preceded with [level#index pct%],
where `level` is the level of that node in the graph (0 are root nodes),
`index` is the index in the parent node's list of callers/callees, and
`pct` is the percantage of this link's `total` time relative to
its siblings.

When given an executable, addresses are decoding using `addr2line`
""")
parser.add_argument('--address-threshold', default='0x100000000',
                    help='Skip common backtrace prefix terminated by one or more addresses greater or equal to the threshold (0=disabled)')
parser.add_argument('-e', '--executable',
                    help='Decode addresses to lines using given executable')
parser.add_argument('-f', '--full-function-names', action='store_const', const=True, default=False,
                    help="When demangling C++ function names, display all information, including the type of the function's parameters. Otherwise, they are omitted (see `c++filt(1) -p`).")
parser.add_argument('-w', '--width', type=int, default=None,
                    help='Smart trim of long lines to width characters (0=disabled)')
parser.add_argument('-d', '--direction', choices=['bottom-up', 'top-down'], default='bottom-up',
                    help='Print graph bottom-up (default, callees first) or top-down (callers first)')
parser.add_argument('-m', '--minimum', type=int, default=None,
                    help='Process only stalls lasting the given time, in milliseconds, or longer')
parser.add_argument('-b', '--branch-threshold', type=float, default=0.05,
                    help='Drop branches responsible for less than this threshold relative to the previous level, not global. (default 5%%)')
parser.add_argument('file', nargs='?',
                    help='File containing reactor stall backtraces. Read from stdin if missing.')

args = parser.parse_args()

resolver = addr2line.BacktraceResolver(executable=args.executable, concise=not args.full_function_names) if args.executable else None

class Node:
    def __init__(self, addr:str):
        self.addr = addr
        self.callers = {}
        self.callees = {}
        self.printed = False

    def __repr__(self):
        return f"Node({self.addr})"

    class Link:
        def __init__(self, node, t:int):
            self.node = node
            self.total = t
            self.count = 1

        def __eq__(self, other):
            return self.total == other.total and self.count == other.count

        def __ne__(self, other):
            return not (self == other)

        def __lt__(self, other):
            return self.total < other.total or self.total == other.total and self.count < other.count

        def add(self, t:int):
            self.total += t
            self.count += 1

    def link_caller(self, t:int, n):
        if n.addr in self.callers:
            link = self.callers[n.addr]
            link.add(t)
            n.callees[self.addr].add(t)
        else:
            self.callers[n.addr] = self.Link(n, t)
            n.callees[self.addr] = self.Link(self, t)
        return n

    def unlink_caller(self, addr:str):
        link = self.callers.pop(addr)
        link.node.callees.pop(self.addr)

    def link_callee(self, t:int, n):
        if n.addr in self.callees:
            link = self.callees[n.addr]
            link.add(t)
            n.callers[self.addr].add(t)
        else:
            self.callees[n.addr] = self.Link(n, t)
            n.callers[self.addr] = self.Link(self, t)
        return n

    def unlink_callee(self, addr:str):
        link = self.callees.pop(addr)
        link.node.callers.pop(self.addr)

    def sorted_links(self, links:list, descending=True):
        return sorted([l for l in links if l.node.addr], reverse=descending)

    def sorted_callers(self, descending=True):
        return self.sorted_links(self.callers.values(), descending)

    def sorted_callees(self, descending=True):
        return self.sorted_links(self.callees.values(), descending)

class Graph:
    def __init__(self):
        # Each node in the tree contains:
        self.count = 0
        self.total = 0
        self.nodes = {}
        self.tail = Node('')
        self.head = Node('')

    def add(self, prev:Node, t:int, addr:str):
        if addr in self.nodes:
            n = self.nodes[addr]
        else:
            n = Node(addr)
            self.nodes[addr] = n
        if prev:
            if prev.addr in self.head.callees:
                self.head.unlink_callee(prev.addr)
            prev.link_caller(t, n)
            if addr in self.tail.callers:
                self.tail.unlink_caller(addr)
        elif not n.callees or addr in self.tail.callers:
            self.tail.link_caller(t, n)
        return n

    def add_head(self, t:int, n:Node):
        self.head.link_callee(t, n)

    def smart_print(self, lines:str, width:int):
        def _print(l:str, width:int):
            if not width or len(l) <= width:
                print(l)
                return
            i = l.rfind(" at ")
            if i < 0:
                print(l[:width])
                return
            sfx = l[i:]
            w = width - len(sfx) - 3
            if w > 0:
                pfx = l[:w]
            else:
                pfx = ""
            print(f"{pfx}...{sfx}")
        for l in lines.splitlines():
            if l:
                _print(l, width)

    def print_graph(self, direction:str):
        top_down = (direction == 'top-down')
        print(f"""
This graph is printed in {direction} order, where {'callers' if top_down else 'callees'} are printed first.
Use --direction={'bottom-up' if top_down else 'top-down'} to print {'callees' if top_down else 'callers'} first.

[level#index/out_of pct%] below denotes:
  level  - nesting level in the graph
  index  - index of node among to its siblings
  out_of - number of siblings
  pct    - percentage of total stall time of this call relative to its siblings
""")
        varargs = vars(args)
        clopts = ""
        for k in varargs.keys():
            val = varargs[k]
            opt = re.sub('_', '-', k)
            if val is None:
                continue
            elif not isinstance(val, bool):
                clopts += f" --{opt}={val}"
            elif val:
                clopts += f" --{opt}"
        print(f"Command line options:{clopts}\n")

        def _prefix(prefix_list:list):
            prefix = ''
            for p in prefix_list:
                prefix += p
            return prefix

        def _recursive_print_graph(n:Node, total:int=0, count:int=0, level:int=-1, idx:int=0, out_of:int=0, rel:float=1.0, prefix_list=[], skip_stats=False):
            nonlocal top_down
            if level >= 0:
                avg = round(total / count) if count else 0
                prefix = _prefix(prefix_list)
                p = '+' if idx == 1 or idx == out_of else '|'
                p += '+'
                l = f"[{level}#{idx}/{out_of} {round(100*rel)}%]"
                cont_indent = len(l) + 1
                if skip_stats:
                    l = f"{' ' * (len(l)-2)} -"
                    stats = ''
                else:
                    stats = f" total={total} count={count} avg={avg}"
                l = f"{prefix}{p}{l} addr={n.addr}{stats}"
                p = "| "
                if resolver:
                    lines = resolver.resolve_address(n.addr).splitlines()
                    if len(lines) == 1:
                        li = lines[0]
                        if li.startswith("??"):
                            l += f": {lines[0]}"
                        else:
                            l += f":\n{prefix}{p}{' '*cont_indent}{li.strip()}"
                    else:
                        l += ":\n"
                        if top_down:
                            lines.reverse()
                        for li in lines:
                            l += f"{prefix}{p}{' '*cont_indent}{li.strip()}\n"
                width = args.width or 0
                self.smart_print(l, width)
                if n.printed:
                    print(f"{prefix}-> continued at addr={n.addr} above")
                    return
                n.printed = True
            next = n.sorted_callees() if top_down else n.sorted_callers()
            if not next:
                return
            link = next[0]
            if level >= 0 and len(next) == 1 and link.total == total and link.count == count:
                _recursive_print_graph(link.node, link.total, link.count, level, idx, out_of, rel, prefix_list, skip_stats=True)
            else:
                total = sum(link.total for link in next)
                next_prefix_list = prefix_list + ["| " if idx < out_of else "  "] if level >= 0 else []
                i = 1
                last_idx = len(next)
                omitted_idx = 0
                omitted_total = 0
                omitted_count = 0
                for link in next:
                    rel = link.total / total
                    if rel < args.branch_threshold:
                        if not omitted_idx:
                            omitted_idx = i
                        omitted_total += link.total
                        omitted_count += link.count
                    else:
                        _recursive_print_graph(link.node, link.total, link.count, level + 1, i, last_idx, rel, next_prefix_list)
                    i += 1
                if omitted_idx:
                    prefix = _prefix(next_prefix_list)
                    p = '++'
                    rel = omitted_total / total
                    avg = round(omitted_total / omitted_count) if count else 0
                    l = f"[{level+1}#{omitted_idx}/{last_idx} {round(100*rel)}%]"
                    print(f"{prefix}{p}{l} {last_idx - omitted_idx + 1} more branches total={omitted_total} count={omitted_count} avg={avg}")

        r = self.head if top_down else self.tail
        _recursive_print_graph(r)

graph = Graph()

# process each backtrace and insert it to the tree
#
# The backtraces are assumed to be in bottom-up order, i.e.
# the first address indicates the innermost frame and the last
# address is in the outermost, in calling order.
#
# This helps identifying closely related reactor stalls
# where a code path that stalls may be called from multiple
# call sites.
def process_graph(t: int, trace: list[str]):
    n = None
    for addr in trace:
        n = graph.add(n, t, addr)
    graph.add_head(t, n)

address_threshold = int(args.address_threshold, 0)
tally = {}

def print_stats(tally:dict, tmin):
    data = []
    total_time = 0
    total_count = 0
    processed_count = 0
    min_time = 1000000
    max_time = 0
    median = None
    p95 = None
    p99 = None
    p999 = None
    for t in sorted(tally.keys()):
        count = tally[t]
        data.append((t, count))
        total_time += t * count
        if t < min_time:
            min_time = t
        if t > max_time:
            max_time = t
        total_count += count
        if t >= tmin:
            processed_count += count
    running_count = 0
    for (t, count) in data:
        running_count += count
        if median is None and running_count >= total_count / 2:
            median = t
        elif p95 is None and running_count >= (total_count * 95) / 100:
            p95 = t
        elif p99 is None and running_count >= (total_count * 99) / 100:
            p99 = t
        elif p999 is None and running_count >= (total_count * 999) / 1000:
            p999 = t
    print(f"Processed {total_count} stalls lasting a total of {total_time} milliseconds.")
    if tmin:
        print(f"Of which, {processed_count} lasted {tmin} milliseconds or longer.")
    avg_time = total_time / total_count if total_count else 0
    print(f"min={min_time} avg={avg_time:.1f} median={median} p95={p95} p99={p99} p999={p999} max={max_time}")

input = open(args.file) if args.file else sys.stdin
count = 0
comment = re.compile('^\s*#')
pattern = re.compile('Reactor stall')
for s in input:
    if comment.search(s) or not pattern.search(s):
        continue
    count += 1
    trace = s.split()
    for i in range(0, len(trace)):
        if trace[i] == 'Reactor':
            i += 3
            break
    t = int(trace[i])
    tally[t] = tally.pop(t, 0) + 1
    trace = trace[i + 6:]
    # The address_threshold typically indicates a library call
    # and the backtrace up-to and including it are usually of
    # no interest as they all contain the stall backtrace geneneration code, e.g.:
    #  seastar::internal::cpu_stall_detector::generate_trace
    # void seastar::backtrace<seastar::backtrace_buffer::append_backtrace_oneline()::{lambda(seastar::frame)#1}>(seastar::backt>
    #  (inlined by) seastar::backtrace_buffer::append_backtrace_oneline() at ./build/release/seastar/./seastar/src/core/reactor.cc:771
    #  (inlined by) seastar::print_with_backtrace(seastar::backtrace_buffer&, bool) at ./build/release/seastar/./seastar/src/core/reactor.cc>
    # seastar::internal::cpu_stall_detector::generate_trace() at ./build/release/seastar/./seastar/src/core/reactor.cc:1257
    # seastar::internal::cpu_stall_detector::maybe_report() at ./build/release/seastar/./seastar/src/core/reactor.cc:1103
    #  (inlined by) seastar::internal::cpu_stall_detector::on_signal() at ./build/release/seastar/./seastar/src/core/reactor.cc:1117
    #  (inlined by) seastar::reactor::block_notifier(int) at ./build/release/seastar/./seastar/src/core/reactor.cc:1240
    # ?? ??:0
    if address_threshold:
        for i in range(0, len(trace)):
            if int(trace[i], 0) >= address_threshold:
                while int(trace[i], 0) >= address_threshold:
                    i += 1
                trace = trace[i:]
                break
    tmin = args.minimum or 0
    if t >= tmin:
        process_graph(t, trace)

try:
    print_stats(tally, tmin)
    graph.print_graph(args.direction)
except BrokenPipeError:
    pass
