#pragma once

#include <QString>

namespace tandem {

// A one-time token from the Wayland compositor (xdg-activation) for a program
// Tandem starts, so its window gets the focus even where window rules put it
// on another workspace. "" off Wayland, without the protocol, or off the UI
// thread. Implemented in Activation_wayland.cpp (else Activation_none.cpp).
QString requestActivationToken();

}  // namespace tandem
