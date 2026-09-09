// NavigateUpFeature.h - double-click empty space in a folder view to go up.
//
// Features never talk to the host (tray app today, PowerToys module later).
// They receive a gesture and act on the shell, nothing more.
#pragma once

#include "../core/MouseHook.h"
#include "../core/ViewHitTest.h"

namespace ee {

class NavigateUpFeature {
public:
    void Initialize(ViewHitTester* tester) { tester_ = tester; }
    void OnGesture(const GestureEvent& event);

private:
    ViewHitTester* tester_ = nullptr;
};

}  // namespace ee
