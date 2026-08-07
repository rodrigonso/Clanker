#pragma once

#include "term/cell.h"
#include "tui/tui.h"
#include "ui/ui.h"

// Shared internals of the TUI controller, split across tui/*.cpp.
//
// Nothing here is part of the TUI's public surface -- callers use tui.h. These
// declarations exist only so one controller can live in several files instead
// of one 2,600-line one. Every function assumes the caller already holds
// `Tui::Lock` unless its name says otherwise.
//
// A named namespace rather than the global one: several of these ("Put",
// "Truncate", "Wake") are names a static library has no business exporting.
namespace TuiInternal {

extern const char* kSpinner[10];
constexpr u64 kDoubleEscapeMs = 500;
constexpr u64 kSpinnerFrameMs = 60;
constexpr u64 kCopyNoticeMs = 1500;

const u8 SELECTION_BG = 238;
const u8 COPY_NOTICE_FG = 114;

// ---- shared helpers ------------------------------------------------------

Style TuiSt(const Tui* t, u8 fg, u8 attr);
void Wake(Tui* t);
void WakeInput(Tui* t);

// Bounded-buffer append helpers used to build the status row without
// allocating; each stops at `cap - 1` bytes and keeps the buffer terminated.
void Put(char* buf, u32 cap, u32* len, const char* s);
void PutView(char* buf, u32 cap, u32* len, StrView s);
u32 PutViewCells(char* buf, u32 cap, u32* len, StrView s, u32 maxCells);
void PutNum(char* buf, u32 cap, u32* len, u32 v);
void PutScaled(char* buf, u32 cap, u32* len, u32 v);

StrView Truncate(StrView s, u32 maxBytes);
u32 Utf8Count(StrView s);

// ---- status row ----------------------------------------------------------

void FormatStatus(Tui* t, char* left, u32 leftCap, u32* ll, char* right,
                  u32 rightCap, u32* rl, u16 cols);
void BuildStatusComponents(Tui* t, u16 statusY, u16 cols);
void BuildCwdComponent(Tui* t, u16 cwdY, u16 cols);
UiLayoutStyle Positioned(u16 x, u16 y, u16 width, u16 height);

// ---- copy notice ---------------------------------------------------------

void ClearCopyNotice(Tui* t);
void ArmCopyNotice(Tui* t, StrView text);
void AdvanceCopyNotice(Tui* t, u64 nowMs);

// ---- display pipeline ----------------------------------------------------

TuiDisplayEvent* QueueDisplayEvent(Tui* t, TuiDisplayEventKind kind);
void SetDisplayPayload(Tui* t, TuiDisplayEvent* ev, StrView text);
void QueueText(Tui* t, StrView text);
void ResetToolUpdates(Tui* t);
TuiToolUpdateSlot* FindToolUpdate(Tui* t, StrView toolCallId);
TuiToolUpdateSlot* AcquireToolUpdate(Tui* t, StrView toolCallId, u16* outIndex);

// ---- scroll / selection --------------------------------------------------

void ResetScrollbar(Tui* t);
void ClearTranscriptSelection(Tui* t);
void ResetCommandPalette(Tui* t);

// ---- session arena -------------------------------------------------------

// Compaction copies the live transcript, so it should not run constantly, but a
// fixed high threshold never fires: 800 turns of 2 KB replies only reach ~7 MB.
// Compact once the arena passes this floor, and thereafter only once it has
// doubled relative to the live set measured at the last compaction, which keeps
// the copying amortized no matter how large the scrollback cap is.
constexpr usize TUI_SESSION_COMPACT_FLOOR = (usize)2 << 20;

void HydrateTranscriptLocked(Tui* t, const Message* messages, u32 count);
void ClearSessionScopedStringsLocked(Tui* t);
bool SessionArenaSwapSafeLocked(const Tui* t);
void RebuildTranscriptLocked(Tui* t, const Message* messages, u32 count,
                             bool allowSwap);

}  // namespace TuiInternal
