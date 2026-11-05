// Pub/Sub: clients subscribe to channels, and PUBLISH sends a message to
// every client subscribed to its channel.
#pragma once

#include "resp.hh"
#include <seastar/core/iostream.hh>
#include <seastar/core/shared_future.hh>
#include <seastar/net/api.hh>
#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// snippet: client
// A client's connection, as seen by Pub/Sub. Both the connection's own
// loop and every publisher write to it, so all their writes go through
// send() or write(), which make each write wait for the previous one.
class client {
    connected_socket& _socket;
    output_stream<char>& _out;
    // Becomes available when everything sent so far has been written.
    shared_future<> _last_write = make_ready_future<>();
    // The memory used by what was sent but not written yet: this client's,
    // and all clients' together.
    size_t _pending = 0;
    size_t& _total_pending;
    std::unordered_set<sstring> _channels;
public:
    client(connected_socket& socket, output_stream<char>& out,
           size_t& total_pending)
        : _socket(socket), _out(out), _total_pending(total_pending) {}
    // Sends data after everything sent before it, without waiting.
    void send(sstring data);
    // Sends data, and waits until it's written.
    future<> write(sstring data);
    // Waits until everything sent so far is written.
    future<> drain() { return _last_write.get_future(); }
    // Makes the client's pending and future writes fail, and its reads
    // end, so that its connection ends.
    void disconnect();
    size_t pending() const { return _pending; }
    std::unordered_set<sstring>& channels() { return _channels; }
private:
    future<> write_after(future<> previous, sstring data, size_t cost);
};
// end snippet: client

// snippet: pubsub
// How PUBLISH delivers a message to the subscribers.
enum class delivery { sequential, parallel, background };

// The channels, and the clients subscribed to each.
class pubsub {
    std::unordered_map<sstring, std::unordered_set<client*>> _channels;
    delivery _delivery;
    // Limits on the memory used by messages waiting to be written: to one
    // subscriber, and to all of them together.
    size_t _max_pending;
    size_t _max_total_pending;
    size_t _total_pending = 0;
public:
    pubsub(delivery d, size_t max_pending, size_t max_total_pending)
        : _delivery(d), _max_pending(max_pending)
        , _max_total_pending(max_total_pending) {}
    size_t& total_pending() { return _total_pending; }
    // Executes a Pub/Sub command, or any command of a subscribed client.
    future<> execute(client& c, const request& req);
    // Unsubscribes a client that is going away from all its channels.
    void forget(client& c);
private:
    future<> subscribe(client& c, const request& req);
    future<> unsubscribe(client& c, const request& req);
    future<> publish(client& c, const request& req);
    void remove(client& c, const sstring& channel);
};
// end snippet: pubsub

// Is this a command that pubsub::execute() handles?
bool is_pubsub_command(const request& req);
