#pragma once

#include "app/runtime.h"

// The interactive slash commands. Each handler matches an AppCommandHandlers
// slot and takes the runtime as its `user` pointer, so the set can be bound
// without knowing which renderer is driving it.
AppCommandHandlers RuntimeCommandHandlers(InteractiveRuntime* rt);

// Clears the conversation and starts a fresh session, keeping the process's
// provider/model selection. Returns false when the reset could not complete.
bool RuntimeResetSession(InteractiveRuntime* rt, const AppCommandUx* ux);

TuiCommandSpec* RuntimeCommandCatalog(InteractiveRuntime* rt, Arena* arena,
                                      u32* count);
