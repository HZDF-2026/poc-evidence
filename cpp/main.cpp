// main.cpp — entry point: Unicode argv on Windows, binary stdout/stderr so
// "\n" never becomes "\r\n".
#include "cli.h"

#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <shellapi.h>
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    (void)argc;
    (void)argv;
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    int wargc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    std::vector<std::string> args;
    if (wargv) {
        for (int i = 1; i < wargc; i++) {
            int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
            std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
            if (n > 0) {
                WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, &s[0], n, nullptr, nullptr);
            }
            args.push_back(std::move(s));
        }
        LocalFree(wargv);
    }
    return pocev::runCli(args);
#else
    (void)argc;
    std::vector<std::string> args(argv + 1, argv + argc);
    return pocev::runCli(args);
#endif
}
