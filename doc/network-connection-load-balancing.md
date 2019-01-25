# Motivation

In sharded systems like seastar it is important for work to be
distributed equally between all shards to achieve maximum performance
from the system. Networking subsystem has its part in distributing work
equally. For instance if on a server all connections will be served by
single shard only, the system will be working with the speed of this
one shard and all other shards will be underutilized.

# Common ways to distribute work received over network between shards

Two common ways to distribute work between shards are:
 - do the work at a shard that received it
 - shard that does actual work depends on a data been processed
   (one way to do it is to hash(data) % smp_count = shard)

# Load Balancing

Those two ways asks for different strategy to distribute connections
between shards.  The first one will work best if each cpu will have the
same amount of connections (assuming each connection gets same amount of
works) the second will work best if data will arrive to a shard where
it is going to be processed and actual connection distribution does
not matter.

Seastar's posix stack supports both of those strategies. Desired
one can be chosen by specifying load balancing algorithm in
listen_options provided to reactor::listen() call. Available options
are:

- load_balancing_algorithm::connection_distribution

  Make sure that new connection will be placed to a shard with smallest
  amount of connections of the same type.

-  load_balancing_algorithm::port

   Destination shard is chosen as a function of client's local port:
   shard = port_number % num_shards.  This allows a client to make sure that
   a connection will be processed by a specific shard by choosing its local
   port accordingly (the knowledge about amount of shards in the server is
   needed and can be negotiated by different channel).
