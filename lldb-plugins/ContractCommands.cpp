#include "ContractCommands.h"
#include <lldb/API/SBDebugger.h>
#include <lldb/API/SBTarget.h>
#include <lldb/API/SBModule.h>
#include <lldb/API/SBBreakpoint.h>
#include <lldb/API/SBBreakpointLocation.h>
#include <lldb/API/SBCommandReturnObject.h>
#include <lldb/API/SBFileSpec.h>
#include <lldb/API/SBFileSpecList.h>
#include <sstream>
#include <iostream>

// Global contract registry
std::map<std::string, ContractInfo> g_contract_registry;
std::vector<std::string> g_call_stack;
std::string g_current_context;

// Helper functions for debugger integration
void UpdateCallStack(const std::string& stack_str) {
  g_call_stack.clear();
  if (stack_str != "main" && !stack_str.empty()) {
    std::stringstream ss(stack_str);
    std::string contract;
    while (std::getline(ss, contract, '>')) {
      // Trim whitespace and arrow
      contract.erase(0, contract.find_first_not_of(" \t\n\r\f\v-"));
      contract.erase(contract.find_last_not_of(" \t\n\r\f\v-") + 1);
      if (!contract.empty() && contract != "main") {
        g_call_stack.push_back(contract);
      }
    }
  }
}

void PushContext(const std::string& contract_address) {
  g_call_stack.push_back(contract_address);
  g_current_context = contract_address;
}

void PopContext() {
  if (!g_call_stack.empty()) {
    g_call_stack.pop_back();
    g_current_context = g_call_stack.empty() ? "" : g_call_stack.back();
  }
}

// Command: "stylus-contract add <address> <library_path>"
bool WalnutContractAddCommand::DoExecute(lldb::SBDebugger debugger, char **command,
                                         lldb::SBCommandReturnObject &result) {
  if (!command || !command[0] || !command[1]) {
    result.Printf("Usage: stylus-contract add <address> <library_path>\n");
    result.SetStatus(lldb::eReturnStatusFailed);
    return false;
  }

  std::string address = command[0];
  std::string library_path = command[1];

  lldb::SBTarget target = debugger.GetSelectedTarget();
  if (!target.IsValid()) {
    result.Printf("No valid target\n");
    result.SetStatus(lldb::eReturnStatusFailed);
    return false;
  }

  // Add the module to the target
  lldb::SBModule module = target.AddModule(library_path.c_str(), nullptr, nullptr);
  if (!module.IsValid()) {
    result.Printf("Failed to load module from: %s\n", library_path.c_str());
    result.SetStatus(lldb::eReturnStatusFailed);
    return false;
  }

  // Store in registry
  ContractInfo info;
  info.library_path = library_path;
  info.module = module;
  g_contract_registry[address] = info;

  result.Printf("Added contract %s with library %s\n", address.c_str(), library_path.c_str());
  result.SetStatus(lldb::eReturnStatusSuccessFinishResult);
  return true;
}

// Command: "stylus-contract breakpoint <address> <function>"
bool WalnutContractBreakpointCommand::DoExecute(lldb::SBDebugger debugger, char **command,
                                                lldb::SBCommandReturnObject &result) {
  if (!command || !command[0] || !command[1]) {
    result.Printf("Usage: stylus-contract breakpoint <address> <function>\n");
    result.SetStatus(lldb::eReturnStatusFailed);
    return false;
  }

  std::string address = command[0];
  std::string function = command[1];

  auto it = g_contract_registry.find(address);
  if (it == g_contract_registry.end()) {
    result.Printf("Contract %s not found. Use 'stylus-contract add' first.\n", address.c_str());
    result.SetStatus(lldb::eReturnStatusFailed);
    return false;
  }

  lldb::SBTarget target = debugger.GetSelectedTarget();
  if (!target.IsValid()) {
    result.Printf("No valid target\n");
    result.SetStatus(lldb::eReturnStatusFailed);
    return false;
  }

  // Create breakpoint specific to the module
  lldb::SBFileSpecList module_list;
  module_list.Append(it->second.module.GetFileSpec());
  
  lldb::SBBreakpoint bp = target.BreakpointCreateByName(
      function.c_str(),
      module_list,
      lldb::SBFileSpecList() // comp_unit_list
  );

  if (!bp.IsValid() || bp.GetNumLocations() == 0) {
    // Try setting a pending breakpoint without module restriction
    bp = target.BreakpointCreateByName(function.c_str());
    
    if (!bp.IsValid()) {
      result.Printf("Warning: Could not set breakpoint on %s in contract %s (function may not exist)\n", 
                    function.c_str(), address.c_str());
      // Return success anyway to allow subsequent commands to run
      result.SetStatus(lldb::eReturnStatusSuccessFinishResult);
      return true;
    }
    
    result.Printf("Warning: Set pending breakpoint on %s for contract %s (will resolve when loaded)\n", 
                  function.c_str(), address.c_str());
  }

  // Store breakpoint reference
  it->second.breakpoints.push_back(bp);

  result.Printf("Set breakpoint on %s in contract %s (ID: %d, %zu locations)\n", 
                function.c_str(), address.c_str(), bp.GetID(), bp.GetNumLocations());
  result.SetStatus(lldb::eReturnStatusSuccessFinishResult);
  return true;
}

