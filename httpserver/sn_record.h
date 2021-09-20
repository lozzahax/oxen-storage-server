#pragma once

#include <cstdint>
#include <string>

#include "lozzaxd_key.h"

namespace oxen {

struct sn_record {
    std::string ip;
    uint16_t port{0};
    uint16_t omq_port{0};
    legacy_pubkey pubkey_legacy{};
    ed25519_pubkey pubkey_ed25519{};
    x25519_pubkey pubkey_x25519{};
};

// Returns true if two sn_record's refer to the same snode (i.e. have the same legacy pubkey).
// Note that other fields/pubkeys are not checked.
inline bool operator==(const sn_record& lhs, const sn_record& rhs) {
    return lhs.pubkey_legacy == rhs.pubkey_legacy;
}
// Returns true if two sn_record's have different pubkey_legacy values.
inline bool operator!=(const sn_record& lhs, const sn_record& rhs) {
    return !(lhs == rhs);
}

}
