#pragma once

// T3-B2: watches the game's map trigger actions (weigh stations, and maybe
// customs) without changing them. The game runs every trigger action through
// one dispatcher, Run(action*), with the action's type in [action+0x14]
// (1-31, a jump table; types 6 and 15 lead to the weigh station code)
// [verified: disasm of eurotrucks2.exe 1.61.1.9s at 0x9C0D20, amtrucks.exe
// 1.61.3.18s at 0x9C2120, T3-B1].
//
// The detour reads the type, always calls the game's own function first and
// only then queues a record; the plugin drains the queue once per frame.

#include <string>
#include <vector>

namespace trigger_probe {

struct Action {
    int type = 0;
    double seconds = 0.0;  // steady clock
};

// Finds the dispatcher by pattern and hooks it (MinHook). Returns a line for
// the log; nothing is hooked when the pattern is not found exactly once.
std::string Start();
void Stop();

// Any thread: the actions run since the last call.
std::vector<Action> Drain();

}  // namespace trigger_probe
