# Terminal UI Component Library

`clank_ui` is an internal, reusable C++17 component library layered over the
cell renderer in `clank_term`. It has no agent, JSON, conversation, provider,
or application dependencies. The Clank-specific full-screen application lives
in `clank_tui`.

## Layers

```text
clank_term  terminal modes, input decoding, cells, screen diffing
clank_ui    frame tree, layout, canvas, events, focus, reusable widgets
clank_tui   transcript, command palette, modals, agent/application state
```

Application code paints through `UiCanvas`; only `clank_ui` writes to `Screen`.
`Screen_Flush` remains responsible for suppressing unchanged terminal output.

## Keyboard Protocols

`Term_Enter` requests two enhanced keyboard protocols alongside the alternate
screen, bracketed paste, and SGR mouse tracking: the kitty keyboard protocol
flag 1 (`CSI > 1 u`, popped with `CSI < 1 u`) and xterm `modifyOtherKeys` level
2 (`CSI > 4 ; 2 m`, reset with `CSI > 4 ; 0 m`). Without them the legacy byte
stream sends a bare CR for both Enter and Shift+Enter, so modified keys are
unreportable. Terminals that implement neither ignore the requests and keep
sending the legacy encoding.

Both protocols also re-encode keys that would otherwise be plain bytes —
Escape becomes `CSI 27 u`, Ctrl+C becomes `CSI 99 ; 5 u`, Alt+b becomes
`CSI 98 ; 3 u`. `KeyDecoder` therefore routes the legacy bytes and both
enhanced forms through one shared mapping so bindings cannot drift apart, and
normalizes everything back to the same `KeyKind` vocabulary. The setup and
teardown strings live in `Term_SetupSequence` / `Term_TeardownSequence` so that
every mode set on entry is reset exactly once on exit, including via the
`atexit` restore path.

## Frame Lifetime

1. Call `Ui_BeginFrame` with the target `Screen`.
2. Build a tree with stable keys and container/widget calls.
3. Call `Ui_EndFrame`.
4. Call `Ui_Render` to measure, lay out, collect interaction regions, and paint.
5. Flush the `Screen` through the existing terminal output path.

The tree and widget payloads live in a resettable frame arena. They remain
valid until the next `Ui_BeginFrame`. Persistent domain state must be owned by
the caller and passed by pointer, such as `LineEdit`, `UiScrollState`,
`UiListState`, or an application model.

## Stable IDs

Every node requires an integer or string key. Its `UiId` is derived from the
parent ID and key. Dynamic collections must use stable domain keys rather than
their current array positions.

Use `Ui_PushId` and `Ui_PopId` when multiple logical instances share the same
component implementation. Debug builds trap duplicate sibling IDs and
unbalanced container or ID scopes.

## Layout

The supported layout model is intentionally smaller than CSS Flexbox:

- row, column, stack, and scroll-view containers
- content, fixed-cell, and weighted-fill lengths
- padding, gaps, min/max constraints, alignment, and justification
- local offsets for overlays
- deterministic remainder distribution
- nested clipping

Layout is measured bottom-up and assigned top-down. Components must not depend
on terminal output during measurement.

## Events and Focus

Input is normalized into `UiEvent` values. Pointer events hit-test the last
completed frame in reverse paint order and bubble through parents. Pointer
capture keeps drag operations routed to the initiating component.

Focusable nodes participate in Tab and Shift-Tab traversal. Focus scopes keep
modal input isolated and restore the prior focus when an overlay disappears.
Application-wide shutdown and agent-control shortcuts remain controller-owned.

Callbacks run while the TUI lock is held. They may mutate state or queue work,
but must not block, perform network I/O, or invoke rendering recursively.

## Widgets

`ui/widgets.h` provides:

- row, column, and stack builders
- text and styled text
- borders and filled panels
- spinner
- selectable list
- scroll view and scrollbar
- multiline text input backed by `LineEdit`
- overlay and modal-panel composition

Application-specific rendering can use a custom `UiNodeOps` implementation.
Custom components receive assigned geometry and a clipped, local-coordinate
`UiCanvas`; they must not access `Screen` directly.

`UiContext::BorderCorners` controls every border drawn by
`UiCanvas_DrawBorder`. `Tui_UpdateRounded` updates it together with
`LineModel::Rounded`, rebuilding retained transcript rows so existing bullets
change in place.

## Stream Reveal

Providers deliver assistant text at wildly different cadences. Anthropic
coalesces deltas into clumps of tens of characters every ~80-100 ms, so
revealing each burst the instant it arrives makes the transcript stair-step,
while OpenAI's token-level deltas already look smooth. `StreamReveal`
(`src/tui/stream_reveal.{h,cpp}`) decouples *arrival* from *reveal* so both
read the same way.

`StreamReveal_Push` appends arriving bytes to a pending buffer.
`StreamReveal_NextFrame` hands the compositor however much of that buffer the
current frame has earned:

