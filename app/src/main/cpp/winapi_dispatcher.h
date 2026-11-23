#pragma once

#include <cstdint>
#include <string>

namespace asterpc {

struct Win64CallFrame {
    // Microsoft x64 ABI integer/pointer argument registers.
    uint64_t rcx=0;
    uint64_t rdx=0;
    uint64_t r8=0;
    uint64_t r9=0;
    uint64_t virtualMilliseconds=0;
    uint32_t lastError=0;
    bool exitRequested=false;
    uint32_t exitCode=0;
};

enum class DispatchStatus {
    NotFound,
    Returned,
    ExitRequested
};

struct DispatchResult {
    DispatchStatus status=DispatchStatus::NotFound;
    uint64_t value=0;
};

class WinApiDispatcher {
public:
    bool supports(const std::string& dll,const std::string& symbol) const;
    DispatchResult dispatch(const std::string& dll,
                            const std::string& symbol,
                            Win64CallFrame& frame) const;
};

std::string runWinApiDispatcherSelfTests();

} // namespace asterpc
