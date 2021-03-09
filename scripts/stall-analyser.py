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
  `total` - the total sum of stalls, in milliseconds
            of all reactor stalls that mention the address.
  `count` - number of backtraces going through the address

When printed, the graph is traversed in descending `total` order
so that we print stall backtraces that are frequent and long.

Each node in the printed output is preceded with [level#index pct%],
where `level` is the level of that node in the graph (0 are root nodes),
`index` is the index in the parent node's list of callers, and
`pct` is the percantage of this link's `total` time out of the parent's
`total` stall time.

When given an executable, addresses are decoding using `addr2line`
""")
parser.add_argument('--address-threshold', default='0x100000000',
                    help='Skip common backtrace prefix terminated by one or more addresses greater or equal to the threshold (0=disabled)')
parser.add_argument('-e', '--executable',
                    help='Decode addresses to lines using given executable')
parser.add_argument('-w', '--width', type=int, default=0,
                    help='Smart trim of long lines to width characters (0=disabled)')
parser.add_argument('file', nargs='?',
                    help='File containing reactor stall backtraces. Read from stdin if missing.')

args = parser.parse_args()

resolver = addr2line.BacktraceResolver(executable=args.executable) if args.executable else None

class Node:
    def __init__(self, addr:str, total:int=0, count:int=0):
        self.addr = addr
        self.total = total
        self.count = count if count else 1 if total else 0
        self.callers = {}
        self.callees = {}
        self.printed = False

    def __repr__(self):
        return f"Node({self.addr}: total={self.total}, count={self.count}, avg={round(self.total/self.count) if self.count else 0})"

    class Link:
        def __init__(self, node, t:int):
            self.node = node
            self.total = t

        def __eq__(self, other):
            return self.total == other.total

        def __ne__(self, other):
            return not (self == other)

        def __lt__(self, other):
            return self.total < other.total

        def add(self, t:int):
            self.total += t

    def add(self, t:int):
        self.total += t
        self.count += 1
        return self

    def link_node(self, t:int, n):
        if n.addr in self.callers:
            link = self.callers[n.addr]
            link.add(t)
            n.callees[self.addr].add(t)
        else:
            self.callers[n.addr] = self.Link(n, t)
            n.callees[self.addr] = self.Link(self, t)
        return n

    def sorted_callers(self, descending=True):
        return sorted(list(self.callers.values()), reverse=descending)

    def sorted_callees(self, descending=True):
        return sorted(list(self.callees.values()), reverse=descending)

class Graph:
    def __init__(self):
        # Each node in the tree contains:
        self.count = 0
        self.total = 0
        self.nodes = {}
        self.tails = {}

    def add(self, prev:Node, t:int, addr:str):
        if addr in self.nodes:
            n = self.nodes[addr]
            n.add(t)
        else:
            n = Node(addr, t)
            self.nodes[addr] = n
        if prev:
            prev.link_node(t, n)
            if addr in self.tails:
                self.tails.pop(addr)
        elif not n.callees and not addr in self.tails:
            self.tails[addr] = n
        return n
    
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
            _print(l, width)

    def print_graph(self):
        def _recursive_print_graph(n:Node, level:int=0, idx:int=0, rel:float=1.0):
            avg = round(n.total / n.count)
            l = f"{'|'*level}[{level}#{idx} {round(100*rel)}%] "
            cont_indent = len(l) - level - 1
            l += f"addr={n.addr} total={n.total} count={n.count} avg={avg}"
            if resolver:
                l += ': '
                l += re.sub('\n +', f"\n{'|'*(level+1)}{' '*cont_indent}", resolver.resolve_address(n.addr))
            self.smart_print(l, args.width)
            if n.printed:
                print(f"{'|'*level}(see above)")
                return
            n.printed = True
            i = 0
            for link in n.sorted_callers():
                _recursive_print_graph(link.node, level + 1, i, link.total / n.total)
                i += 1

        for r in self.tails.values():
            print('')
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

address_threshold = int(args.address_threshold, 0)

input = open(args.file) if args.file else sys.stdin
count = 0
pattern = re.compile('Reactor stall')
for s in input:
    if not pattern.search(s):
        continue
    count += 1
    trace = s.split()
    for i in range(0, len(trace)):
        if trace[i] == 'Reactor':
            i += 3
            break
    t = int(trace[i])
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
    process_graph(t, trace)

try:
    graph.print_graph()
except BrokenPipeError:
    pass
