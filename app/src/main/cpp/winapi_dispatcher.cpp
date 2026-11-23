#include "winapi_dispatcher.h"
#include <jni.h>

#include <algorithm>
#include <cctype>
#include <sstream>

namespace asterpc {
namespace {

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool supportedModule(const std::string& dll) {
    const std::string module=lowerAscii(dll);
    return module=="kernel32.dll" || module=="kernelbase.dll";
}

DispatchResult returned(uint64_t value=0) {
    return {DispatchStatus::Returned,value};
}

} // namespace

DispatchResult WinApiDispatcher::dispatch(const std::string& dll,
                                          const std::string& symbol,
                                          Win64CallFrame& frame) const {
    if (!supportedModule(dll)) return {};

    // Names are intentionally case-sensitive, matching PE export lookup.
    if (symbol=="GetTickCount") {
        return returned(static_cast<uint32_t>(frame.virtualMilliseconds));
    }
    if (symbol=="GetTickCount64") {
        return returned(frame.virtualMilliseconds);
    }
    if (symbol=="GetLastError") {
        return returned(frame.lastError);
    }
    if (symbol=="SetLastError") {
        frame.lastError=static_cast<uint32_t>(frame.rcx);
        return returned();
    }
    if (symbol=="GetCurrentProcess") {
        // Windows pseudo-handle: (HANDLE)-1. It is not a host process handle.
        return returned(UINT64_MAX);
    }
    if (symbol=="ExitProcess") {
        frame.exitCode=static_cast<uint32_t>(frame.rcx);
        frame.exitRequested=true;
        return {DispatchStatus::ExitRequested,0};
    }
    return {};
}

std::string runWinApiDispatcherSelfTests() {
    WinApiDispatcher dispatcher;
    unsigned passed=0;
    unsigned total=0;
    std::ostringstream report;
    auto check=[&](const char* name, bool ok) {
        ++total;
        if (ok) ++passed;
        report << (ok ? "PASS: " : "FAIL: ") << name << "\n";
    };

    Win64CallFrame frame;
    frame.virtualMilliseconds=0x100000005ULL;
    frame.lastError=87;
    DispatchResult result=dispatcher.dispatch("KERNEL32.DLL","GetTickCount",frame);
    check("GetTickCount returns the low 32 bits of virtual time",
          result.status==DispatchStatus::Returned && result.value==5);

    result=dispatcher.dispatch("kernelbase.dll","GetTickCount64",frame);
    check("GetTickCount64 returns the full virtual tick count",
          result.status==DispatchStatus::Returned && result.value==0x100000005ULL);

    result=dispatcher.dispatch("kernel32.dll","GetLastError",frame);
    check("GetLastError returns thread-context last-error value",
          result.status==DispatchStatus::Returned && result.value==87);

    frame.rcx=1234;
    result=dispatcher.dispatch("kernel32.dll","SetLastError",frame);
    check("SetLastError consumes RCX under the Win64 integer-argument convention",
          result.status==DispatchStatus::Returned && result.value==0 && frame.lastError==1234);

    result=dispatcher.dispatch("kernel32.dll","GetCurrentProcess",frame);
    check("GetCurrentProcess returns the documented pseudo-handle",
          result.status==DispatchStatus::Returned && result.value==UINT64_MAX);

    frame.rcx=0x2A;
    result=dispatcher.dispatch("kernel32.dll","ExitProcess",frame);
    check("ExitProcess records the guest exit code and requests guest termination",
          result.status==DispatchStatus::ExitRequested && frame.exitRequested && frame.exitCode==42);

    const bool before=frame.exitRequested;
    result=dispatcher.dispatch("user32.dll","GetTickCount",frame);
    check("unsupported DLL is rejected without side effects",
          result.status==DispatchStatus::NotFound && frame.exitRequested==before);

    result=dispatcher.dispatch("kernel32.dll","gettickcount",frame);
    check("unknown or incorrectly cased export is unresolved",
          result.status==DispatchStatus::NotFound);

    report << "Win32 subset dispatcher: " << passed << "/" << total << " tests passed.\n"
           << "Scope: deterministic call-frame dispatcher. The experimental CPU can invoke explicitly registered test thunks; PE import entries are not yet automatically bound, and broad Win32 compatibility is not implemented.";
    return report.str();
}

} // namespace asterpc

extern "C" JNIEXPORT jstring JNICALL
Java_com_asterpc_MainActivity_runWinApiSelfTest(JNIEnv* env, jobject) {
    const std::string result=asterpc::runWinApiDispatcherSelfTests();
    return env->NewStringUTF(result.c_str());
}
