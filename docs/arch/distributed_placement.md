# Distributed placement

Node selection and compute actor creation have separate owners. The Scheduler
keeps node heartbeats and a round-robin cursor. It returns distinct local
scheduler IDs to the Query and retains no state for that request. Selection is
advisory: a selected node may fail before actor creation.

After receiving the placement, the Query creates a Collector. The Collector
splits the requested actor count evenly across the selected local schedulers
and requests creation directly. Local schedulers allow oversubscription; the
advertised worker count is not an admission limit.

The Collector gathers replies until all selected local schedulers respond, an
error occurs, or the allocation times out. It sends one result to the Query and
terminates. On failure it stops the compute actors whose IDs it has received;
it does not wait for late replies. Duplicate replies do not count toward
completion.

The Query-to-compute-actor lifecycle protocol, distributed query execution, and
graph partitioning are outside this preparatory change. Compute actor cleanup
when a Query disappears belongs to that future lifecycle protocol.
