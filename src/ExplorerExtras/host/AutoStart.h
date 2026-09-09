// AutoStart.h - "start with Windows" for the standalone tray build.
//
// Host-layer only. A host that owns process lifetime itself would simply not
// compile this file in.
#pragma once

namespace ee {

bool IsAutoStartEnabled();

// Registers or removes the Run entry, always pointing at the current exe.
bool SetAutoStartEnabled(bool enabled);

}  // namespace ee
