// Pub/Sub: clients subscribe to channels, and PUBLISH sends a message to
// every client subscribed to its channel.
#pragma once

#include "resp.hh"
#include <seastar/core/condition-variable.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/queue.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/sharded.hh>
#include <seastar/net/api.hh>
#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// snippet: client
// A client's connection, as seen by Pub/Sub. Both the connection's own
// loop and every publisher write to it, so all their writes go through a
// queue, from which one fiber writes them, one at a time and in order.
class client {
    // What was sent, but not written yet. A message holds units of the
    // semaphore that limits the memory of all the waiting messages, and
    // gives them back when it's destroyed, written or not.
    struct message {
        sstring data;
        semaphore_units<> units;
    };
    connected_socket& _socket;
    output_stream<char>& _out;
    queue<message> _queue;
    size_t _pending = 0;            // bytes sent but not written yet
    condition_variable _written;    // signaled when _pending becomes 0
    std::exception_ptr _failure;    // why writing failed, if it did
    future<> _writer;               // the fiber that writes the queue
    std::unordered_set<sstring> _channels;
public:
    client(connected_socket& socket, output_stream<char>& out);
    // Sends data after everything sent before it, without waiting.
    void send(sstring data, semaphore_units<> units = {});
    // Sends data, and waits until it's written.
    future<> write(sstring data);
    // Writes a reply to the client's own command. A client that isn't
    // subscribed has no other writers, so it writes directly.
    future<> reply(sstring data);
    // Waits until everything sent so far is written.
    future<> drain();
    // Waits for everything sent to be written, and stops the writer.
    future<> close();
    // Drops the unwritten messages, and makes the client's reads end, so
    // that its connection ends.
    void disconnect();
    size_t pending() const { return _pending; }
    std::unordered_set<sstring>& channels() { return _channels; }
private:
    future<> write_messages();
};
// end snippet: client

// snippet: pubsub
// The channels, and the clients of this shard subscribed to each. Every
// shard has an instance.
class pubsub : public peering_sharded_service<pubsub> {
    std::unordered_map<sstring, std::unordered_set<client*>> _channels;
    // A subscriber whose unwritten messages use more than this is
    // disconnected, as Redis does.
    size_t _max_pending;
    // The memory of all the subscribers' unwritten messages together: a
    // publisher waits for units before it sends.
    semaphore _memory;
    size_t _memory_limit;
public:
    pubsub(size_t max_pending, size_t memory_limit)
        : _max_pending(max_pending), _memory(memory_limit)
        , _memory_limit(memory_limit) {}
    // Executes a Pub/Sub command, or any command of a subscribed client.
    future<> execute(client& c, const request& req);
    // Unsubscribes a client that is going away from all its channels.
    void forget(client& c);
private:
    future<> subscribe(client& c, const request& req);
    future<> unsubscribe(client& c, const request& req);
    future<> publish(client& c, const request& req);
    // Sends a message to this shard's subscribers of the channel, and
    // returns how many it sent it to.
    future<size_t> publish_here(const sstring& channel, const sstring& data);
    void remove(client& c, const sstring& channel);
};
// end snippet: pubsub

// Is this a command that pubsub::execute() handles?
bool is_pubsub_command(const request& req);
