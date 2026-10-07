// StartMenu.h - the app's own entry in the Start menu.
//
// There is no installer to put one there, and without one the only way back
// after Exit is finding the executable again. So the app keeps its own: made
// the first time it runs, and repointed whenever the executable has moved.
//
// Made once, not kept: someone who deletes it has said they do not want it, and
// it does not come back on the next start.
#pragma once

namespace ee {

// |made_before| is whether a shortcut was ever made here. Returns whether one
// has been now, so the caller can remember it. A copy running from a temporary
// folder - the zip, double-clicked - leaves the Start menu alone: a shortcut to
// something Windows is about to delete is worse than none.
bool EnsureStartMenuShortcut(bool running_from_temp, bool made_before);

}  // namespace ee
