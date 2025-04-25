//
// stylusdb
//

#include "FunctionCallTrace.h"

#include <llvm/Support/WithColor.h>
#include <llvm/Support/raw_ostream.h>

#include <lldb/API/SBAddress.h>
#include <lldb/API/SBBreakpoint.h>
#include <lldb/API/SBBreakpointLocation.h>
#include <lldb/API/SBBroadcaster.h>
#include <lldb/API/SBCommandInterpreter.h>
#include <lldb/API/SBCommandReturnObject.h>
#include <lldb/API/SBDebugger.h>
#include <lldb/API/SBEvent.h>
#include <lldb/API/SBFileSpec.h>
#include <lldb/API/SBFrame.h>
#include <lldb/API/SBListener.h>
#include <lldb/API/SBProcess.h>
#include <lldb/API/SBStream.h>
#include <lldb/API/SBTarget.h>
#include <lldb/API/SBThread.h>
#include <lldb/API/SBValue.h>

#define PLUGIN_API __attribute__((used))

namespace lldb {

PLUGIN_API bool PluginInitialize(lldb::SBDebugger debugger) {
  lldb::SBCommandInterpreter interp = debugger.GetCommandInterpreter();
  debugger.SetPrompt("(stylusdb) ");

  if (!RegisterWalnutCommands(interp)) {
    llvm::WithColor::error() << "Failed to register Walnut commands.\n";
    return false;
  }

  // Auto-load formatters directly using the same commands as format-enable
  lldb::SBCommandReturnObject result;
  
  // Add expression-based formatters that work immediately
  interp.HandleCommand("type summary add --summary-string \"${var.limbs[0]}\" "
                       "\"ruint::Uint<256, 4>\"",
                       result);
  interp.HandleCommand("type summary add --summary-string \"${var.limbs[0]}\" "
                       "\"ruint::Uint<128, 2>\"",
                       result);
  interp.HandleCommand("type summary add --summary-string \"${var.limbs[0]}\" "
                       "\"ruint::Uint<64, 1>\"",
                       result);
  interp.HandleCommand("type summary add --summary-string \"${var.limbs[0]}\" "
                       "\"ruint::Uint<32, 1>\"",
                       result);

  // Also add for common type aliases
  interp.HandleCommand("type summary add --summary-string \"${var.limbs[0]}\" "
                       "\"alloy_primitives::aliases::U256\"",
                       result);
  interp.HandleCommand("type summary add --summary-string \"${var.limbs[0]}\" "
                       "\"alloy_primitives::aliases::U128\"",
                       result);
  interp.HandleCommand("type summary add --summary-string \"${var.limbs[0]}\" "
                       "\"alloy_primitives::aliases::U64\"",
                       result);

  llvm::WithColor(llvm::outs(), llvm::HighlightColor::String)
      << "Walnut plugin loaded with contract type formatters.\n";

  return true;
}

} // namespace lldb