// Command: "stylus-contract list"
bool WalnutContractListCommand::DoExecute(lldb::SBDebugger debugger, char **command,
                                          lldb::SBCommandReturnObject &result) {
  if (g_contract_registry.empty()) {
    result.Printf("No contracts registered\n");
  } else {
    result.Printf("Registered contracts:\n");
    for (const auto& [addr, info] : g_contract_registry) {
      result.Printf("  %s -> %s (%zu breakpoints)\n", 
                    addr.c_str(), 
                    info.library_path.c_str(),
                    info.breakpoints.size());
    }
  }
  
  result.SetStatus(lldb::eReturnStatusSuccessFinishResult);
  return true;
}

// Command: "stylus-contract stack"
bool WalnutContractStackCommand::DoExecute(lldb::SBDebugger debugger, char **command,
                                           lldb::SBCommandReturnObject &result) {
  if (g_call_stack.empty()) {
    result.Printf("Call stack: [main]\n");
  } else {
    result.Printf("Call stack: main");
    for (const auto& contract : g_call_stack) {
      result.Printf(" -> %s", contract.c_str());
    }
    result.Printf("\n");
  }
  
  if (!g_current_context.empty()) {
    result.Printf("Current context: %s\n", g_current_context.c_str());
  }
  
  result.SetStatus(lldb::eReturnStatusSuccessFinishResult);
  return true;
}

// Command: "stylus-contract context <address>"
bool WalnutContractContextCommand::DoExecute(lldb::SBDebugger debugger, char **command,
                                             lldb::SBCommandReturnObject &result) {
  if (!command || !command[0]) {
    result.Printf("Usage: stylus-contract context <address>\n");
    result.Printf("       stylus-contract context show\n");
    result.SetStatus(lldb::eReturnStatusFailed);
    return false;
  }

  std::string arg = command[0];
  
  if (arg == "show") {
    if (g_current_context.empty()) {
      result.Printf("Current context: [main]\n");
    } else {
      result.Printf("Current context: %s\n", g_current_context.c_str());
    }
    result.SetStatus(lldb::eReturnStatusSuccessFinishResult);
    return true;
  }

  // Switch to specified context
  std::string address = arg;
  auto it = g_contract_registry.find(address);
  if (it == g_contract_registry.end()) {
    result.Printf("Contract %s not found. Use 'stylus-contract add' first.\n", address.c_str());
    result.SetStatus(lldb::eReturnStatusFailed);
    return false;
  }

  lldb::SBTarget target = debugger.GetSelectedTarget();
  if (!target.IsValid()) {
    result.Printf("No valid target\n");
    result.SetStatus(lldb::eReturnStatusFailed);
    return false;
  }

  // Set the current context
  g_current_context = address;
  
  // Try to focus on the module in the debugger
  lldb::SBModule module = it->second.module;
  if (module.IsValid()) {
    result.Printf("Switched context to contract %s\n", address.c_str());
    result.Printf("Module: %s\n", it->second.library_path.c_str());
  } else {
    result.Printf("Warning: Module for contract %s is not valid\n", address.c_str());
  }

  result.SetStatus(lldb::eReturnStatusSuccessFinishResult);
  return true;
}

// Register stylus-contract commands
bool RegisterWalnutContractCommands(lldb::SBCommandInterpreter &interpreter) {
  // Create multiword command: "stylus-contract"
  lldb::SBCommand contract_cmd = interpreter.AddMultiwordCommand(
      "stylus-contract", "Multi-contract debugging commands for Stylus");
  if (!contract_cmd.IsValid()) {
    std::fprintf(stderr, "Failed to create multiword command 'stylus-contract'\n");
    return false;
  }

  // Subcommand: "stylus-contract add"
  {
    auto *add_iface = new WalnutContractAddCommand();
    lldb::SBCommand add_cmd = contract_cmd.AddCommand(
        "add", add_iface, "Add a contract: stylus-contract add <address> <library_path>");
    if (!add_cmd.IsValid()) {
      std::fprintf(stderr, "Failed to register 'stylus-contract add'\n");
      return false;
    }
  }

  // Subcommand: "stylus-contract breakpoint"
  {
    auto *bp_iface = new WalnutContractBreakpointCommand();
    lldb::SBCommand bp_cmd = contract_cmd.AddCommand(
        "breakpoint", bp_iface, "Set breakpoint: stylus-contract breakpoint <address> <function>");
    if (!bp_cmd.IsValid()) {
      std::fprintf(stderr, "Failed to register 'stylus-contract breakpoint'\n");
      return false;
    }
  }

  // Subcommand: "stylus-contract list"
  {
    auto *list_iface = new WalnutContractListCommand();
    lldb::SBCommand list_cmd = contract_cmd.AddCommand(
        "list", list_iface, "List all contracts: stylus-contract list");
    if (!list_cmd.IsValid()) {
      std::fprintf(stderr, "Failed to register 'stylus-contract list'\n");
      return false;
    }
  }

  // Subcommand: "stylus-contract stack"
  {
    auto *stack_iface = new WalnutContractStackCommand();
    lldb::SBCommand stack_cmd = contract_cmd.AddCommand(
        "stack", stack_iface, "Show call stack: stylus-contract stack");
    if (!stack_cmd.IsValid()) {
      std::fprintf(stderr, "Failed to register 'stylus-contract stack'\n");
      return false;
    }
  }

  // Subcommand: "stylus-contract context"
  {
    auto *context_iface = new WalnutContractContextCommand();
    lldb::SBCommand context_cmd = contract_cmd.AddCommand(
        "context", context_iface, "Switch context: stylus-contract context <address>");
    if (!context_cmd.IsValid()) {
      std::fprintf(stderr, "Failed to register 'stylus-contract context'\n");
      return false;
    }
  }

  return true;
}
