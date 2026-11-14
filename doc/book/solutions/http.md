# HTTP {#sol:http}

## Exercise 1

The solution is a patch to Shoal version 13,
`examples/solutions/shoal-admin-put.patch`. `PUT` runs a `SET`, through
the code that executes commands, as `DELETE` runs a `DEL`, so the value
is logged, and its memory counted, as a client's would be:

```{.cpp include="examples/solutions/shoal-admin-put.patch" file="commands.cc" snippet="set-key"}
```

A body of a gigabyte should be refused, and before it's read. Without
more settings, Seastar's server reads the whole body of a request, of any
length, into one string, before it calls the handler: a gigabyte of
contiguous memory, the large allocation that [chapter](#ch:memory) warned
against, which may well fail, for an API that isn't the data path and
has no use for a value that large. Seastar's own field for that string,
`request::content`, is in fact deprecated, and with `-Werror` the first
version of this solution, which used it, didn't compile. So the admin
server sets two things on every shard: content streaming, so that the
handler reads the body from a stream, and a limit of a megabyte on
`Content-Length`, beyond which the server answers 413 Payload Too Large
without reading the body at all. A body sent in chunks has no
`Content-Length`, so the handler counts as it reads, and stops at the
limit too:

```{.cpp include="examples/solutions/shoal-admin-put.patch" file="admin.cc" snippet="put-key"}
```

```
$ curl -X PUT --data-binary hello localhost:8080/v1/keys/greeting
$ redis-cli GET greeting
"hello"
$ curl -X PUT --data-binary @2MB localhost:8080/v1/keys/big
Content length limit (1048576) exceeded: 2000000
$ curl -X PUT -H 'Transfer-Encoding: chunked' --data-binary @2MB \
    localhost:8080/v1/keys/big
{"message": "a value may have at most 1048576 bytes", "code": 413}
```

The patch's tests check each case, the gigabyte one with a request that
only declares a `Content-Length` of a gigabyte, and gets 413 without
sending a byte of it. The first version of that test sent a real body of
two megabytes, and failed with a broken pipe: the server answers, and
closes the connection, before the client has finished sending, as it
should.

## Exercise 2

The patch, `examples/solutions/shoal-admin-config.patch`, adds the
operation to `admin.json`, with a new model, `option`, of a name and a
value. Shoal's options are private to `main.cc`, so `main.cc` makes a
list of names and values, which `set_routes()` copies into each shard's
handler:

```{.cpp include="examples/solutions/shoal-admin-config.patch" file="main.cc" snippet="config-of"}
```

The handler checks the query's parameters itself, because Seastar
doesn't: an operation's description lists its parameters, but the
generated code only checks that the required ones are there, and ignores
any others. A misspelled parameter would then be silently ignored, and
`?nmae=maxmemory` would return all the options:

```{.cpp include="examples/solutions/shoal-admin-config.patch" file="admin.cc" snippet="get-config"}
```

```
$ curl 'localhost:8080/v1/config?name=maxmemory'
[{"name": "maxmemory", "value": "100000000"}]
$ curl 'localhost:8080/v1/config?nmae=maxmemory'
{"message": "unknown parameter 'nmae'", "code": 400}
```

## Exercise 3

The patch, `examples/solutions/shoal-admin-keys-by-shard.patch`, replaces
`stream_range_as_array()` with a writer of its own: a function that gets
the reply's output stream, and writes the array itself, asking one shard
at a time for its keys. As in `set_admin_routes()`, the lambda only calls
a coroutine function, and gives it copies of what it needs:

```{.cpp include="examples/solutions/shoal-admin-keys-by-shard.patch" file="admin.cc" snippet="get-keys"}
```

`keys_on()` replaces `matching_keys()`: it finds one shard's keys, in the
keys group. The reply now holds one shard's keys at a time, and the
output stream's buffer. The cost is that the shards search one after
another, rather than all at once, and that an error is reported late:
by the time the writer runs, the server has sent the status, 200 OK, so a
failure on the third shard can only cut the reply short, which the client
sees as a broken chunked reply, not as an error status. Version 13's
tests, among them `test_keys`, with 20,000 keys, pass unchanged.

## Exercise 4

`examples/solutions/http-ex4.cc`'s client deletes the keys with
`max_concurrent_for_each()` ([chapter](#ch:concurrency-within-a-shard)), and asks the
client how many connections it opened:

```{.cpp include="examples/solutions/http-ex4.cc" snippet="delete-all"}
```

The second client gets a pool of ten connections, through the
constructor that takes a connection factory:

```{.cpp include="examples/solutions/http-ex4.cc" snippet="clients"}
```

```
default pool: 0 keys left, 100 connections opened
pool of 10: 0 keys left, 10 connections opened
```

A client opens a connection for each request that it sends while all its
connections are busy, up to its pool's limit, 100 by default, and then
makes requests wait for a connection to be free. HTTP/1.1 sends one
request at a time on a connection, so a hundred concurrent requests need
a hundred connections, and with a pool of ten, ninety of them wait. For
a server on another machine, a hundred connections from each shard of
each client may be more than it wants; the pool's limit is how a client
bounds them.

## Exercise 5

The admin API requires the certificate too:

```
$ shoal --admin-tls-port 8443 --tls-cert-file server.crt \
    --tls-key-file server.key --tls-ca-cert-file ca.crt --tls-auth-clients yes
$ curl --cacert ca.crt https://localhost:8443/v1/info
curl: (56) ... tlsv13 alert certificate required
$ curl --cacert ca.crt --cert stranger.crt --key stranger.key \
    https://localhost:8443/v1/info
curl: (56) Send failure: Broken pipe
$ curl --cacert ca.crt --cert client.crt --key client.key \
    https://localhost:8443/v1/info
{"shards": 2, "keys": 0, "used_memory": 0, "maxmemory": 0, "evicted_keys": 0}
```

Whether a server asks for a client's certificate is a property of its
credentials, set by the builder's `set_client_auth()`, and both ports
listen with the same credentials. To require a certificate on one port
only, each shard needs two credentials objects, built from the same
files with different `set_client_auth()`, and the reloading of
[chapter](#ch:networking) must rebuild both when the files change. That
reloading function is given the builder, so it can call `rebuild()` on
each.
