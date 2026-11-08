#include "app/extensions.h"

bool AppExtensions_Add(AppExtensions* extensions, const AppExtension* extension) {
  if (!extensions || !extension || extensions->Count >= APP_EXTENSION_CAPACITY)
    return false;
  extensions->Items[extensions->Count++] = *extension;
  return true;
}

AgentHarnessHooks AppExtensions_Hooks(AppExtensions* extensions) {
  if (!extensions) return AgentHarnessHooks{};
  extensions->Chain = AgentHookChain{};
  for (u32 i = 0; i < extensions->Count; i++) {
    AgentHarnessHooks hooks = extensions->Items[i].Hooks;
    hooks.User = extensions->Items[i].User;
    AgentHookChain_Add(&extensions->Chain, &hooks);
  }
  return AgentHookChain_Compose(&extensions->Chain);
}

void AppExtensions_SessionChanged(AppExtensions* extensions,
                                  const AppSessionContext* session) {
  if (!extensions || !session) return;
  for (u32 i = 0; i < extensions->Count; i++) {
    const AppExtension& extension = extensions->Items[i];
    if (extension.SessionChanged) extension.SessionChanged(extension.User, session);
  }
}

void AppExtensions_OnEvent(AppExtensions* extensions, const AgentHarnessEvent* event) {
  if (!extensions || !event) return;
  for (u32 i = 0; i < extensions->Count; i++) {
    const AppExtension& extension = extensions->Items[i];
    if (extension.OnEvent) extension.OnEvent(extension.User, event);
  }
}

void AppExtensions_Shutdown(AppExtensions* extensions) {
  if (!extensions || extensions->ShutDown) return;
  extensions->ShutDown = true;
  for (u32 i = 0; i < extensions->Count; i++) {
    const AppExtension& extension = extensions->Items[i];
    if (extension.Shutdown) extension.Shutdown(extension.User);
  }
}
