// cli.h — the frozen-usage CLI entry.
#ifndef pocev_CLI_H
#define pocev_CLI_H

#include <string>
#include <vector>

namespace pocev {

// args are the user arguments only (process.argv.slice(2)); returns the
// process exit code.
int runCli(const std::vector<std::string>& args);

}  // namespace pocev

#endif  // pocev_CLI_H
