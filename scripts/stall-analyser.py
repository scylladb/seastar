#!/usr/bin/env python

import argparse
import sys
import re

import addr2line
from collections import defaultdict
from itertools import chain, dropwhile
from typing import Iterator, Self


def get_command_line_parser():
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
    parser.add_argument('-w', '--width', type=int, default=0,
                        help='Smart trim of long lines to width characters (0=disabled)')
    parser.add_argument('-d', '--direction', choices=['bottom-up', 'top-down'], default='bottom-up',
                        help='Print graph bottom-up (default, callees first) or top-down (callers first)')
    parser.add_argument('-m', '--minimum', type=int, dest='tmin', default=0,
                        help='Process only stalls lasting the given time, in milliseconds, or longer')
    parser.add_argument('-b', '--branch-threshold', type=float, default=0.03,
                        help='Drop branches responsible for less than this threshold relative to the previous level, not global. (default 3%%)')
    parser.add_argument('--format', choices=['graph', 'trace'], default='graph',
                        help='The output format, default is %(default)s. `trace` is suitable as input for flamegraph.pl')
    parser.add_argument('file', nargs='?',
                        type=argparse.FileType('r'),
                        default=sys.stdin,
                        help='File containing reactor stall backtraces. Read from stdin if missing.')
    return parser


class Node:
    def __init__(self, addr: str):
        self.addr = addr
        self.callers = {}
        self.callees = {}
        self.printed = False

    def __repr__(self):
        return f"Node({self.addr})"

    class Link:
        def __init__(self, node: "Node", t: int) -> None:
            self.node = node
            self.total = t
            self.count = 1

        def __eq__(self, other: Self) -> bool:
            return self.total == other.total and self.count == other.count

        def __ne__(self, other: Self):
            return not (self == other)

        def __lt__(self, other: Self) -> bool:
            return self.total < other.total or self.total == other.total and self.count < other.count

        def add(self, t: int) -> bool:
            self.total += t
            self.count += 1

    def link_caller(self, t: int, n: Self) -> Self:
        if n.addr in self.callers:
            link = self.callers[n.addr]
            link.add(t)
            n.callees[self.addr].add(t)
        else:
            self.callers[n.addr] = self.Link(n, t)
            n.callees[self.addr] = self.Link(self, t)
        return n

    def unlink_caller(self, addr: str) -> None:
        link = self.callers.pop(addr)
        link.node.callees.pop(self.addr)

    def link_callee(self, t: int, n) -> Self:
        if n.addr in self.callees:
            link = self.callees[n.addr]
            link.add(t)
            n.callers[self.addr].add(t)
        else:
            self.callees[n.addr] = self.Link(n, t)
            n.callers[self.addr] = self.Link(self, t)
        return n

    def unlink_callee(self, addr: str) -> None:
        link = self.callees.pop(addr)
        link.node.callers.pop(self.addr)

    def sorted_links(self, links: list, descending=True) -> list:
        return sorted([l for l in links if l.node.addr], reverse=descending)

    def sorted_callers(self, descending=True) -> list:
        return self.sorted_links(self.callers.values(), descending)

    def sorted_callees(self, descending=True) -> list:
        return self.sorted_links(self.callees.values(), descending)


class Graph:
    def __init__(self, resolver: addr2line.BacktraceResolver):
        self.resolver = resolver
        # Each node in the tree contains:
        self.count = 0
        self.total = 0
        self.nodes = dict[str, Node]()
        self.tail = Node('')
        self.head = Node('')

    def empty(self):
        return not self.nodes

    def __bool__(self):
        return not self.empty()

    def process_trace(self, trace: list[str], t: int) -> None:
        # process each backtrace and insert it to the tree
        #
        # The backtraces are assumed to be in bottom-up order, i.e.
        # the first address indicates the innermost frame and the last
        # address is in the outermost, in calling order.
        #
        # This helps identifying closely related reactor stalls
        # where a code path that stalls may be called from multiple
        # call sites.
        node = None
        for addr in trace:
            node = self.add(node, t, addr)
        self.add_head(t, node)

    def add(self, prev: Node, t: int, addr: str):
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

    def add_head(self, t: int, n: Node):
        self.head.link_callee(t, n)

    def smart_print(self, lines: str, width: int):
        def _print(l: str, width: int):
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

    def print_graph(self, direction: str, width: int, branch_threshold: float):
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

        def _prefix(prefix_list: list):
            prefix = ''
            for p in prefix_list:
                prefix += p
            return prefix

        def _recursive_print_graph(n: Node, total: int = 0, count: int = 0, level: int = -1, idx: int = 0, out_of: int = 0, rel: float = 1.0, prefix_list: list[str] = [], skip_stats: bool = False):
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
                if self.resolver:
                    lines = self.resolver.resolve_address(n.addr).splitlines()
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
                    if rel < branch_threshold:
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


