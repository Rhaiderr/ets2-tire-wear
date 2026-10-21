// F1/F2: force feedback on the player's wheel.
//
// With a force feedback wheel, a steering offset sent through the Input API
// is felt backwards: the game's own force feedback brings the virtual steering
// back to centre, so the real wheel pushes away from the blown tire and the
// truck straightens itself [verified: G923 test, vega 133]. So the pull goes
// to the wheel itself: a constant force of our own, added to the game's.
//
// The game sets its wheels up through DirectInput 8 before the plugin loads
// [verified: game.log "[di8] Initializing device ..."], so the plugin cannot
// see that happen. Instead it detours the device read functions of the
// system's dinput8.dll (in memory, with MinHook; no file is touched) and, as
// the game reads its devices, keeps the force feedback wheel among them (a
// driving device first: a PC may also have a virtual joystick with force
// feedback [verified: vega, vJoy read by the game next to the G923]). The
// force is created on that same device object, which the game holds in
// exclusive mode, and updated right after the game's own reads.
#pragma once

#include <string>
#include <vector>

namespace ffb {

// Game thread, once: install the detours (no-op without a game controller).
// device: when not empty, only a force feedback device whose name contains
// it (any case) is used, even one that does not call itself a wheel.
void Start(bool invert, const std::string& device);

// Shutdown: the force stops, the detours turn into pass-through (the DLL is
// pinned) and the wheel is let go.
void Stop();

// True while a force feedback wheel is in use (found, a driving device, no
// failure): the pull then goes to the wheel instead of the steering input.
bool Active();

// Game thread, every frame: the force wanted now, -1 (left) ... 1 (right).
void SetForce(float force);

// Messages from any thread, to be forwarded to the game log from the game thread.
std::vector<std::string> TakeMessages();

}  // namespace ffb
