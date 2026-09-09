// AutoStart.h - "start with Windows" for the standalone tray build.
//
// Host-layer only. Under PowerToys the runner owns process lifetime, so this
// file is simply not compiled into the module.
#pragma once

namespace ee {

bool IsAutoStartEnabled();

// Registers or removes the Run entry, always pointing at the current exe.
bool SetAutoStartEnabled(bool enabled);

}  // namespace ee
