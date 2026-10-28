# Session integration trial: synthetic only

This is an in-memory prototype of the proposed contract, not an integration
with SpartanPort, Windows process control, Computer Use or an IPC transport.
It uses Python's standard dataclasses, enums, UUID generation and lock only.
No process is started, inspected or closed; no file command, pipe, socket,
window message, input event, game file or savedata operation is implemented.

The current Windows attestation blocker is deliberately represented by
`WindowsUnavailable`: enabling this adapter records
`windows-child-attestation-unavailable` and leaves the session stopped. It does
not call OpenProcess, retry the old error, ask for elevation or fall back to
another identity source. No new live control channel has been added.

## What was prototyped

`experiments/gow3_automation/integration_trial.py` contains a disabled-by-default
model. A launch intent is created before the synthetic adapter's binding and
contains a unique collection/session label, expected image digest and isolated
root label. These are intent data, not proof of a real launch, safe filesystem
path or completed backup. The synthetic adapter exists only in the test file.

The test adapter returns an opaque in-memory owned binding. Every operation
checks that exact object through its adapter; copied PID/birth/image metadata
cannot substitute for it. This models a capability contract but provides no
Windows authenticity. No existing framework API that can supply the equivalent
live binding at unchanged rights has been established.

The command vocabulary deliberately has no Save, Delete or New Game action:

| Command | Synthetic precondition and consumption |
| --- | --- |
| Navigate Load | Fresh focused title observation with New Game selected; requires a later Load-selected observation. |
| Open Load | Fresh title observation explicitly selecting Load; requires the load-dialog transition. |
| Load Existing | Dialog type must be load; exact existing-slot label, selected item and dialog generation must match again at consumption. |
| Move | Confirmed load completion associated with the consumed load command; 1..250 ms, bounded axes, fixed deadline on consumer clock. No health/life/death field is required. |
| Close Owned | Produces a request carrying the exact owned binding, never a process name or arbitrary PID. Remains CLOSING until a matching exit observation arrives. |

Submit only queues a command. The bounded acknowledgement queue records
`consumed` when the input consumer samples it, not when the supervisor offers
it. A movement expiration records `neutral-at-consumer` once. The initial queue
capacity is 16, permitted range 1..64; overflow counts a lost acknowledgement,
clears pending/active input and latches stop. It never silently overwrites.
Queued commands expire after 250 ms; observations after 500 ms. The synthetic
run has a 30-second limit. A close without its exit observation after one second
is reported unconfirmed; there is no force-close fallback.

Session, command sequence and dialog generation are explicit. A single lock
orders observations, submissions, sampling and acknowledgement draining. This
tests the Python contract, not the renderer's real thread ownership. Clock
regression, stale/foreign observations, focus loss, loss counters, revoked
binding, state changes between submit/consume and adapter errors stop the model.
The disabled path returns bypass without contacting the adapter.

## Tests actually executed

Run only the synthetic test module:

```
python -B -m unittest discover -s experiments/gow3_automation -p test_integration_trial.py -v
```

26 tests passed in 0.013 seconds with the existing Python interpreter. Evidence:
`out/integration-trial-20261027T205832736Z/tests.log` and `result.json`.

They cover the full synthetic title/load/loading/playing/close route; missing
Windows attestation without retry; disabled adapter inactivity; unique session
intent; mismatched launch binding; typed load rejecting save/delete/new/wrong
slot or generation; dialog changes before consumption; incorrect load cause;
duplicate/foreign commands; invalid axes/durations; all 250 permitted durations;
delayed consumer neutralization; stale/future/out-of-order observations; lost
focus/events; invalid clocks and run deadline; copied identity metadata;
revocation/adapter failure; bounded acknowledgement loss; no effect when a
consumption acknowledgement cannot be retained; wrong exit identity; unconfirmed
close; and eight concurrent consumers producing only one consumption ack.

No life/death detector is included. Movement remains an input-consumption claim,
not proof that the character or camera actually moved, or that enemies did not
affect the scene. An exit acknowledgement is fabricated by the test fixture;
it is not evidence of a Windows process shutdown.

## Boundaries still unresolved

1. The framework must supply a supported live ownership/identity interface at
   unchanged rights. Until then WindowsUnavailable remains terminal. UUIDs,
   filenames, self-reported logs and visual window identity do not supply that
   guarantee to a separate native script.
2. A real load-only adapter would have to use the copied native dialog state,
   propagate its generation and distinguish loading completion. The existing
   SDL choice handler has not been modified or exposed as a control endpoint.
3. The real input consumer must own expiry and neutralization. The model only
   guarantees its return value on a subsequent sample; it cannot establish a
   wall-clock release while a real game thread is stalled.
4. Real owned-instance closure and its acknowledgement need a supported adapter.
   The model does not replace the existing PID/birth/image check or claim that
   a timeout proves exit.
5. Filesystem isolation and verified save backup remain prerequisites for any
   later game trial. They were intentionally not accessed or simulated as facts.

`scripts/gow3_native_menu_step.py` remains unchanged, SHA-256
`D44C781FE4D5FF33F643E891FC5C150C82031EB4ED803ED993F29BD85DD8A1E5`.
No runtime source, launch script or security configuration was changed. There
was no OpenProcess retry, live command, game launch, save access, commit or push.
This prototype validates contract logic only; it does not unblock autonomous
gameplay or validate reprojection/DLAA.
