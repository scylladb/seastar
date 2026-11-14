# Cleans a flame graph's folded stacks (stackcollapse-perf.pl's output) of a
# Seastar program, so that the names that matter fit in their boxes:
#  - merges the shards: cuts every stack below the reactor's loop,
#    seastar::reactor::do_run, where each shard's thread runs its tasks,
#    and drops the stacks of other threads;
#  - shortens each name: no template arguments, no parameters, and no
#    seastar:: or std:: namespaces;
#  - drops the frames of C++'s plumbing, such as std::invoke and
#    operator(), and a frame that repeats the one below it, and shows the
#    reactor's loop, and the coroutine machinery, as one frame each;
# and adds up the stacks that are then the same.
#   clean_stacks.py < folded.txt > cleaned.txt
import re
import sys
from collections import Counter

LOOP = 'seastar::reactor::do_run'

# Frames that only pass the call on.
PLUMBING = re.compile(r'^(__invoke\w*|invoke\w*|futurize\w*|futurize::\w+|operator|operator\(\)|decltype'
                      r'|apply|__apply_impl|_ZSt\w+|coroutine_handle::resume|then_impl\w*'
                      r'|call|noncopyable_function::.*|indirect_vtable_for.*|direct_vtable_for.*'
                      r'|__invoke_result|invoke_result|\w*_vtable\w*|repeater|input_stream::consume)$')


RENAME = {
    'coroutine_traits_base::promise_type::run_and_dispose': 'coroutine',
    'basic_sstring::str': 'sstring::str',
    'basic_sstring::compare': 'sstring::compare',
    'basic_sstring::size': 'sstring::size',
    'basic_sstring::basic_sstring': 'sstring::sstring',
}


def strip_brackets(name, open_, close):
    """Removes every bracketed part, nested ones too: "f<a<b>>::g" -> "f::g"."""
    out, depth = [], 0
    for c in name:
        if c == open_:
            depth += 1
        elif c == close and depth:
            depth -= 1
        elif not depth:
            out.append(c)
    return ''.join(out)


def shorten(name):
    name = re.sub(r'\[clone [^]]*\]|\(\.\w+\)|\.(cold|part\.\d+|constprop\.\d+)', '', name)
    name = name.replace('operator()', 'operator')
    name = strip_brackets(name, '(', ')')
    name = strip_brackets(name, '<', '>')
    name = re.sub(r'^(void|bool|auto|seastar::future|std::\w+) ', '', name.strip())
    for ns in ('seastar::internal::', 'seastar::', 'std::__n4861::', 'std::__detail::',
               'std::', 'fmt::v11::detail::', 'fmt::v11::'):
        name = name.replace(ns, '')
    return name.strip() or '?'


stacks = Counter()
for line in sys.stdin:
    stack, count = line.rstrip('\n').rsplit(' ', 1)
    frames = stack.split(';')
    starts = [i for i, f in enumerate(frames) if f.startswith(LOOP)]
    if not starts:
        continue
    kept = []
    for f in frames[starts[0]:]:
        f = shorten(f)
        # The reactor's loop, and the coroutine machinery, each as one frame.
        f = RENAME.get(f, f)
        if f.startswith('reactor::'):
            f = 'reactor'
        if PLUMBING.match(f):
            continue
        # Only the function's own name, without its class: the boxes are
        # narrow, and the class is usually clear from the stack below.
        f = f.rsplit('::', 1)[-1]
        if kept and kept[-1] == f:
            continue
        kept.append(f)
    stacks[';'.join(kept)] += int(count)
for stack, count in stacks.items():
    print(stack, count)
