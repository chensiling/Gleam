// GleamTests - Unit test entry point

#include <gtest/gtest.h>

#ifdef _WIN32
#include <windows.h>
#include <crtdbg.h>
#include <cstdio>
#include <cstdlib>

// Route CRT diagnostics to stderr instead of a modal dialog.
//
// Without this, an abort() or failed CRT assertion inside any test opens the
// "Microsoft Visual C++ Runtime Library - Debug Error!" message box and the run
// blocks waiting for a click. Unbuffered stdout keeps gtest's progress output
// from being lost, since abort() does not flush the stream.
static void suppressCrtDialogs() {
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    const int reports[] = { _CRT_WARN, _CRT_ERROR, _CRT_ASSERT };
    for (int report : reports) {
        _CrtSetReportMode(report, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(report, _CRTDBG_FILE_STDERR);
    }

    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
}
#endif

int main(int argc, char **argv) {
#ifdef _WIN32
    suppressCrtDialogs();
#endif
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