```
rate = min(BASE_SCALARS_PER_SECOND + backlogBytes * BACKLOG_GAIN_PER_BYTE,
           MAX_SCALARS_PER_SECOND)
```

The proportional term is what makes this safe. Text arriving at R scalars per
second settles at `(R - 180) / 3` bytes behind, so the backlog is bounded by
the arrival rate rather than by the length of the reply: a 600 cps model runs
about 140 bytes behind whether the message is 200 bytes or 200 KB. The gain
term measures the backlog in bytes rather than decoded scalars to keep it
O(1); multibyte text therefore overestimates the backlog, which only reveals
faster.

A single budget, carried between frames in milli-scalar fixed point, accrues at
that rate and is capped at one full step's worth. The cap is what stops idle
time from banking credit that would later burst, and it bounds any one frame at
`MAX_STEP_MS x rate`. Elapsed time is likewise clamped to `MAX_STEP_MS`, so a
stalled render thread cannot claim the whole gap.

Note that *time* is clamped, not scalars per frame. A per-frame scalar cap
silently becomes a hard ceiling of `cap x frame rate` that no amount of backlog
can overcome, which is how an earlier pacing attempt (removed in `c95cd8e`)
throttled long replies to 240 chars/sec. A frame here may emit up to
`50 ms x 960 cps` at once. The frame always stops on a UTF-8 scalar boundary,
so a budget edge never splits a character.

(jcode runs a second budget pool at the flat `MAX_SCALARS_PER_SECOND` and emits
the smaller of the two. With the cap expressed as `MAX_STEP_MS x rate` that pool
is redundant: since `rate <= MAX_SCALARS_PER_SECOND`, the flat pool is provably
never the smaller one, so it was dropped rather than carried as dead state.)

`StreamReveal_NextFrame` takes a `blocked` flag that `PumpDisplay` sets whenever
anything is queued behind the reveal — a tool card, message end, the next user
turn — because those events cannot be applied until the text drains. Blocking
arms a completion deadline: everything pending is revealed no later than
`kRevealDeadlineMs` (120 ms) out, and once that passes a frame drains
unconditionally. This bounds the wait on any boundary at a constant instead of
letting it scale with reply length, while still animating short replies rather
than popping them. Arming is idempotent, so staying blocked cannot push the tail
out and neither can a late delta. The same flag lets malformed trailing bytes
through, since nothing more is coming to complete them.

Deadline arming lives inside `NextFrame` rather than in a separate `Finish` call
precisely because both are driven by the same condition; splitting them would
let a caller get one without the other.

Pacing is display-only and never gates the agent thread.
`Tui_AppendAssistantText` takes the lock, queues, wakes the render thread and
returns; it never waits on the reveal. Time-to-first-token, turn wall time and
tool-start timestamps are all stamped at arrival, so none of them observes the
animation. The controller itself costs ~0.02 us per frame.

The real cost of pacing is that it raises the transcript append count, and
`LineModel_AppendAssistant` re-renders the whole assistant entry on every
append — roughly 70 us at 2 KB, 240 us at 8 KB and 1 ms at 32 KB. This is not
new work: at one append per frame the count matches what token-level providers
already produced before pacing existed. It is the one place where cost scales
with both message length and frame rate, so it is the first thing to measure if
streaming ever feels heavy.

## Diagnostics

`Ui_GetMetrics` reports frame node count, hit-region count, and frame-arena
bytes.

The `/debug` slash command toggles a performance overlay: a bordered panel in
the top-right corner, inset one column so it clears the transcript scrollbar. It
reports frame rate, mean and worst frame interval over the last 64 frames, and
the last turn's time-to-first-token, wall time, token usage, tool duration, and
current process resident memory (RSS).

Three properties are deliberate:

- The panel is a leaf node with only a `Paint` op and no `UI_NODE_FOCUSABLE`
  flag, so it cannot take focus, be hit-tested, or enter Tab traversal. No
  input path needs to know it exists.
- It is built after the status row but before the command palette and modal, so
  those still paint over it.
- Timing samples run whether or not the panel is visible, which is why the
  window is already full the moment it is switched on. RSS is queried on reveal
  and at most once per second while visible.

The frame numbers describe the frame before the one being composed; RSS is
sampled during compose. The metrics include the panel's own cost: one extra node
and a fill 34 columns wide, so a frame reads slightly slower with the overlay on
than with it off. The panel is suppressed entirely, rather than clipped, on a
terminal too small to hold it, and no RSS query is made in that state.

The `ui`, `ui_widgets`, and `ui_demo` test suites exercise the tree, layout,
events, widgets, and a standalone screen that does not link application types.

## Scope

The library is currently internal. Public ABI stability, external packaging,
full CSS/Yoga behavior, React compatibility, and accessibility semantics are
not provided.
