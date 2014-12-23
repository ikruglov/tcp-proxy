tcp-proxy
=========

experimental zero-copy tcp-proxy

usage:
$ make
$ ./bin/tcp-proxy <local ip:port> <upstream ip:port>

ex:
$ ./bin/tcp-proxy localhost:8080 localhost:8000

Some implementations hints:
- by default start `nproc` threads each running independent event loop (libev)
- each eventloop accepting connection (socket created with SO_REUSEPORT)
- accepting a connection is malloc-free (occasional reallocations are possible)
- communication happens between downstream <-> upstream by means of splice()

further possible improvement/tunings:
- backoff strategy when when reading from a socket
- pthread CPU/memory affinity
- IRQ and interface's queue processing affinity
- add support of SO_LINGER which can be useful to reduce amount of
  sockets in TAIM_WAIT state after disconnecting from upstream

