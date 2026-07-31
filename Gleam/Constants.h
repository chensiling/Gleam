/**
 * @file Constants.h
 * @brief Centralized constants for the Gleam debugger
 *
 * Extracts magic numbers and hardcoded strings from implementation files
 * to improve maintainability and consistency. All constants are organized
 * into namespaces by category.
 *
 * **Design rationale:**
 * - Magic numbers scattered across code are hard to maintain and document
 * - Centralizing constants makes it easy to tune parameters (timeouts, limits)
 * - String constants ensure consistency in logging and error messages
 * - constexpr ensures compile-time evaluation and zero runtime cost
 *
 * @note All constants are constexpr for compile-time evaluation
 */

#ifndef GLEAM_CONSTANTS_H
#define GLEAM_CONSTANTS_H

#include <cstdint>

namespace Gleam {

/**
 * @brief Thread and process operation limits
 *
 * Tunable parameters for retry logic and timeout handling in process
 * and thread management operations.
 */
namespace Limits {
    /**
     * @brief Maximum retry attempts for thread termination
     *
     * Used in resolveExitContract() when waiting for threads to exit
     * after termination request. Prevents infinite loops if threads
     * are stuck.
     */
    constexpr int MAX_TERMINATE_RETRIES = 100;

    /**
     * @brief Delay between termination retry attempts (milliseconds)
     *
     * Sleep duration between checks when waiting for thread termination.
     * Total max wait = MAX_TERMINATE_RETRIES * TERMINATE_RETRY_DELAY_MS.
     */
    constexpr int TERMINATE_RETRY_DELAY_MS = 10;

    /**
     * @brief Standard thread wait timeout (milliseconds)
     *
     * Default timeout for WaitForSingleObject on thread handles.
     */
    constexpr int THREAD_WAIT_TIMEOUT_MS = 1000;

    /**
     * @brief Short thread wait timeout (milliseconds)
     *
     * Used for quick checks where we don't want to block for long.
     */
    constexpr int SHORT_THREAD_WAIT_MS = 100;

    /**
     * @brief Log interval for resolve retry operations
     *
     * Emit a progress log every N retry attempts to avoid log spam
     * while still showing that work is happening.
     */
    constexpr int RESOLVE_RETRY_LOG_INTERVAL = 32;

    /**
     * @brief Upper bound on a single "stepn" request.
     *
     * Every instruction costs one debug event plus one line of output, so a
     * mistyped count is a denial of service against the operator's terminal
     * rather than a useful request. `tgo` is the tool for long runs.
     */
    constexpr uint64_t STEPN_MAX = 0x10000;

    /**
     * @brief Cap on the bytes read for one OutputDebugString event.
     *
     * nDebugStringLength is supplied by the debuggee, so it sizes a read of
     * debuggee-controlled length. 32 KiB is far above any real diagnostic
     * message and keeps a hostile length field from turning every event into
     * a huge allocation plus escape pass.
     */
    constexpr size_t DEBUGSTRING_MAX_BYTES = 32 * 1024;

    /**
     * @brief Cap on the TLS callback array walk in "moduleinfo".
     *
     * The array is NUL-terminated in a well-formed image, but the terminator
     * lives in target memory: a corrupt or deliberately hostile image must not
     * be able to spin the walk. Real images have single digits of callbacks.
     */
    constexpr uint32_t TLS_CALLBACK_MAX = 64;
}

/**
 * @brief Memory and buffer size constants
 *
 * Size constants for memory allocations, buffers, and page-related
 * operations. Sizes are chosen based on Windows API requirements
 * and performance characteristics.
 */
namespace Memory {
    /**
     * @brief Size of break-in stub code (bytes)
     *
     * The injected stub that triggers a breakpoint for attach operations.
     * Must be large enough to hold the stub code sequence.
     */
    constexpr size_t BREAK_IN_STUB_SIZE = 16;

    /**
     * @brief Page size for break-in stub allocation (bytes)
     *
     * Allocate a full page (4KB) for the stub to ensure proper alignment
     * and protection. Windows memory management works in 4KB pages.
     */
    constexpr size_t BREAK_IN_PAGE_SIZE = 0x1000;

    /**
     * @brief Standard MAX_PATH buffer size
     *
     * Windows MAX_PATH constant (260 characters). Used for file paths
     * and module names.
     */
    constexpr size_t MAX_PATH_BUFFER = 260;

    /**
     * @brief Extended path buffer (2x MAX_PATH)
     *
     * For operations that may need longer paths or concatenation.
     */
    constexpr size_t EXTENDED_PATH_BUFFER = 520;

