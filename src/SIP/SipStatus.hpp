// SipStatus.hpp
#pragma once
#include <cstdint>
#include <string_view>
#include <optional>

namespace PocketDial {

enum class SipStatusClass : uint8_t {
    Provisional = 1,
    Success     = 2,
    Redirection = 3,
    ClientError = 4,
    ServerError = 5,
    GlobalFail  = 6,
    Unknown     = 0
};

struct SipStatusInfo {
    uint16_t       code;
    SipStatusClass klass;
    bool           softFail;  // 4xx only: log+continue vs abort
};

std::optional<SipStatusInfo> parseSipStatusLine(std::string_view line) noexcept;
SipStatusClass               sipStatusClass(uint16_t code) noexcept;

} // namespace PocketDial
