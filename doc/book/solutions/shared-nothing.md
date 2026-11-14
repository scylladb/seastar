# Shared nothing {#sol:shared-nothing}

## Exercise 1

Here is the program. Shard 1 now returns the rows themselves, and shard 0
counts the frees that cross shards while it destroys them:

```{.cpp include="examples/solutions/shared-nothing-ex1.cc"}
```

With two shards, it prints

```
shard 0 got 3 rows
destroying them on shard 0 took 4 cross-shard frees
```

The vector and its strings were moved to shard 0, which owns them from
then on, so shard 0 frees them. But their memory was allocated on shard 1:
the vector's array of three `sstring`s, and the characters of each
string, since each is longer than the fifteen characters that an `sstring`
keeps inside itself. That's four blocks of shard 1's memory, and four
frees on the wrong shard.

The program still works because Seastar's allocator expects this. A block
freed on the wrong shard isn't returned to shard 0's memory, which it
doesn't belong to: it goes onto a list that belongs to shard 1, which
shard 1 empties later, taking the blocks back, as [chapter](#ch:memory)
explains. Such a free
costs more than an ordinary one, and the memory comes back to shard 1
only later, but nothing breaks. That's why returning values by value from
another shard is fine, and common: a few cross-shard frees for each
result is a small price.

What can't cross shards this way is anything that belongs to shard 1's
other machinery, not just its memory. An `lw_shared_ptr` in the result
would be destroyed on shard 0, which would change its count there, while
copies of it may still exist on shard 1: the race that `wrong-shard.cc`
showed. A timer would be destroyed on shard 0 too, and a timer that is
armed removes itself, when it's destroyed, from the timers of the shard
that destroys it: shard 0 would remove it from its own timers, where it
isn't, and shard 1's would still point to it, after it was destroyed. For such objects, the answer is the
`foreign_ptr` that the chapter's `foreign.cc` used: the object stays on
its shard, and is destroyed there.

## Exercise 2

Shard 1 builds the table in an `lw_shared_ptr`, and gives shard 0 a
`foreign_ptr` to it; shard 0 copies that, and gives the copy to shard 2:

```{.cpp include="examples/solutions/shared-nothing-ex2.cc"}
```

It prints

```
shard 0 has two pointers, owned by shards 1 and 1
shard 2 reads 2 rows, and waits
shard 0 drops its pointer
the table is destroyed on shard 1
shard 0 is done
```

`copy()` runs on shard 1, the owner: it sends a message there, copies the
`lw_shared_ptr` on shard 1, which increments the table's count, and wraps
the copy in a new `foreign_ptr`, whose owner is therefore shard 1 too,
as the first line shows. It must run there, because the count of an
`lw_shared_ptr` may change only on the shard that created it; copying the
wrapped pointer on shard 0 would be exactly the bug of `wrong-shard.cc`.
That's also why `copy()` returns a future: making the copy takes a
message to the owner, and its reply.

The table is destroyed once, on shard 1, and only after both pointers are
gone. When shard 2's lambda finishes, `submit_to()` destroys it, back on
shard 0 (as exercise 3 shows, that's where `submit_to()` destroys its
function), and with it `t2`, whose destructor sends shard 1 a message to
destroy its `lw_shared_ptr`. That brings the count down to one. Then shard 0's
`destroy()` sends the second message, the count reaches zero, and shard 1
destroys the table. Each `foreign_ptr` carries one reference, and every
change to the count happens on shard 1.

This is how several shards can share an object that one of them owns:
each holds a `foreign_ptr` that `copy()` made, and the owner destroys the
object when the last one is gone. It works, but every copy and every
destruction costs a message to the owner, so it suits objects that are
shared for a long time, such as a configuration, rather than ones that
are copied for every request.

## Exercise 3

Built against a `debug` Seastar, `wrong-shard.cc` stops on the first
shard other than shard 0 that copies the pointer. In our runs, that was
shard 1:

```
ERROR ... [shard 1:main] seastar - shared_ptr accessed on non-owner cpu ...
```

but any of the other shards could be first, since they all start at the
same moment. The message comes from the check that `debug` mode adds to
every `lw_shared_ptr` and `shared_ptr`: each remembers the thread that
created it, and checks it whenever its count changes.

The second part has two variants:

```{.cpp include="examples/solutions/shared-nothing-ex3.cc"}
```

In the first, only shard 0 copies the pointer, into the lambda's
capture, and shard 1 only reads the vector through it. That isn't a bug,
and a `debug` build doesn't object. `submit_to()`'s documentation explains
why: "This movement and the eventual destruction of func are both done
in the _calling_ core". The lambda is made on shard 0, moved on shard 0,
run on shard 1, and destroyed back on shard 0, so the count changes only
on shard 0. Reading the vector on shard 1 is safe too, because shard 0
doesn't change it while shard 1 reads, and the message that carried the
lambda to shard 1 made shard 0's writes visible there.

In the second, the lambda copies the pointer once more, on shard 1, and
destroys the copy there. That's a bug: the count changes on shard 1,
while shard 0 may use the same pointer. A `release` build doesn't notice,
and prints the right count, since only one change crosses shards, and
nothing else happens to touch the count at that moment. A `debug` build
stops at once:

```
ERROR ... [shard 1:main] seastar - shared_ptr accessed on non-owner cpu ...
```

So the rule isn't "never send a Seastar pointer to another shard", but
"never change its count on another shard": copying it, destroying a copy,
or assigning it. A pointer that only travels inside a lambda given to
`submit_to()` is safe; one that the other shard copies, or keeps after the
lambda ends, isn't. Since it's easy to cross that line without
noticing, the `debug` build's check is worth having, and a test suite
that runs in `debug` mode, as [chapter](#ch:testing) recommends, finds such
bugs the first time they happen, rather than in the rare run where two
shards collide.

## Exercise 4

With four shards and 4 GB:

```{.cpp include="examples/solutions/shared-nothing-ex4.cc"}
```

prints (sorted)

```
shard 0 allocated 512 MB
shard 0 couldn't allocate 900 MB
shard 0 couldn't allocate 2048 MB
shard 0: 492 MB free
shard 1: 1005 MB free
shard 2: 1005 MB free
shard 3: 1005 MB free
```

The 2 GB fails for the reason that the chapter gave: shard 0 has only its
own 1 GB, and the 3 GB free on the other shards can't help it. The 900 MB
is the surprise, since shard 0 had more than 1,000 MB free. Seastar's
allocator serves a large allocation from a free block of pages whose size
is a power of two, rounding the request up, so 900 MB needs a free block
of 1 GB, and a shard of 1 GB that has allocated anything at all doesn't
have one. [Chapter](#ch:memory) explains why the allocator works this way.
In practice, a single allocation can't be much larger than half of a
shard's memory.

So a program whose data isn't divided evenly among its shards runs out of
memory when its fullest shard does, however much the others have free:
here, shard 0 kept 512 MB, and had less than half of its memory left,
while each of the others still had nearly all of its own. And a program
that allocates a few huge blocks, such as one large hash table, can fail
long before its shard is full. The cure for both is the same: divide the
data, so that each shard holds a share of it, in many smaller pieces.
[Chapter](#ch:designing-a-sharded-application) shows how.
