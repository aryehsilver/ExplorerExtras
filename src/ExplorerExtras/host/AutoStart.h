// AutoStart.h - "start with Windows" for the standalone tray build.
//
// Host-layer only. A host that owns process lifetime itself would simply not
// compile this file in.
//
// Two things can make the Run entry stop working while the setting still says
// it is on, and both have been seen in the wild:
//
//   - The executable moves. This app is a single file with no installer, so
//     being run from Downloads and then put somewhere permanent is the normal
//     course of events - and the entry still points at where it used to be.
//   - Windows' own Startup apps switch is turned off. That lives in a separate
//     key and silently overrides the Run entry, which stays exactly as it was.
//   - The executable is running from inside the downloaded zip. Windows
//     extracts it to a temporary folder for the double-click and deletes that
//     folder later, taking the startup with it.
//
// So the question "is auto-start on?" is not "does the value exist?", and the
// state is reported in enough detail to say which of those is the matter.
#pragma once

#include <string>

namespace ee {

struct AutoStartState {
    bool registered = false;   // there is a Run entry under our name
    bool points_here = false;  // ...and it names the executable now running
    bool blocked = false;      // Windows' Startup apps switch is off for us
    bool temporary = false;    // we are running from a folder Windows will bin
    std::wstring command;      // what the entry actually says, quotes and all
};

AutoStartState AutoStartStatus();

// True only when Windows would actually start this executable at sign-in.
bool IsAutoStartEnabled();

// Registers or removes the Run entry, always pointing at the current exe.
// Writing it again is how a moved executable repairs itself.
bool SetAutoStartEnabled(bool enabled);

// Turns Windows' own Startup apps switch back on for us. Only ever called
// because someone asked for auto-start by hand: a switch turned off in the
// Settings app is a decision, and is not to be quietly undone.
bool ClearStartupBlock();

}  // namespace ee
