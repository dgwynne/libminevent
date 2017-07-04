`libminevent`
=============

This library implementas a minimal subset of `libevent`, specifically
the APIs for handling events on file descriptors, timers, and
signals. It does not include the buffer related APIs.

It also differs from `libevent` in that the evtimer and signal APIs
are not simple wrappers around the event API, they are distinct
interfaces. Code that currently uses `event_set()`, `event_add()`,
and `event_del()` to handle timeouts and signals must be converted
to use the evtimer and signal APIs instead.

Why?
----

I'm an idiot.
