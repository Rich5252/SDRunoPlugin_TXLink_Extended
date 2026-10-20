// JTTY_UDP.cpp - see JTTY_UDP.h

#include "JTTY_UDP.h"
#include <sstream>

namespace Jtty
{
    // Sync vector is13 from WSJT-X lib/jtty/jtty_fec_mod.f90
    static const int Sync[SyncLen] = { 0,2,2,3,0,0,3,2,1,3,1,2,0 };

    bool TonesToHex(const std::vector<int>& tones, std::string& hex, std::string& error)
    {
        static const char digits[] = "0123456789ABCDEF";
        hex.clear();

        if (tones.empty() || tones.size() % FrameLen != 0) {
            error = "tone count " + std::to_string(tones.size()) + " is not a multiple of 59";
            return false;
        }
        int frames = static_cast<int>(tones.size() / FrameLen);
        if (frames > MaxFrames) {
            error = std::to_string(frames) + " frames exceeds maximum of 16";
            return false;
        }

        hex.reserve(frames * (DataLen / 2));
        for (int f = 0; f < frames; f++) {
            const int* t = &tones[f * FrameLen];
            for (int k = 0; k < SyncLen; k++) {
                if (t[k] != Sync[k]) {
                    error = "bad sync in frame " + std::to_string(f + 1);
                    hex.clear();
                    return false;
                }
            }
            for (int d = 0; d < DataLen; d += 2) {
                int s0 = t[SyncLen + d], s1 = t[SyncLen + d + 1];
                if (s0 < 0 || s0 > 3 || s1 < 0 || s1 > 3) {
                    error = "tone out of range in frame " + std::to_string(f + 1);
                    hex.clear();
                    return false;
                }
                hex += digits[s0 | (s1 << 2)];
            }
        }
        return true;
    }

    TxSymbols ParseTxSymbols(const std::string& field)
    {
        TxSymbols r;
        if (field.empty()) return r;                         // Kind::None
        if (field == "JABORT") { r.kind = Kind::Abort; return r; }

        std::istringstream iss(field);
        std::string tag;
        iss >> tag;
        if (tag.size() < 2 || tag[0] != 'J') {
            r.kind = Kind::Error;
            r.error = "unexpected TxSymbols prefix: " + tag.substr(0, 16);
            return r;
        }
        try {
            r.id = std::stoull(tag.substr(1));
        }
        catch (...) {
            r.kind = Kind::Error;
            r.error = "bad message id: " + tag;
            return r;
        }

        std::vector<int> tones;
        tones.reserve(FrameLen * MaxFrames);
        int v;
        while (iss >> v) tones.push_back(v);
        if (!iss.eof()) {
            r.kind = Kind::Error;
            r.error = "non-numeric tone in TxSymbols";
            return r;
        }

        if (!TonesToHex(tones, r.hex, r.error)) {
            r.kind = Kind::Error;
            return r;
        }
        r.frames = static_cast<int>(tones.size() / FrameLen);
        r.kind = Kind::Message;
        return r;
    }
}
