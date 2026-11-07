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

## Proposed task-to-actor exchange

An actor-backed task can connect the scheduler's worker pool to a ComputeActor
on the event loop. The producer writes into a bounded local connection; the
ComputeActor is its only consumer and performs `Fetch` and network `Send`.
The same connection provides buffering and backpressure across the thread
boundary.

```mermaid
flowchart LR
    subgraph workers[Scheduler workers]
        P[Producer task]
        T[TActorTaskNode.Execute]
    end
    Q[Bounded local IConnection]
    subgraph loop[Event loop]
        A[ComputeActor]
    end
    R[Remote node]
    P --> Q
    Q -->|Fetch| A
    T -. Notify .-> A
    A -->|Send| R
    A -. Wake .-> T
```

The task checks the shared transfer state and wakes the actor through a
coalescing Notifier. It does not fetch rowsets itself. The actor drains the
connection as transport capacity permits. When it needs more input or finishes
the transfer, it publishes that state before waking the task. The next task
step can then request upstream work or report completion.

Task notifications use a bounded multiple-producer, single-consumer queue:
workers reserve slots with a producer-side compare-and-swap, while the actor
alone advances the read cursor. A slot becomes readable only after its value
has been published; an unfinished reservation is not skipped. Producers notify
after publication, so a delayed producer can wake the actor again. A full queue
rejects a push without moving its value. Element moves must not throw because
later producers may already have reserved slots beyond a failed publication.

This model requires a generic external-wakeup contract in the scheduler. A
`WAITING` result suspends a task until an external event; a thread-safe `Wake`
reschedules it without losing notifications that arrive during execution.
An empty ready queue is not completion while external work remains pending,
and wakeup handles must respect the graph's lifetime. The actor-specific
bridge belongs in `qdbd`; the scheduler only needs waiting and wakeup semantics.
This exchange is a design proposal, not an implemented execution path.
