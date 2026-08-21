#pragma once

#include "base/arena.h"
#include "base/basics.h"
#include "base/str.h"

// Renderer-neutral seam for putting a question to the user mid-run.
//
// Lives in its own header rather than in hooks.h so both the harness and the
// `ask_user` tool can include it without dragging in the session reducer.
//
// The harness never learns whether a TUI, a plain REPL, or nobody at all is on
// the other end: the app installs an implementation and everything here goes
// through it. A run with no installed seam behaves exactly as it did before one
// existed, which is what keeps `-p` and the offline suite working unchanged.

// Zero-initialized means Unavailable on purpose: a caller that forgets to set a
// status reports "nobody answered" rather than fabricating an answer.
enum class AgentAskStatus : u8 {
  Unavailable,  // no user to ask, or the seam is not installed
  Answered,
  Declined,  // a user was present and dismissed the question
};

struct AgentAskRequest {
  StrView Question;
  // Empty uses the renderer's ordinary question category.
  StrView Category;
  // Optional fixed choices. An empty list asks for free text instead.
  const StrView* Choices;
  u32 ChoiceCount;
};

struct AgentAskResponse {
  AgentAskStatus Status;
  // Only meaningful when the request supplied choices and Status is Answered.
  u32 ChoiceIndex;
  // The answer text, owned by the arena passed to the call. For a choice-based
  // question this is the selected choice verbatim.
  StrView Answer;
};

struct AgentAskUser {
  void* User;
  // Returns false when the question could not be put to a user at all. A
  // question that reached someone who declined it returns true with
  // Status == Declined, which is a different outcome and must stay distinct.
  bool (*Ask)(void* user, Arena* arena, const AgentAskRequest* request,
              AgentAskResponse* out);
};

// Single entry point, so no call site has to null-check the seam or normalize a
// partially-filled response.
inline AgentAskResponse AgentAskUser_Invoke(const AgentAskUser* ask, Arena* arena,
                                            const AgentAskRequest* request) {
  AgentAskResponse response = {};
  if (!ask || !ask->Ask) return response;
  if (!ask->Ask(ask->User, arena, request, &response)) {
    AgentAskResponse unavailable = {};
    return unavailable;
  }
  return response;
}
