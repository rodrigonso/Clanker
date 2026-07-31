#pragma once

#include "session/entry.h"

// Folded view of a session's tree records. Built once at open time
// (SessionReducer_Reduce) and then updated in O(1) per appended record
// (SessionReducer_Fold) -- callers must never rebuild it from scratch on a
// hot per-turn path. `Records`/`RecordCount` mirror the caller's backing
// array; since that array can grow (Array_Push may relocate), callers must
// refresh both fields (SessionReducer_Fold does this) before every read.
struct SessionReduction {
  const SessionRecord* Records;
  u32 RecordCount;
  // Every record ID maps to its position. Reference lookups additionally
  // verify the indexed record is a tree node; duplicate checks do not.
  DurableIdIndex Index;
  DurableId HeadId;
  bool HeadIsSet;

  // Settings inherited along the ancestry chain of the current head. Only
  // recomputed (O(branch)) when a Leaf record explicitly moves the head;
  // ordinary appends onto the current head update these in O(1).
  StrView Provider;
  StrView Model;
  StrView ThinkingLevel;
  Array<StrView> ToolNames;
};

// Allocates `out->Index` from `arena` and folds every record in order. Full
// O(n) bulk validation (via SessionRecords_Validate) runs exactly once here;
// this is meant for session open only, never for a per-turn/per-append path.
SessionResult SessionReducer_Reduce(Arena* arena, const SessionRecord* records, u32 count,
                                    SessionReduction* out);

// Validates one candidate record against the already-folded `reduction`
// using O(1) index lookups only (no re-scan of prior history). `pending`/
// `pendingCount` (optional) are earlier records of the *same* in-flight
// append batch that have already passed this check but are not yet folded
// into `reduction`; they are matched with a bounded linear scan over just
// that small batch, never over the historical log. Callers append the
// record only if this returns Ok.
SessionResult SessionReducer_ValidateAppend(const SessionReduction* reduction,
                                            const SessionRecord* candidate,
                                            const SessionRecord* pending = nullptr,
                                            u32 pendingCount = 0);

// Incrementally folds records[count-1] into `reduction`, which must already
// reflect records[0..count-2]. `records`/`count` become the reduction's new
// base pointer/count. O(1) except for the rare Leaf (checkout) record, which
// costs O(branch) to recompute inherited Model/Thinking/Tools settings.
void SessionReducer_Fold(SessionReduction* reduction, const SessionRecord* records, u32 count);

SessionResult SessionReducer_Ancestry(const SessionReduction* reduction, Arena* arena,
                                      DurableId headId,
                                      Array<const SessionRecord*>* outBranch);

// Materializes the model-visible message sequence for a branch. Harness
// records and branch settings never enter this array.
SessionResult SessionReducer_Context(Arena* arena, const SessionReduction* reduction,
                                     DurableId headId, Array<Message>* outMessages);

// Full O(n) bulk validation used only at session open (and other one-shot
// contexts like fork copy verification): schema-version, envelope,
// contiguous seq, duplicate-EntryId, and parent/reference-existence checks
// across the whole record set. Builds and discards its own temporary
// DurableIdIndex; never called from a per-append hot path.
SessionResult SessionRecords_Validate(const SessionRecord* records, u32 count);
