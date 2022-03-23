# Shared token bucket

## Intro

The classical token bucket has two parameters -- rate and limit. The rate
is the amount of tokens that are put into bucket per some period of time
(say -- second), the limit is the maximum amount of tokens that can be
accumulated in the bucket. The process of regeneration of tokens this way
is called "replenishing" below. When a request needs to be served it should
try to get a certain amount of tokens from the bucket.

The shared token bucket implements the above model for seastar sharded
architecture. The implementation doesn't use locks and is built on atomic
arithmetics.

## Theory

### Rovers

The bucket is implemented as a pair of increasing counters -- tail and
head rovers. The consumer of tokens advances the tail rover. To replenish
tokens into the bucket the head rover is advanced.

    +--------------------------------------------------------------------->
    ^             ^
    tail          head

    grab N tokens:

    +--------------------------------------------------------------------->
    .---> ^       ^
          tail    head

    replenish N tokens:

    +--------------------------------------------------------------------->
          ^       .---> ^
          tail          head

It's possible that after grab the tail would overrun the head and will occur
in front of it. This would mean that there's not enough tokens in the bucket
and that some amount of tokens were claimed from it.

    grab a lot of tokens:

    +--------------------------------------------------------------------->
          .------------ ^ --> ^
                        head  tail

To check if the tokens were grabbed the caller needs to check if the head is
(still) in front of the tail. This approach adds the ability for the consumers
to line up in the queue when they all try to grab tokens from a contented
bucket. The "ticket lock" model works the same way.

### Capped release

The implementation additionally support so called "capped release". This is
when tokens are not replenished from nowhere, but leak into the main bucket
from another bucket into which the caller should explicitly put them. This mode
can be useful in cases when the token bucket guards the entrance into some
location that can temporarily (or constantly, but in that case it would denote
a bucket misconfiguration) slow down and stop handling tokens at the given
rate. To prevent token bucket from over-subscribing the guardee at those times,
the second bucket can be refilled with the completions coming from the latter.

In terms of rovers this is implemented with the help of a third rover called
ceiling (or ceil in the code). This ceil rover actually defines the upper
limit at which the head rover may point. Respectively, putting tokens into
the second bucket (it's called releasing below) means advancing the ceil.

## Practice

### API

To work with the token bucket there are 4 calls:

 * `grab(N)` -- grabs a certain amount of tokens from the bucket and returns
   back the resulting "tail" rover value. The value is useless per-se and is
   only needed to call the deficiency() method

 * `replenish(time)` -- tries to replenish tokens into the bucket. The amount
   of replenished tokens is how many had accumulated since last replenish
   till the `time` parameter

 * `release(N)` -- releases the given number of token making them available
   for replenishment. Only works if capped release is turned on by the
   template parameter, otherwise asserts

 * `deficiency(tail)` -- returns back the number of tokens that were claimed
   from the bucket but that are not yet there. Non-zero number means that the
   bucket is contented and the request dispatching should be delayed

### Example

For example, the simple dispatch loop may look like this

    while (true) {
        request = get_next_request()
        tail = token_bucket.grab(request.cost())
        while (token_bucket.deficiency(tail)) {
            yield
        }
        request.dispatch()
    }

And in the background there should run a timer calling `token_bucket.replenish(now())`

Additionally, if there's a need to cap the token bucket with the real request serving
rate, upon request completion one should call `token_bucket.release(request.cost())`
