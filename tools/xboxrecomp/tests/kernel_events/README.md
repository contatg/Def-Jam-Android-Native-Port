# Kernel events

KEVENTs a title builds by writing the header itself, with no
`KeInitializeEvent` call, checked through the thunk dispatcher. Like
`tests/memory_regressions`, it needs no title and no game files: it runs the
real memory layout on the small synthetic XBE from `tools/conformance`.

```
cmake -S tests/kernel_events -B build/kernel-events -A x64
cmake --build build/kernel-events --config Release
ctest --test-dir build/kernel-events -C Release --output-on-failure
```

| Test | What it checks |
|---|---|
| `kernel_events_default` | Lazy shadow objects support kernel set/reset, single waits, WaitAny and WaitAll. Direct header writes do not signal an existing shadow. |
| `kernel_events_title_kevents` | `RECOMP_TITLE_KEVENTS=1`. The table below. |

The opt-in mode additionally makes direct guest header writes authoritative:

| Check | Why it matters |
|---|---|
| An unset synchronization event times out | A zero timeout must report the state, not an error. |
| `KeSetEvent` sets it; the wait succeeds and clears `SignalState` | A synchronization wait consumes the signal, in guest memory too. |
| A second wait times out again | The same. |
| `SignalState` written to 1 by the title counts | XDK code sets and clears events by writing the header. |
| A notification event stays set across two waits | Notification events are not consumed. |
| `KeResetEvent` clears it | Host event and guest header agree. |
| WaitAny consumes only its returned index | Unselected synchronization headers remain signalled. |
| WaitAll consumes synchronization headers only on success | Timeouts preserve state; notification headers remain signalled. |
| Explicit reinitialization replaces header-event ownership | One guest address has one native shadow object. |
| Thunks consume their stdcall stack arguments | New event ordinals preserve the guest caller's stack. |

Both modes also resolve an inline semaphore on its first multiple wait and
verify that releasing two permits allows exactly two subsequent waits.

Concurrent waiter/setter races, pulse delivery to an already blocked waiter,
and real-title behavior are not established by these sequential fixtures.
