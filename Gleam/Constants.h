// Constants.h - Centralized constants for Gleam debugger
// Extracts magic numbers and hardcoded strings for better maintainability

#ifndef GLEAM_CONSTANTS_H
#define GLEAM_CONSTANTS_H

#include <cstdint>

namespace Gleam {

// Thread and process limits
namespace Limits {
    constexpr int MAX_TERMINATE_RETRIES = 100;
    constexpr int TERMINATE_RETRY_DELAY_MS = 10;
    constexpr int THREAD_WAIT_TIMEOUT_MS = 1000;
    constexpr int SHORT_THREAD_WAIT_MS = 100;
    constexpr int RESOLVE_RETRY_LOG_INTERVAL = 32;
}

// Memory and buffer sizes
namespace Memory {
    constexpr size_t BREAK_IN_STUB_SIZE = 16;
    constexpr size_t BREAK_IN_PAGE_SIZE = 0x1000;
    constexpr size_t MAX_PATH_BUFFER = 260;  // MAX_PATH
    constexpr size_t EXTENDED_PATH_BUFFER = 520;  // MAX_PATH * 2
    constexpr size_t SMALL_DETAIL_BUFFER = 64;
    constexpr size_t MEDIUM_DETAIL_BUFFER = 96;
    constexpr size_t LARGE_DETAIL_BUFFER = 128;
}

// Exception codes (commonly used)
namespace ExceptionCodes {
    constexpr uint32_t BREAKPOINT = 0x80000003;
    constexpr uint32_t SINGLE_STEP = 0x80000004;
    constexpr uint32_t ACCESS_VIOLATION = 0xC0000005;
}

// String constants
namespace Strings {
    constexpr const char* DEFAULT_THREAD_PROMPT = "selected thread %u no longer exists (use 'thread' to reselect)";
    constexpr const char* DETACH_REFUSED_PAGE = "detach refused: break-in stub page still held (free failed)";
    constexpr const char* DETACH_REFUSED_THREADS = "detach refused: target left attached (threads still suspended by us)";
    constexpr const char* PATCHES_CLEARED = "patches cleared on restart";
    constexpr const char* DETACHING = "detaching...";
}

// Stop reasons (for emitStop)
namespace StopReasons {
    constexpr const char* SYSTEM = "system";
    constexpr const char* ATTACH = "attach";
    constexpr const char* BREAKPOINT = "breakpoint";
    constexpr const char* ENTRY = "entry";
    constexpr const char* STEP = "step";
    constexpr const char* STEPOVER = "stepover";
    constexpr const char* PAUSE = "pause";
    constexpr const char* EXCEPTION = "exception";
    constexpr const char* TRACE = "trace";
}

// Event types (for logging)
namespace Events {
    constexpr const char* PROCESS_CREATE = "process";
    constexpr const char* BREAKIN = "breakin";
    constexpr const char* BP_BOUND = "bp bound";
    constexpr const char* BP_UNBOUND = "bp unbound";
    constexpr const char* BP_REJECTED = "bp rejected";
    constexpr const char* IGNORED = "ignored";
    constexpr const char* EXCEPTION_EVENT = "exception";
    constexpr const char* ERROR_EVENT = "error";
}

} // namespace Gleam

#endif // GLEAM_CONSTANTS_H
