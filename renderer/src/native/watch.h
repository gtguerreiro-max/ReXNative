// Guest memory watch: a hardware data breakpoint on one guest dword, for finding which recompiled
// function writes a value (Fable II's NaN camera matrix: the guest D3D's constant shadow).
//
// The control file's `watch <guest address hex> [nan|any] [count]` puts the address in debug
// register 0 of every thread of the process (Windows; elsewhere it logs that it is unsupported).
// Each write that matches (`nan`: the dword now holds a NaN; `any`: every write) logs the value
// and the recompiled functions on the host stack, innermost first, as guest addresses
// ("sub_82345678+0x1C4"), from the generated code's function table. `watch off` clears it.
// `read` mode takes a physical address and watches reads and writes through its three CPU views
// (0xA0000000, 0xC0000000, 0xE0000000 + address: debug registers 0-2), the guest code's only:
// who reads back a resolve (Blur's exposure).
#pragma once

#include <cstdint>
#include <string>

namespace rexnative::watch {

// Starts (or moves) the watch; `mode` "nan", "any" or "read"; at most `count` accesses logged.
void Set(uint32_t guest_address, const std::string& mode, uint32_t count);
void Clear();

}  // namespace rexnative::watch
