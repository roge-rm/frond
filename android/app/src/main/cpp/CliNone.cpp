// Android has no readline console. This keeps the command table that Commands.cpp
// registers and makes Start, Stop and Execute do nothing.
#include <string>
#include <unordered_map>
#include <vector>

#include "Cli.h"

namespace {
    std::vector<cli::Command> commands;
    std::unordered_map<std::string, size_t> commandMap;
}  // namespace

namespace cli {
    void AddCommands(const std::vector<Command>& newCommands) {
        for (const auto& command : newCommands) {
            commands.push_back(command);
            commandMap.insert({std::string(command.name), commands.size() - 1});
        }
    }

    const Command* GetCommand(const std::string& name) {
        auto it = commandMap.find(name);
        return it == commandMap.end() ? nullptr : &commands[it->second];
    }

    const std::vector<Command>& GetCommands() { return commands; }

    void Start(std::optional<std::string>) {}

    void Stop() {}

    bool Execute(void*) { return false; }
}  // namespace cli
