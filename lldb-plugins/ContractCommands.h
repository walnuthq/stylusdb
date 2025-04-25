#pragma once

#include <lldb/API/SBCommandInterpreter.h>
#include <string>
#include <map>

// Command: "stylus-contract add <address> <library_path>"
class WalnutContractAddCommand : public lldb::SBCommandPluginInterface {
public:
  bool DoExecute(lldb::SBDebugger debugger, char **command,
                 lldb::SBCommandReturnObject &result) override;
};

// Command: "stylus-contract breakpoint <address> <function>"
class WalnutContractBreakpointCommand : public lldb::SBCommandPluginInterface {
public:
  bool DoExecute(lldb::SBDebugger debugger, char **command,
                 lldb::SBCommandReturnObject &result) override;
};

// Command: "stylus-contract list"
class WalnutContractListCommand : public lldb::SBCommandPluginInterface {
public:
  bool DoExecute(lldb::SBDebugger debugger, char **command,
                 lldb::SBCommandReturnObject &result) override;
};

// Command: "stylus-contract stack"
class WalnutContractStackCommand : public lldb::SBCommandPluginInterface {
public:
  bool DoExecute(lldb::SBDebugger debugger, char **command,
                 lldb::SBCommandReturnObject &result) override;
};

// Command: "stylus-contract context <address>"
class WalnutContractContextCommand : public lldb::SBCommandPluginInterface {
public:
  bool DoExecute(lldb::SBDebugger debugger, char **command,
                 lldb::SBCommandReturnObject &result) override;
};

// Global contract registry
struct ContractInfo {
  std::string library_path;
  lldb::SBModule module;
  std::vector<lldb::SBBreakpoint> breakpoints;
};

extern std::map<std::string, ContractInfo> g_contract_registry;
extern std::vector<std::string> g_call_stack;
extern std::string g_current_context;

// Helper functions for debugger integration
void UpdateCallStack(const std::string& stack_str);
void PushContext(const std::string& contract_address);
void PopContext();

bool RegisterWalnutContractCommands(lldb::SBCommandInterpreter &interpreter);
