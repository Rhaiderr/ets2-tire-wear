#include "payment.h"

#include "pattern.h"

namespace payment {
namespace {

// Pay's first bytes: the null check, the amount > 0 check and the balance
// compare (T3-M1).
const char* kPayPattern =
    "48 83 EC 58 48 85 C9 0F 84 ?? ?? ?? ?? 48 85 D2 0F 8E ?? ?? ?? ?? 48 8B 41 10 48 39 50 10 7D ?? "
    "48 8B 0D ?? ?? ?? ?? BA 06 00 00 00";
// A caller in the game: mov rdi,[economy] ... mov r8b,1; mov rdx,r9; mov rcx,rdi; call Pay.
const char* kCallerPattern = "48 8B 3D ?? ?? ?? ?? 4C 8B FA 48 89 7D 88 74 ?? 41 B0 01 49 8B D1 48 8B CF E8";
constexpr size_t kCallerEconomyRel = 3;  // rel32 of "mov rdi,[rip+x]"; next instruction at +7
constexpr size_t kCallerCallAt = 25;     // the E8; its rel32 at +26, next instruction at +30

}  // namespace

Located Locate() {
    Located out;
    const pattern::Range text = pattern::GameText();
    if (!text.size) {
        out.detail = "no .text section in the game's executable";
        return out;
    }
    const auto pay = pattern::Find(text, kPayPattern);
    const auto caller = pattern::Find(text, kCallerPattern);
    if (pay.size() != 1 || caller.size() != 1) {
        out.detail = "patterns found pay " + pattern::Count(pay.size()) + " time(s), caller " +
                     pattern::Count(caller.size()) + " time(s) (need 1 each)";
        return out;
    }
    const uint8_t* c = caller[0];
    const uint8_t* economy_slot = pattern::Rel32(c + kCallerEconomyRel, c + kCallerEconomyRel + 4);
    const uint8_t* call_target = pattern::Rel32(c + kCallerCallAt + 1, c + kCallerCallAt + 5);
    if (call_target != pay[0]) {
        out.detail = "the caller calls exe+" + pattern::Hex(call_target - text.base) +
                     ", not the pay pattern at exe+" + pattern::Hex(pay[0] - text.base);
        return out;
    }
    out.ok = true;
    out.pay = reinterpret_cast<uintptr_t>(pay[0]);
    out.economy_slot = reinterpret_cast<uintptr_t>(economy_slot);
    out.detail = "pay at exe+" + pattern::Hex(pay[0] - text.base) + ", economy at exe+" +
                 pattern::Hex(economy_slot - text.base);
    return out;
}

uintptr_t Economy(const Located& l) {
    return l.ok ? *reinterpret_cast<const uintptr_t*>(l.economy_slot) : 0;
}

int64_t Balance(const Located& l) {
    const uintptr_t economy = Economy(l);
    if (!economy) return -1;
    const uintptr_t bank = *reinterpret_cast<const uintptr_t*>(economy + 0x10);
    return bank ? *reinterpret_cast<const int64_t*>(bank + 0x10) : -1;
}

Result Take(const Located& l, int64_t amount) {
    const uintptr_t economy = Economy(l);
    if (!economy || amount <= 0) return Result::kUnavailable;  // a negative amount would deposit
    using PayFn = bool(__fastcall*)(uintptr_t economy, int64_t amount, bool commit);
    return reinterpret_cast<PayFn>(l.pay)(economy, amount, true) ? Result::kTaken : Result::kRefused;
}

}  // namespace payment
