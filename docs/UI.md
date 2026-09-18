# Terminal UI Component Library

`clanker_ui` is an internal, reusable C++23 component library layered over the
cell renderer in `clanker_term`. It has no agent, JSON, conversation, provider,
or application dependencies. The Clanker-specific full-screen application lives
in `clanker_tui`.

## Layers

```text
clanker_term  terminal modes, input decoding, cells, screen diffing
clanker_ui    frame tree, layout, canvas, events, focus, reusable widgets
clanker_tui   transcript, command palette, modals, agent/application state
```

Application code paints through `UiCanvas`; only `clanker_ui` writes to `Screen`.
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

### Modal Panel

A pending modal (`Tui_BeginSelection`, `Tui_BeginTextEntry`, and the permission
prompts that build on them) *replaces* the composer rather than floating over
it. `ComposeAtSize` asks `ModalPanelHeight` for the row count, omits the
composer border and `Ui_TextInput` entirely, and anchors the panel at the rows
the composer would have used: full terminal width, bottom border on the row
above the status bar.

`ModalPanelHeight` derives the desired height from the content -- one row per
matching option for a selection, a fixed three-row answer box for text entry,
plus borders and title -- then clamps it to half the terminal, itself bounded to
`[4, 16]` rows, and to whatever is left after reserving the status row, the
context line, and one transcript row. It returns `0` when that leaves fewer
than four rows, which keeps the composer in place on terminals too short to
paint a panel at all.

Focus needs no special handling when the composer node disappears: `Ui_Dispatch`
falls back to the first focusable node, and `Ui_Paint` hides the cursor unless a
painted component requests it.

## Stream Reveal

Providers deliver assistant text at wildly different cadences. Some endpoints
coalesce deltas into clumps of tens of characters every ~80-100 ms, so
revealing each burst the instant it arrives makes the transcript stair-step,
while token-level deltas already look smooth. `StreamReveal`
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
animation. The pacing arithmetic itself costs ~0.02 us per frame.

Queued display events own arena-backed FIFO batches, not session storage. A
batch closes after 32 events or once its allocations reach 256 KiB; a single
large event or coalesced text buffer can exceed that threshold. Completed
batches are reclaimed even while newer events remain queued. At most one empty
batch keeps 64 KiB committed for reuse. Text adopted by `StreamReveal` pins its
batch until the final revealed slice has been copied into the line model;
dequeue alone does not release the text. Direct reveal storage is also recycled
and trimmed to 64 KiB after draining. Reveal timing and UTF-8 handling do not
depend on batch boundaries.

Tool-progress slots share a reclaimable arena for IDs and replace-in-place
previews. Queued updates identify the slot rather than borrowing its strings,
so idle slots can be discarded and live slots compacted without waiting for
every tool to finish. Status strings have independently resettable owners.
Hydration borrows single text blocks or joins multiple blocks in per-message
scratch; the line model copies only what it retains. Tool outcomes are computed
from the full result before keeping the bounded preview. These ownership rules
reclaim obsolete copies, not genuine pending output or retained transcript
content.

## Transcript Layout and Ownership

Line-model mutations retain content and invalidate only the changed entry.
They do not wrap, parse markdown, or copy the transcript on arrival.
`PumpDisplay` materializes layout once after draining the events/reveal for a
render turn, under the same TUI lock. Hydration likewise lays out once after
loading its messages. Display queries also materialize outstanding changes,
so even const row/revision queries require that lock; they are not independent
read-only operations.

Each entry caches its wrapped rows, including owned span text. Layout formats
only dirty entries and tools whose preview visibility changed. Progress on an
unfinished tool does not invalidate rows, since previews appear on completion.
Mutations directly maintain the earliest dirty entry index: layout does not
search history for dirty entries. Expanded-preview costs and their admission
boundary are cached. That boundary is reconsidered only when expansion
membership, an expanded preview, or width changes, not on assistant appends.

The flat display-row index is a ring. Cached uncropped entry coordinates let
layout replace metadata from the earliest dirty entry onward, leaving prefix
rows in their existing slots. Front cropping advances the ring head without
copying history. If markdown shrinks enough to expose formerly cropped prefix
rows, a binary coordinate lookup restores only those newly visible rows.
Entry-ID/row-range lookup is likewise binary, rather than scanning the flat
transcript to restore a scroll anchor.

Normal append-to-last-assistant layout is independent of retained history:
its work is the active assistant's formatting, visible-prefix fingerprinting,
and changed visible row writes. A changed middle entry updates the following
entry coordinates and visible row metadata, proportional to that suffix.
Actual eviction is linear in retained entries, removes the prefix with one
move, and does not re-render survivors. Capped hydration starts from the newest
entries and does not format the discarded prefix. Separators belong to index
assembly, not entry caches, so dropping an entry does not invalidate its neighbor.

Row caches share an arena rather than reserving/committing an arena per entry.
The latest allocation is replaced in place when possible (notably during
streaming). These are shared, lazily committed arenas, not a 64 KiB allocation
per entry. When total row storage exceeds 64 KiB and dead storage exceeds the
live set, live cached rows are copied into a fresh arena without re-running
layout, and visible row pointers are refreshed. This amortized reclamation and
geometric growth of the flat ring may copy retained rows; ordinary tail layout
does neither. Markdown scratch is reset between entries and trimmed after
streaming ends. Cached text does not borrow mutable tool buffers, reveal
batches, or markdown scratch.

Multiple assistant deltas before layout produce one render of that assistant,
not one per token. The active markdown entry can still reflow as a whole at
that boundary: closing emphasis, setext headings, tables, and fences can change
earlier rows. This is intentionally distinct from re-rendering retained
history. Prefix fingerprints and cropped-row offsets preserve append-stable
selection revisions; semantic reflows and front cropping invalidate them.
Width changes deliberately invalidate all retained entry layouts, as do
bullet-style changes; expand-all may likewise update the whole retained suffix.
A trial-width scrollbar probe leaves caches untouched
and stops once it exceeds the viewport, rejecting histories with too many
visible entries without formatting them.