class StackCollapse:
    # collapse stall backtraces into single lines
    def __init__(self, resolver: addr2line.BacktraceResolver) -> None:
        self.resolver = resolver
        # track every stack and the its total sample counts
        self.collapsed = defaultdict(int)
        # to match lines like
        # (inlined by) position_in_partition::tri_compare::operator() at ././position_in_partition.hh:485
        self.pattern = re.compile(r'''(.+)\s                 # function signature
                                      at\s                   #
                                      [^:]+:(?:\?|\d+)       # <source>:<line number>''', re.X)

    def process_trace(self, frames: list[str], count: int) -> None:
        # each stall report is mapped to a line of perf samples
        # so the output looks like:
        # row_cache::update;row_cache::do_update;row_cache::upgrade_entry 42
        # from outer-most caller to the inner-most callee, and 42 is the time
        # in ms, but we use it for the count of samples.
        self.collapsed[';'.join(frames)] += count

    def _annotate_func(self, line: str) -> str:
        # sample input:
        #   (inlined by) position_in_partition::tri_compare::operator() at ././position_in_partition.hh:485
        # sample output:
        #   position_in_partition::tri_compare::operator()_[i]
        if line.startswith("??"):
            return ""

        inlined_prefix = ' (inlined by) '
        inlined = line.startswith(inlined_prefix)
        if inlined:
            line = line[len(inlined_prefix):]

        matched = self.pattern.match(line)
        assert matched, f"bad line: {line}"
        func = matched.groups()[0]
        # annotations
        if inlined:
            func += "_[i]"
        return func

    def _resolve(self, addr: str) -> Iterator[str]:
        lines = self.resolver.resolve_address(addr).splitlines()
        return (self._annotate_func(line) for line in lines)

    def print_graph(self, *_) -> None:
        for stack, count in self.collapsed.items():
            frames = filter(lambda frame: frame,
                            chain.from_iterable(self._resolve(addr) for addr in stack.split(';')))
            print(';'.join(reversed(list(frames))), count)


def print_stats(tally: dict, tmin: int) -> None:
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


def print_command_line_options(args):
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


def main():
    args = get_command_line_parser().parse_args()
    comment = re.compile(r'^\s*#')
    pattern = re.compile(r"Reactor stalled for (?P<stall>\d+) ms on shard (?P<shard>\d+).*Backtrace:")
    expected_input_format = "Expected one or more lines ending with: 'Reactor stalled for <n> ms on shard <i>. Backtrace: <addr> [<addr> ...]'"
    address_threshold = int(args.address_threshold, 0)
    # map from stall time in ms to the count of the stall time
    tally = {}
    resolver = None
    if args.executable:
        resolver = addr2line.BacktraceResolver(executable=args.executable,
                                               concise=not args.full_function_names)
    if args.format == 'graph':
        render = Graph(resolver)
    else:
        render = StackCollapse(resolver)

    for s in args.file:
        if comment.search(s):
            continue
        # parse log line like:
        # ... scylla[7795]: Reactor stalled for 860 ms on shard 4. Backtrace: 0x4f002d2 0x4efef30 0x4f001e0
        m = pattern.search(s)
        if not m:
            continue
        # extract the time in ms
        trace = s[m.span()[1]:].split()
        t = int(m.group("stall"))
        # and the addresses after "Backtrace:"
        tally[t] = tally.pop(t, 0) + 1
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
            trace = list(dropwhile(lambda addr: int(addr, 0) >= address_threshold, trace))
        if t >= args.tmin:
            if not trace:
                print(f"""Invalid input line: '{s.strip()}'
{expected_input_format}
Please run `stall-analyser.py --help` for usage instruction""", file=sys.stderr)
                sys.exit(1)
            render.process_trace(trace, t)

    try:
        if not render:
            print(f"""No input data found.
{expected_input_format}
Please run `stall-analyser.py --help` for usage instruction""", file=sys.stderr)
            sys.exit(1)
        if args.format == 'graph':
            print_command_line_options(args)
            print_stats(tally, args.tmin)
        render.print_graph(args.direction, args.width, args.branch_threshold)
    except BrokenPipeError:
        pass


if __name__ == '__main__':
    main()
