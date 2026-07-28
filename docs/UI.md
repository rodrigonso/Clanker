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

## Diagnostics

`Ui_GetMetrics` reports frame node count, hit-region count, focusable count, and
frame-arena bytes. `Ui_SetDebugBounds` enables an opt-in cell overlay for
inspecting assigned component rectangles.

The `ui`, `ui_widgets`, and `ui_demo` test suites exercise the tree, layout,
events, widgets, and a standalone screen that does not link application types.

## Scope

The library is currently internal. Public ABI stability, external packaging,
full CSS/Yoga behavior, React compatibility, and accessibility semantics are
not provided.