Session compaction uses `LineEdit_Clone` to deep-own the complete editor state
in the replacement arena: text, cursor, selection anchor, sticky column,
history position, recall history, and the saved live draft. Growable buffer
backing pointers refer to the final arena handle, not a stack-local copy.
Composer scrolling/drag state survives compaction too. `/clear` intentionally
resets the editor and history. Both arena swaps retain every queued submission
field, including permission mode, while copying its text and preserving FIFO
order across a wrapped queue.

## Markdown

`Markdown_Render` (`src/tui/markdown.{h,cpp}`) turns an assistant message into
logical span-lines for the transcript. It is modelled on pi's
`packages/tui/src/components/markdown.ts`, which lexes with `marked` and
hand-writes the walk to ANSI; Clanker keeps the same visual vocabulary but scans
lines directly instead of materialising a token tree. That choice is a
consequence of streaming: a dirty assistant is rendered at the layout boundary,
so allocation has to stay proportional to output rather
than to a parse forest. Only blockquotes, list items and table cells recurse.

Supported: ATX and setext headings, fenced (``` and `~~~`) and indented code,
bullet/ordered/task lists with nesting, blockquotes, horizontal rules, GFM
tables, HTML passthrough, and paragraphs; inline, the CommonMark set of
backslash escapes, code spans, emphasis, strikethrough, links, images and
autolinks.

Visual rules follow pi rather than a generic renderer: h1 is bold and
underlined with no `#`, h2 is bold with no `#`, h3-h6 keep a styled `### `
prefix; every bullet normalises to `- `; a code fence prints as three
backticks plus the info string with the body indented two columns; a rule is
`─` repeated `min(width, 80)`. A nested list sits on an absolute four-column
grid, so a child's marker starts at column 4 regardless of how wide the
parent's marker was.

A soft line break stays a line break. pi's `applyTextWithNewlines` splits on
`\n` and rejoins on `\n`, so a single newline inside a paragraph is preserved,
and Clanker matches that: agent replies use single newlines deliberately, and
joining them would reflow text the model laid out. The inline pass still runs
over the whole paragraph before the spans are cut, so emphasis may span a
break.

Colours come from a named `MarkdownPalette` rather than literals at use sites.
`Style` carries a 256-colour palette index, so the entries are the nearest
xterm cube/ramp approximations of pi's dark theme. `Link` deliberately takes
110 rather than its true nearest 109, because `mdCode` also rounds to 109 and
links would otherwise be indistinguishable from inline code.

Two properties are load-bearing:

- **Width is a parameter, never module state.** Rules and tables are sized
  against it, and `LineModel_FitsAtWidth` re-renders at a trial width. Width
  changes therefore invalidate every retained entry through
  `LineModel_SetWidth`, reflowing tables at the next layout boundary.
- **Rows own their wrap prefix.** Each row carries its own list marker
  indent, quote bar or code indent in `SpanLine::Continuation`, and
  `BuildAssistant` *concatenates* the assistant icon indent onto it instead of
  overwriting it. Without this a wrapped list item snaps back to the margin and
  a wrapped quote loses its bar. `Continuation` stays a single `Span` only
  because the markdown palette sets no background colour, which makes the
  leading indent invisible and lets `indent + "│ "` be one quote-styled span;
  adding a background would break that.

Streaming needs no incremental parser, matching pi. The one concession is
`TrimPartialClosingFence`, a port of pi's `trimPartialClosingFences`: while a
closing fence is being typed, a trailing line of one or two backticks is
dropped so the block does not visibly shrink and regrow. Blank rows are emitted
between blocks but never before the first or after the last, so a reply does
not jump as a trailing blank appears and disappears.

Deliberate divergences from pi:

- **No OSC 8 hyperlinks.** `Screen` stores one scalar per cell and re-emits it
  verbatim, so an escape sequence cannot live in the grid. Links always use
  pi's non-hyperlink fallback, `text` styled plus ` (url)` dim.
- **No syntax highlighting**, which is also pi's behaviour for an unknown
  language tag, and **no LaTeX**.
- **No GFM column alignment** (`:---`), which pi does not implement either.
- A list marker wider than four columns (`- [x] `, `100. `) keeps its nested
  list under the parent's content rather than pulling it back to the grid,
  which would place the child to the left of the text it belongs to.

Rendering reaches assistant entries only. User echo, info, error and tool rows
stay plain text.

## Command Submission

Slash commands are pushed onto a 64-slot queue and executed by the main loop,
which owns the runtime state they mutate. For the length of a turn that loop is
inside the agent run, so a queued command does not execute until the turn ends;
the composer reports it as `Queued command: ...` rather than appearing to do
nothing. Ordinary prompt text does not queue -- it steers the live run directly
from the input thread.

Three commands would be useless under that rule, so `TuiCommandSpec::Inline`
lets the catalog mark what the input thread may do immediately:

- `/debug` toggles the overlay in place. It touches only `Perf.Visible`, so
  nothing in a running turn can observe it and it never reaches the queue.
- `/exit` cancels the running turn and stops, matching Ctrl+D rather than
  waiting for a turn it was meant to interrupt.
- `/clear` cancels the running turn but still queues. The reset swaps the
  session log, which cannot happen underneath a turn that is appending to it,
  so only the cancellation is immediate; the loop performs the reset once the
  turn unwinds.

Argument forms the dispatcher would reject resolve to no inline action and are
queued unchanged, so usage errors keep coming from the one place that owns the
argument policy.

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