    /**
     * @brief Small detail string buffer
     *
     * For short context strings in logging and error messages
     * (e.g., hex addresses, small symbol names).
     */
    constexpr size_t SMALL_DETAIL_BUFFER = 64;

    /**
     * @brief Medium detail string buffer
     *
     * For typical detail strings (symbol names, short messages).
     */
    constexpr size_t MEDIUM_DETAIL_BUFFER = 96;

    /**
     * @brief Large detail string buffer
     *
     * For longer detail strings (formatted error messages, full contexts).
     */
    constexpr size_t LARGE_DETAIL_BUFFER = 128;
}

/**
 * @brief Windows exception codes
 *
 * Commonly used exception codes from ntstatus.h. These are the values
 * passed to exception handlers and used to identify exception types.
 */
namespace ExceptionCodes {
    /**
     * @brief Breakpoint exception (int3)
     *
     * Generated by the int3 instruction (0xCC). Used for software
     * breakpoints and the break-in stub.
     */
    constexpr uint32_t BREAKPOINT = 0x80000003;

    /**
     * @brief Single-step exception (trap flag)
     *
     * Generated when the trap flag (TF) in EFLAGS is set. Used for
     * step-into and step-over operations.
     */
    constexpr uint32_t SINGLE_STEP = 0x80000004;

    /**
     * @brief Access violation exception
     *
     * Generated when accessing invalid memory (read/write/execute violation).
     * The most common crash exception.
     */
    constexpr uint32_t ACCESS_VIOLATION = 0xC0000005;
}

/**
 * @brief String constants for messages and prompts
 *
 * User-facing strings used in logging, error messages, and interactive
 * prompts. Centralizing these ensures consistency and makes localization
 * easier (if ever needed).
 */
namespace Strings {
    /**
     * @brief Prompt when selected thread no longer exists
     *
     * Shown when the user's currently selected thread has exited and
     * they need to select a new one.
     */
    constexpr const char* DEFAULT_THREAD_PROMPT = "selected thread %u no longer exists (use 'thread' to reselect)";

    /**
     * @brief Detach refusal: break-in stub page still held
     *
     * Shown when detach is refused because the injected break-in stub
     * page couldn't be freed (zombie thread still holding it).
     */
    constexpr const char* DETACH_REFUSED_PAGE = "detach refused: break-in stub page still held (free failed)";

    /**
     * @brief Detach refusal: threads still suspended
     *
     * Shown when detach is refused because some threads are still
     * suspended by the debugger and couldn't be resumed.
     */
    constexpr const char* DETACH_REFUSED_THREADS = "detach refused: target left attached (threads still suspended by us)";

    /**
     * @brief Confirmation message after clearing patches on restart
     */
    constexpr const char* PATCHES_CLEARED = "patches cleared on restart";

    /**
     * @brief Status message shown when detaching
     */
    constexpr const char* DETACHING = "detaching...";
}

/**
 * @brief Stop reason strings for emitStop()
 *
 * Standardized reason codes passed to the stop event logger. These
 * appear in logs and may be parsed by frontend tools.
 */
namespace StopReasons {
    constexpr const char* SYSTEM = "system";           ///< System-level stop (initial attach)
    constexpr const char* ATTACH = "attach";           ///< Stop due to attach operation
    constexpr const char* BREAKPOINT = "breakpoint";   ///< Hit a breakpoint
    constexpr const char* ENTRY = "entry";             ///< Stopped at entry point
    constexpr const char* STEP = "step";               ///< Single-step completed
    constexpr const char* STEPOVER = "stepover";       ///< Step-over completed
    constexpr const char* PAUSE = "pause";             ///< User-requested pause
    constexpr const char* EXCEPTION = "exception";     ///< Exception occurred
    constexpr const char* TRACE = "trace";             ///< Trace mode stop
}

/**
 * @brief Event type strings for logging
 *
 * Standardized event names used in logEvent() calls. These provide
 * consistent categorization for debugger events in logs.
 */
namespace Events {
    constexpr const char* PROCESS_CREATE = "process";     ///< Process creation event
    constexpr const char* BREAKIN = "breakin";            ///< Break-in operation
    constexpr const char* BP_BOUND = "bp bound";          ///< Breakpoint successfully bound
    constexpr const char* BP_UNBOUND = "bp unbound";      ///< Breakpoint unbound/removed
    constexpr const char* BP_REJECTED = "bp rejected";    ///< Breakpoint rejected (invalid address, etc.)
    constexpr const char* IGNORED = "ignored";            ///< Event ignored by handler
    constexpr const char* EXCEPTION_EVENT = "exception";  ///< Exception event
    constexpr const char* ERROR_EVENT = "error";          ///< Error event
}

} // namespace Gleam

#endif // GLEAM_CONSTANTS_H
