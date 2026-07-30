// Error handling implementation

#include "Error.h"
#include <sstream>
#include <Windows.h>

namespace Gleam {

std::string Error::format() const {
    std::ostringstream oss;

    // Category
    switch (category) {
        case ErrorCategory::Memory:
            oss << "[Memory Error] ";
            break;
        case ErrorCategory::Symbol:
            oss << "[Symbol Error] ";
            break;
        case ErrorCategory::Process:
            oss << "[Process Error] ";
            break;
        case ErrorCategory::Breakpoint:
            oss << "[Breakpoint Error] ";
            break;
        case ErrorCategory::Exception:
            oss << "[Exception Error] ";
            break;
        case ErrorCategory::Internal:
            oss << "[Internal Error] ";
            break;
        case ErrorCategory::System:
            oss << "[System Error] ";
            break;
        default:
            oss << "[Unknown Error] ";
            break;
    }

    // Message
    oss << message;

    // Context
    if (!context.empty()) {
        oss << " (context: " << context << ")";
    }

    // System error code
    if (systemCode != 0) {
        oss << " [code: 0x" << std::hex << systemCode << std::dec << "]";

        std::string sysMsg = formatWindowsError(systemCode);
        if (!sysMsg.empty()) {
            oss << " - " << sysMsg;
        }
    }

    return oss.str();
}

std::string formatWindowsError(int errorCode) {
    if (errorCode == 0)
        return "";

    char* msgBuf = nullptr;
    DWORD size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        errorCode,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPSTR)&msgBuf,
        0,
        NULL
    );

    if (size == 0) {
        return "";
    }

    std::string result(msgBuf, size);
    LocalFree(msgBuf);

    // Remove trailing newlines
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }

    return result;
}

} // namespace Gleam
