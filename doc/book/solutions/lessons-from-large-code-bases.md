# Lessons from large Seastar code bases {#sol:lessons-from-large-code-bases}

## Exercise 1

```{.cpp include="examples/solutions/lessons-from-large-code-bases-ex1.cc"}
```

It printed, in one run:

```
succeeded at attempt 3
gave up after 6 attempts, 166 ms: never
```

The number of attempts and the time vary from run to run, since the
waits are random, but the retry always gave up before the deadline, 200
milliseconds away: after the fifth or sixth failure, the next wait,
which by then is 40 to 100 milliseconds, would have ended after it.

What the deadline doesn't bound is the call itself: `f()` may take
longer than what is left of the deadline, and the retry only finds out
when it returns. To bound that too, give `f()` the deadline, for example
by waiting for it with `with_timeout()` ([chapter](#ch:time)), if it's
harmless to abandon, or better, by passing the deadline to the request
itself, so that the other server stops working on it too. This is what
Redpanda's `retry_chain_node` is for: the work done under a retry gets a
node whose deadline is no later than the retry's.

## Exercise 2

```{.cpp include="examples/solutions/lessons-from-large-code-bases-ex2.cc"}
```

It printed, with 2 shards and with 4:

```
in parallel: a got 1 of 2 locks, b got 1 of 2: a deadlock
shard 0 first: a got 2 of 2 locks, b got 2 of 2
in parallel: a got 3 of 4 locks, b got 1 of 4: a deadlock
shard 0 first: a got 4 of 4 locks, b got 4 of 4
```

In parallel, `a` took shard 0's lock at once, and `b` shard 1's, since
each was delayed only on the other shard; 20 milliseconds later, each
waited for the lock that the other held, until the waits timed out.
(With 4 shards, `a` also got shards 2 and 3, since its messages to them
were sent before `b`'s.) With shard 0 first, `b`, delayed there, found `a`
holding shard 0's lock, and waited for it, without holding anything; `a`
then got shard 1's lock, 20 milliseconds late but uncontested, held all
of them for 10 milliseconds, and released them, and then `b` got them
all.

`take()` is a coroutine with parameters, not a lambda with captures,
since the lambda given to `map()` is a temporary that is destroyed as
soon as it returns the future, and a coroutine lambda's captures may be
destroyed with it, while the coroutine still runs ([chapter](#ch:object-lifetime)).
