# Motivation

In sharded systems like Seastar, it is important for work to be
distributed equally between all shards to achieve maximum performance
from the system. Networking subsystem has its part in distributing work
equally. For instance, if all connections on a server are served by a
single shard, the system will run at the speed of that
one shard and all other shards will be underutilized.

# Common ways to distribute work received over the network between shards

Two common ways to distribute work between shards are:
 - do the work at a shard that received it
 - choose the shard that does the actual work based on the data being processed
   (one way to do it is to hash(data) % smp_count = shard,
    another way is to bind shards to different server addresses)

# Load Balancing

These two approaches require different strategies for distributing connections
between shards. The first works best if each CPU has the
same number of connections (assuming each connection gets the same amount of
work). The second works best if data arrives at the shard where
it is going to be processed and actual connection distribution does
not matter.

Seastar's POSIX stack supports both strategies. Choose one by specifying the load-balancing algorithm in
the `listen_options` provided to the `reactor::listen()` call. Available options
are:

- load_balancing_algorithm::connection_distribution

  Ensures that a new connection is placed on the shard with the fewest
  connections of the same type.

-  load_balancing_algorithm::port

   Destination shard is chosen as a function of client's local port:
   shard = port_number % num_shards.  This allows a client to make sure that
   a connection will be processed by a specific shard by choosing its local
   port accordingly (the knowledge about amount of shards in the server is
   needed and can be negotiated by different channel).

- load_balancing_algorithm::fixed

  Destination shard is statically configured in listen_options::fixed_cpu. This
  allows a client to make sure that a connection to a server address will be
  established in a specific shard, without any further negotiations.
