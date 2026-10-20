#pragma once
// JTTY_UDP.h
//
// JTTY support for TXLink: converts the JTTY tone list that WS sends in the
// UDP Status "TxSymbols" field into a TXlink4_Nano {J...} serial payload.
//
// WS (with jtty_udp_symbols.patch) puts in TxSymbols for mode "JTTY":
//   "J<id> t t t ..."  one accepted message: 59 tones per frame (13 sync + 46
//                      data), 1..16 frames. <id> increases with every accepted
//                      message, including messages chained onto an active TX.
//                      Other Status packets repeat the latest id - send once.
//   "JABORT"           transmission aborted (Esc / Halt Tx / watchdog)
//   ""                 anything else (e.g. natural end of transmission)
//
// Nano command {J<hex>}: data symbols only (sync is re-inserted by the Nano),
//   23 hex digits per frame, each hex digit = s0 | (s1 << 2).
//
// No project includes - pure functions, testable on their own.

#include <string>
#include <vector>
#include <cstdint>

namespace Jtty
{
    constexpr int SyncLen   = 13;
    constexpr int DataLen   = 46;
    constexpr int FrameLen  = SyncLen + DataLen;     // 59
    constexpr int MaxFrames = 16;

    enum class Kind { None, Message, Abort, Error };

    struct TxSymbols
    {
        Kind        kind   = Kind::None;
        uint64_t    id     = 0;
        int         frames = 0;
        std::string hex;            // Nano payload (without {J and })
        std::string error;
    };

    // Parse the WS TxSymbols field (see above)
    TxSymbols ParseTxSymbols(const std::string& field);

    // tones: 59*n channel symbols incl. sync -> Nano hex payload (sync removed).
    // Checks the sync of every frame. Returns false and sets error on failure.
    bool TonesToHex(const std::vector<int>& tones, std::string& hex, std::string& error);
}
