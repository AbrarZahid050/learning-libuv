# CppUV Architecture

## Current implementation

CppUV is currently a Windows-only asynchronous I/O library built around:

- **Loop**: owns the scheduler, timer queue, task queue, IOCP poller, completion-handler registry, and global pending-operation count.
- **Poller**: wraps the Windows I/O Completion Port APIs and normalizes completion packets.
- **TcpServer**: submits overlapped `AcceptEx` operations and dispatches accepted sockets.
- **TcpConnection**: submits overlapped `WSARecv` and `WSASend` operations, owns the write queue, applies read backpressure, and manages connection shutdown.
- **Winsock**: manages process-level Winsock initialization.

The current TCP implementation deliberately uses different lifetime strategies for server and connection objects. A connection operation retains a `shared_ptr<TcpConnection>`; a pending server accept retains a separate completion-lifetime object so its cleanup can finish after the `TcpServer` wrapper is destroyed.

These are correctness requirements, not incidental implementation details.

## Design goals

The next architecture step introduces common concepts without hiding the operating-system rules or weakening the existing lifetime guarantees.

### Loop

The loop remains the owner of:

- IOCP creation and waiting.
- Task and timer scheduling.
- Completion dispatch.
- The global count of submitted kernel operations that have not yet been fully dispatched.
- Shutdown coordination.

The loop must outlive every handle and every outstanding request associated with it.

### Handle

A handle represents a long-lived library resource associated with a loop, such as a TCP connection, TCP listener, UDP socket, or pipe.

The handle abstraction should eventually centralize common resource lifecycle concerns:

- The owning loop reference (non-owning; the loop must outlive the handle).
- Completion-dispatch registration and deregistration.
- Handle lifecycle state.
- Per-handle outstanding-operation accounting.
- The rule that a completion handler cannot be deregistered until all operations using it have drained.

A handle must not assume that every derived resource has the same ownership model. In particular, a handle may be shared-owned while an operation is pending, or it may use a separate completion-lifetime state to finish cleanup after its public wrapper is destroyed.

### Request

A request represents one asynchronous operation submitted to the operating system, for example:

- TCP accept.
- TCP read.
- TCP write.
- TCP connect.
- UDP receive or send.
- Filesystem work.

A request must own every buffer and native operation structure that the operating system may access until completion is dispatched. It must have one clear transition from allocated, to submitted, to completed (or failed before submission).

Important invariants:

1. Do not free or move an overlapped operation after successful submission until its completion packet is handled.
2. Reserve the loop's pending-operation count before submitting kernel I/O.
3. If submission fails synchronously without an I/O completion being queued, release the reservation and request resources immediately.
4. For an accepted pending operation, release the reservation exactly once when its completion has been processed.
5. Cancellation does not mean the operation's memory can be freed immediately. Wait for the cancellation completion.
6. Keep the target handle or an independent cleanup state alive for as long as the completion path needs it.

### Completion dispatch

The completion key identifies the registered handle-level dispatcher. The `OVERLAPPED*` identifies the individual request. The dispatcher should validate and route the request to the appropriate operation handler.

The request abstraction must not rely on unsafe assumptions about base-class pointer offsets or object layout. The conversion from an `OVERLAPPED*` to its owning request must be explicit and well-defined.

## Threading model

The intended initial model is:

- One thread runs `Loop::run()`.
- Application threads may submit work through thread-safe loop-posting APIs.
- Handle lifecycle methods and their callbacks are serialized on the loop thread unless an API explicitly documents otherwise.
- The IOCP poller may be extended to multiple loop threads only after synchronization and callback concurrency rules are designed and tested.

This is intentionally conservative. A single loop thread makes handle state transitions easier to reason about while the library's core abstractions are being established.

## Refactoring sequence

1. Add regression tests and explicit lifecycle rules for the existing loop.
2. Introduce the request representation and test its state transitions independently.
3. Introduce the handle lifecycle and completion-registration abstraction.
4. Migrate TCP read/write requests to the request abstraction.
5. Migrate TCP connection and listener dispatch to the handle abstraction.
6. Keep the existing TCP behavior and shutdown tests passing throughout the migration.

## Non-goals for this step

This architecture step does not yet add UDP, DNS, filesystem APIs, a thread pool, or non-Windows backends. Those features should reuse the handle/request model after it has been validated by TCP.
