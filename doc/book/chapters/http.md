# HTTP {#ch:http}

Seastar includes an HTTP server and an HTTP client, built on its own
networking ([chapter](#ch:networking)), and running on every shard. Many
Seastar servers use the server for an *admin API*: a way for operators
and tools to ask a running server how it's doing, and to tell it to do
things, such as rewrite its log, over a protocol that every language and
every command-line tool speaks. ScyllaDB's REST API, which its `nodetool`
uses, and Redpanda's admin API are both built this way. Seastar's
Prometheus metrics, in [chapter](#ch:observability), are served by the
same HTTP server. And some servers carry their actual data over HTTP:
ScyllaDB's Alternator, an implementation of Amazon's DynamoDB API, and
Redpanda's HTTP proxy for Kafka, serve every request of their clients
over Seastar's HTTP server.

Shoal's version 13 gets an admin API, with five operations, defined in a
JSON file from which a script of Seastar's generates the C++ code, as
ScyllaDB and Redpanda do, and served over HTTP and HTTPS.

## A server, routes and handlers {index="HTTP server"}

[`httpd::http_server_control`]{index="`seastar::httpd::http_server_control`" .main},
from `<seastar/http/httpd.hh>`, runs an HTTP server on every shard, a
`sharded<http_server>` ([chapter](#ch:talking-between-shards)), whose
connections are spread among the shards, as Shoal's own are. Shoal
starts one when it's given an admin port:

```{.cpp include="shoal/v13/main.cc" snippet="admin-server"}
```

Each shard's server has
[`routes`]{index="`seastar::httpd::routes`" .main}, from
`<seastar/http/routes.hh>`, which map a method and a path to a
*handler*, an object with a `handle()` method that gets the request and
returns a future of the reply. `set_routes()` calls its function on every
shard, to fill that shard's routes. Most handlers are a
[`function_handler`]{index="`seastar::httpd::function_handler`"}, from
`<seastar/http/function_handlers.hh>`, which wraps a function: one that
returns a string, or a JSON value, at once, or one that returns a future
of it. A path can have parameters, such as `/v1/keys/{key}`, which the
handler reads with `get_path_param()`, and the request's query, such as
`?pattern=a*`, which it reads with `get_query_param()`.

A handler reports an error by throwing one of the exceptions of
`<seastar/http/exception.hh>`:
[`not_found_exception`]{index="`seastar::httpd::not_found_exception`"}
for 404 Not Found,
[`bad_param_exception`]{index="`seastar::httpd::bad_param_exception`"}
for 400 Bad Request, or their base,
[`base_exception`]{index="`seastar::httpd::base_exception`"}, with any
status. The server turns it into a reply with that status, and a JSON
body with the message. Any other exception becomes 500 Internal Server
Error. Shoal's `DELETE` answers 404 for a key that doesn't exist, and
`POST /v1/rewrite` answers 409 Conflict if a rewrite is already running:

```{.cpp include="shoal/v13/admin.cc" snippet="delete-key"}
```

`delete_key()` and `start_rewrite()` are functions of the store, which
`DEL` and `BGREWRITEAOF` call too, so the admin API and the Redis
protocol do exactly the same thing. `delete_key()` even runs a `DEL`
command, through the code that executes commands, so the deletion is
logged, and waits for the log as a client's `DEL` would.

## An API defined in JSON {index="`json2code`"}

A server whose API has dozens of operations, each with parameters and
JSON objects in its replies, needs the API described somewhere, both for
the people and tools that use it, and to keep the code consistent with
the description. Seastar's answer is a description in the format of
Swagger, the predecessor of OpenAPI, version 1.2 or 2.0, a JSON file
(Shoal's, like most of ScyllaDB's and Redpanda's, uses 1.2), from
which a script of Seastar's,
`seastar-json2code.py`, generates C++ code: a class for each kind of JSON
object, and an object for each operation, which a handler is attached
to. ScyllaDB describes its REST API in 30 such files, and Redpanda its
admin API in 25. Shoal's is `api/admin.json`; one of its five operations,
and the object it returns, are:

```json
{
    "path": "/v1/info",
    "operations": [{
        "method": "GET",
        "summary": "The keys and the memory of all the shards together",
        "type": "info",
        "nickname": "get_info",
        "produces": ["application/json"],
        "parameters": []
    }]
}
```

```json
"info": {
    "id": "info",
    "properties": {
        "shards": {"type": "long", "description": "The number of shards"},
        "keys": {"type": "long", "description": "The number of keys"},
        ...
    }
}
```

Seastar installs the script in the `bin` directory of its installation,
and Shoal's `CMakeLists.txt` runs it, to generate `admin.json.hh` and
`admin.json.cc` in the build directory:

```{.cmake include="shoal/v13/CMakeLists.txt" snippet="cmake"}
```

The generated code is in a namespace named after the file,
`httpd::admin_json`: the object `info` is a struct whose members are the
properties, and the operation `get_info` is an object whose `set()`
attaches a handler to a server's routes, with the operation's method and
path:

```{.cpp include="shoal/v13/admin.cc" snippet="set-routes"}
```

Note the lambdas: each only calls a coroutine function, passing it what
it needs. A coroutine lambda would reach its captures through the lambda
object, the Lambda Coroutine Fiasco of [chapter](#ch:object-lifetime).
Here the routes keep the lambdas for as long as the server runs, so it
would happen to work, but following the rule everywhere is easier than
proving, in each place, that breaking it is safe. The handler builds
the generated object, fills its members, and returns it, and Seastar
writes it as JSON:

```{.cpp include="shoal/v13/admin.cc" snippet="get-info"}
```

Each shard reports four plain numbers, which `map_reduce0()` adds up on
the shard that serves the request; only that shard builds the JSON
object. Here it is, at work:

```
$ shoal -c2 --admin-port 8080 &
$ redis-cli MSET a 1 b 22 c 333
OK
$ curl localhost:8080/v1/info
{"shards": 2, "keys": 3, "used_memory": 300, "maxmemory": 0, "evicted_keys": 0}
$ curl -X DELETE localhost:8080/v1/keys/a
$ curl -X DELETE localhost:8080/v1/keys/a
{"message": "no key 'a'", "code": 404}
```

## A reply too large to build {index="HTTP server!streaming replies"}

`GET /v1/keys` returns the keys that match a pattern, which may be
millions. Building the whole reply as one string would need it all in
memory at once, contiguous, the large allocation that
[chapter](#ch:memory) warned against.
[`json::stream_range_as_array()`]{index="`seastar::json::stream_range_as_array()`" .main},
from `<seastar/json/json_elements.hh>`, writes a range as a JSON array
instead, an element at a time, to the connection's output stream:

```{.cpp include="shoal/v13/admin.cc" snippet="get-keys"}
```

`matching_keys()` finds the keys as `KEYS` does, in the keys scheduling
group ([chapter](#ch:scheduling-groups)), and the reply's writer, which
Seastar wrote with continuations ([chapter](#ch:continuations)), checks
for preemption between elements, so a long list stalls nothing. The keys
themselves are still all in memory; exercise 3 asks how to avoid even
that.

## HTTPS {index="HTTPS"}

An admin API can delete data and rewrite logs, so it needs the same
protection as the server's own protocol: encryption, and often
certificates that identify its clients. Seastar's HTTP server serves HTTPS on a port that is given TLS
credentials when it starts listening, the same credentials that
[chapter](#ch:networking) built for Shoal's Redis port. Shoal 13 listens
for its admin API with `--admin-port` over HTTP, and with
`--admin-tls-port` over HTTPS, with the certificate of
`--tls-cert-file`:

```{.cpp include="shoal/v13/main.cc" snippet="listen-admin"}
```

Each shard listens with the credentials that its Redis server built on
that shard, which shard 0 rebuilds on every shard when the certificate
files change, as [chapter](#ch:networking) described. `http_server_control` also has a
`listen()` that takes credentials, but it hands the same
`seastar::shared_ptr` to every shard, and a `shared_ptr` belongs to one
shard ([chapter](#ch:shared-nothing)), so Shoal listens on each shard's
`http_server` itself, through `server().invoke_on_all()`. (This is
Seastar issue <https://github.com/scylladb/seastar/issues/3841>.)

Shoal 13's tests check every operation, over HTTP and over HTTPS, with
Python's own `urllib`. Valkey has no admin API, so against Valkey these
tests are skipped.

## A data path over HTTP {index="HTTP server!data path"}

An admin API serves a few requests a second. ScyllaDB's Alternator serves
all of its clients' requests over Seastar's HTTP server, with request
bodies of up to 16 MB, and its code shows what that takes beyond routes
and handlers:

- **Two servers per shard.** Alternator is a `sharded` service whose
  instance on each shard has two `http_server`s, one for HTTP and one for
  HTTPS, with the same routes. It builds each shard's credentials on that
  shard, and listens on each shard's server itself, as Shoal does.
- **Limits before reading.** The server rejects a request whose request
  line and headers are too long, and Alternator rejects one whose
  `Content-Length` is larger than its limit before reading the body at
  all.
- **Admission control.** Before it reads a body, Alternator checks how
  many requests the shard is already handling, and sheds the request,
  with an error, if there are too many, rather than piling up memory for
  requests that it would have to queue anyway. Then it takes units of a
  semaphore of memory ([chapter](#ch:coordination)) for the request,
  twice the size of the body, since the parsed request may take as much
  again.
- **The body as a stream.** With `set_content_streaming(true)`, the
  server doesn't read the body into one string, but gives the handler a
  stream, which Alternator reads into pieces of its own, so that a large
  request never needs a large allocation.
- **Parsing in a thread.** A small body is parsed at once; a large one
  goes to a Seastar thread that the shard keeps for this, whose parser
  yields as it goes ([chapter](#ch:threads)), so that a large request
  doesn't stall its shard.

Redpanda's HTTP proxy for Kafka, and its schema registry, are built on
Seastar's HTTP server too.

## The HTTP client {index="HTTP client"}

[`http::client`]{index="`seastar::http::client`" .main}, from
`<seastar/http/client.hh>`, sends requests to one server, over a pool of
connections that it keeps open between requests, up to 100 of them
unless it's told otherwise. `make_request()` sends a request, and calls a
handler with the reply's status and headers, and its body as a stream,
when the reply arrives. A client is ended with `close()`, which closes
the connections in its pool, but doesn't wait for requests in flight: the
program must wait for those before it calls `close()`.
([Chapter](#ch:clean-shutdown)'s table of how each kind of object is
ended lists the client too.)

Requests fail, and the client can retry them. By default, it doesn't. A
client constructed with `retry_requests::yes` retries, once, a request
that failed because the connection broke, such as when the server closed
a pooled connection just as the client reused it, and only such
failures. A [`retry_strategy`]{index="`seastar::http::retry_strategy`"},
from `<seastar/http/retry_strategy.hh>`, passed to `make_request()`,
decides otherwise: its `should_retry()` gets the error and the number of
attempts so far, and returns a future, so it can wait before saying yes.
This one retries a server that answers 503 Service Unavailable, waiting
longer each time:

```{.cpp include="examples/http/retry.cc" snippet="strategy"}
```

The program's server answers the first two requests with 503:

```{.cpp include="examples/http/retry.cc" snippet="flaky"}
```

and its client gets the reply on the third attempt:

```{.cpp include="examples/http/retry.cc" snippet="client"}
```

```
busy; retrying after 10 ms
busy; retrying after 20 ms
200: ready
```

The expected status, `ok`, is what makes a 503 an error: the client
calls the handler only for a reply of the expected status, and fails the
request, with `unexpected_status_error`, for any other, which the
strategy then sees.

## HTTP/2, HTTP/3 and WebSocket {index="HTTP!versions"}

Seastar's HTTP server and client speak HTTP/1.1 only. HTTP/2 isn't
supported: Seastar's request for it,
<https://github.com/scylladb/seastar/issues/734>, has been open since
2020. Seastar's TLS can negotiate the protocol of a connection, with
ALPN, which a server needs in order to offer HTTP/2 over HTTPS, but
nothing in Seastar uses it for HTTP/2. HTTP/3 isn't supported either, nor
QUIC, the protocol over UDP on which it runs, though a pull request that
adds an experimental QUIC transport,
<https://github.com/scylladb/seastar/pull/3440>, is under review. A server that must speak
these to its clients today needs something in front of it that speaks
them, and passes the requests on over HTTP/1.1.

[WebSocket]{.index} starts as HTTP: a client sends an HTTP/1.1 request
that asks to *upgrade* the connection, and after the server's reply, 101
Switching Protocols, the two ends exchange messages over the same TCP
connection, in both directions, whenever they like, which plain HTTP,
where only the client asks, doesn't allow. Seastar's support for it is
experimental ([chapter](#ch:upgrading-seastar)), in the namespace
`seastar::experimental::websocket`. Its
[server]{index="`seastar::experimental::websocket::server`"}, from
`<seastar/websocket/server.hh>`, is a server of its own, not part of
`http_server`: it listens on its own port, reads the upgrade request
itself, and passes the connection to the handler registered for the
subprotocol that the client names, in its `Sec-WebSocket-Protocol`
header, or to the one registered with an empty name, if the client names
none. The handler reads the messages' contents from an input stream,
and writes its own to an output stream. The server has no TLS, so it
can't serve secure WebSocket (`wss://`). Seastar's `demos` directory has
a small WebSocket server and client, and its `doc/websocket.md` describes
the server.

## Exercises

1. Add `PUT /v1/keys/{key}` to Shoal's admin API, which sets the key to
   the request's body. What should it do with a body of a gigabyte?

2. Add `GET /v1/config`, which returns Shoal's options, and make it
   reject a query parameter that it doesn't know with 400 Bad Request.

3. `GET /v1/keys` still holds all the matching keys in memory before it
   writes the first. Make it write each shard's keys as soon as that shard
   has found them, so that it holds one shard's keys at a time.

4. Write a program, like `retry.cc`, whose server has a thousand keys,
   and `DELETE /keys/{key}`, and whose client deletes them all, a hundred
   requests at a time. How many connections does the client open? And
   with a pool of ten connections?

5. Shoal's admin API over HTTPS uses the credentials of its Redis port.
   Start Shoal with `--tls-auth-clients yes`, and check with `curl`, with
   and without `--cert`, whether the admin API then requires a client
   certificate too. What would it take to require one on the Redis port
   but not on the admin API?
