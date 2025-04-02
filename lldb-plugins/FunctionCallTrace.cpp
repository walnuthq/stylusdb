//
// LLDB Plugin: Function Call Tracing
//
// Multiword command: "calltrace"
//   - start [regex] : sets breakpoints on matching functions (or ".*" if
//   omitted)
//   - stop          : prints JSON trace & writes to
//   /tmp/lldb_function_trace.json
//
// by djolertrk
//

#include <cstdio>
#include <lldb/API/LLDB.h>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace lldb;

// -------------------------------------------------------------------------
// Data structure for a single call instance

struct CallRecord {
  std::string function;
  std::string file;
  uint32_t line;
  // For each argument: (name, value)
  std::vector<std::pair<std::string, std::string>> args;

  // For a small integer/pointer return. (Architecture & type dependent.)
  std::string return_value;
};

// We need to track multiple "in-flight" calls if the same function is called
// more than once concurrently. Key them by thread_id + stack pointer, etc.
struct CallInstance {
  CallRecord record;
  // A list of breakpoints we set for each return instruction in the function
  std::vector<SBBreakpoint> return_bps;
};

static std::mutex g_data_mutex;

// Each function entry spawns a new "CallInstance" that we store in a map.
static std::unordered_map<uint64_t, CallInstance> g_active_calls;
// A global "finished" list for calls that have returned:
static std::vector<CallRecord> g_completed_calls;

// A function to find or generate a unique ID for the new call instance
static uint64_t MakeUniqueCallID(SBThread &thread, SBFrame &frame) {
  // For illustration, we combine the thread id and the stack pointer:
  uint64_t tid = thread.GetThreadID();
  uint64_t sp = frame.GetSP();
  return (tid << 32) ^ (sp & 0xffffffff);
}

// -------------------------------------------------------------------------
// Helper: read a small integer return from the correct register.

static std::string GetReturnValueString(SBProcess &process, SBFrame &frame) {
  if (!frame.IsValid())
    return "<unavailable>";

  std::string triple_str;
  if (const char *triple_cstr = process.GetTarget().GetTriple())
    triple_str = triple_cstr;

  bool is_arm64 = (triple_str.find("arm64") != std::string::npos);
  bool is_x86_64 = (triple_str.find("x86_64") != std::string::npos) ||
                   (triple_str.find("x86-64") != std::string::npos);

  // In 64-bit ABIs, an i32 return typically still goes in “the same register,”
  // but the *subregister* is the real 32-bit value. The top bits could be sign
  // extended or leftover. Let's try to read w0/eax first for 32-bit.
  if (is_arm64) {
    // Try w0 first
    SBValue w0_val = frame.FindRegister("w0");
    if (w0_val.IsValid()) {
      uint64_t raw_val = w0_val.GetValueAsUnsigned(0);
      // That is the 32-bit value zero-extended. (For a signed i32, you might
      // cast.)
      std::stringstream ss;
      ss << (int32_t)(raw_val & 0xffffffff);
      return ss.str();
    }
    // Otherwise fallback to x0
    SBValue x0_val = frame.FindRegister("x0");
    if (x0_val.IsValid()) {
      // That’s a 64-bit quantity. Could interpret as pointer or
      // signed/unsigned...
      const char *val_cstr = x0_val.GetValue();
      if (val_cstr)
        return val_cstr;
    }
  } else if (is_x86_64) {
    // Try eax for 32-bit
    SBValue eax_val = frame.FindRegister("eax");
    if (eax_val.IsValid()) {
      uint64_t raw_val = eax_val.GetValueAsUnsigned(0);
      // This is the 32-bit portion.
      // If you suspect a signed 32-bit return, cast to int32_t:
      std::stringstream ss;
      ss << (int32_t)(raw_val & 0xffffffff);
      return ss.str();
    }
    // Otherwise fallback to rax
    SBValue rax_val = frame.FindRegister("rax");
    if (rax_val.IsValid()) {
      const char *val_cstr = rax_val.GetValue();
      if (val_cstr)
        return std::string(val_cstr);
    }
  }

  return "<unavailable>";
}

// -------------------------------------------------------------------------
// Return-breakpoint callback: we are about to "ret" from the function.

static bool ReturnBreakpointHitCallback(void *baton, SBProcess &process,
                                        SBThread &thread,
                                        SBBreakpointLocation &location) {
  uint64_t call_id = reinterpret_cast<uint64_t>(baton);

  // Attempt to find the "call instance" in g_active_calls
  std::lock_guard<std::mutex> lock(g_data_mutex);
  auto it = g_active_calls.find(call_id);
  if (it == g_active_calls.end()) {
    // Not found? This might happen if we have weird re-entrancy
    return false; // continue
  }

  CallInstance &call_inst = it->second;

  // Try to get the return register as a small integer/pointer
  SBFrame frame = thread.GetFrameAtIndex(0);
  call_inst.record.return_value = GetReturnValueString(process, frame);

  // Move this record to g_completed_calls
  g_completed_calls.push_back(std::move(call_inst.record));

  // Cleanup: disable or remove these one-time breakpoints
  for (auto &bp : call_inst.return_bps) {
    bp.SetEnabled(false);
    process.GetTarget().BreakpointDelete(bp.GetID());
  }

  // Erase from active calls
  g_active_calls.erase(it);

  // Return false => auto-continue
  return false;
}

// -------------------------------------------------------------------------
// Function-entry breakpoint callback

static bool FunctionEntryBreakpointCallback(void *baton, SBProcess &process,
                                            SBThread &thread,
                                            SBBreakpointLocation &location) {
  // Gather the call info
  SBFrame frame = thread.GetFrameAtIndex(0);
  if (!frame.IsValid()) {
    return false; // continue
  }

  CallRecord record;
  // Function name
  if (const char *fn = frame.GetFunctionName()) {
    record.function = fn;
  } else {
    record.function = "<unknown>";
  }

  // File & line
  SBLineEntry line_entry = frame.GetLineEntry();
  if (line_entry.IsValid()) {
    record.line = line_entry.GetLine();
    SBFileSpec fs = line_entry.GetFileSpec();
    if (fs.IsValid())
      record.file = fs.GetFilename();
    else
      record.file = "<unknown>";
  }

  // Gather arguments
  SBValueList args = frame.GetVariables(/*args*/ true, /*locals*/ false,
                                        /*statics*/ false, /*in_scope*/ true);
  for (uint32_t i = 0; i < args.GetSize(); i++) {
    SBValue a = args.GetValueAtIndex(i);
    if (!a.IsValid())
      continue;
    const char *nm = a.GetName();
    const char *va = a.GetValue();
    record.args.push_back({nm ? nm : "<anon>", va ? va : "<unavailable>"});
  }

  // Indicate we haven't got the return value yet.
  record.return_value = "<pending>";

  // Next, find all 'ret' instructions in this function and set breakpoints.
  SBFunction sb_func = frame.GetFunction();
  if (!sb_func.IsValid()) {
    // Could not find the function symbol. Just store partial record
    // in the completed list. We won't see the real return value.
    std::lock_guard<std::mutex> lock(g_data_mutex);
    g_completed_calls.push_back(std::move(record));
    return false; // continue
  }

  // The start & end addresses of the function:
  SBAddress start_addr = sb_func.GetStartAddress();
  SBAddress end_addr = sb_func.GetEndAddress();
  // Disassemble that range:
  SBTarget target = process.GetTarget();
  addr_t start_load = start_addr.GetLoadAddress(target);
  addr_t end_load = end_addr.GetLoadAddress(target);

  // If start_load is invalid, fallback:
  if (start_load == LLDB_INVALID_ADDRESS || end_load == LLDB_INVALID_ADDRESS ||
      end_load <= start_load) {
    // Can't do it
    std::lock_guard<std::mutex> lock(g_data_mutex);
    g_completed_calls.push_back(std::move(record));
    return false;
  }

  // Read instructions
  SBInstructionList insts =
      target.ReadInstructions(start_addr, end_load - start_load);
  if (!insts.IsValid() || insts.GetSize() == 0) {
    // can't find instructions
    std::lock_guard<std::mutex> lock(g_data_mutex);
    g_completed_calls.push_back(std::move(record));
    return false;
  }

  // Create a new call instance
  CallInstance call_inst;
  call_inst.record = std::move(record);
  uint64_t call_id = MakeUniqueCallID(thread, frame);

  // Search for instructions that match "ret" (simplistic approach)
  for (uint32_t i = 0; i < insts.GetSize(); i++) {
    SBInstruction insn = insts.GetInstructionAtIndex(i);
    const char *mnemonic = insn.GetMnemonic(target);
    if (!mnemonic)
      continue;
    if (strstr(mnemonic, "ret")) {
      // Set a breakpoint at this instruction address
      SBAddress ret_addr = insn.GetAddress();
      lldb::addr_t load_addr = ret_addr.GetLoadAddress(target);
      if (load_addr == LLDB_INVALID_ADDRESS)
        continue;
      SBBreakpoint ret_bp = target.BreakpointCreateByAddress(load_addr);
      if (ret_bp.IsValid()) {
        // Associate the same call_id baton with each 'ret' breakpoint
        ret_bp.SetCallback(ReturnBreakpointHitCallback,
                           reinterpret_cast<void *>(call_id));
        ret_bp.SetAutoContinue(true);
        call_inst.return_bps.push_back(ret_bp);
      }
    }
  }

  if (call_inst.return_bps.empty()) {
    // No ret instructions found
    std::lock_guard<std::mutex> lock(g_data_mutex);
    call_inst.record.return_value = "<no ret instructions found>";
    g_completed_calls.push_back(std::move(call_inst.record));
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(g_data_mutex);
    g_active_calls[call_id] = std::move(call_inst);
  }

  // Return false => continue
  return false;
}

// -------------------------------------------------------------------------
// "start" command: create a function-entry breakpoint by regex

class CallTraceStartCommand : public SBCommandPluginInterface {
public:
  bool DoExecute(SBDebugger dbg, char **command,
                 SBCommandReturnObject &result) override {
    std::string regex(".*");
    if (command && command[0])
      regex = command[0];

    SBTarget target = dbg.GetSelectedTarget();
    if (!target.IsValid()) {
      result.Printf("No valid target.\n");
      result.SetStatus(eReturnStatusFailed);
      return false;
    }

    // Create an entry-breakpoint for the function
    SBBreakpoint bp = target.BreakpointCreateByRegex(regex.c_str());
    if (!bp.IsValid()) {
      result.Printf("Failed to create breakpoint for '%s'.\n", regex.c_str());
      result.SetStatus(eReturnStatusFailed);
      return false;
    }

    // Attach callback
    bp.SetCallback(FunctionEntryBreakpointCallback, nullptr);
    // Let it auto-continue so it won't stop on entry
    bp.SetAutoContinue(true);

    result.Printf(
        "calltrace: capturing returns via 'ret' breakpoints. Regex = %s\n",
        regex.c_str());
    result.SetStatus(eReturnStatusSuccessFinishResult);
    return true;
  }
};

// -------------------------------------------------------------------------
// Helpers to print JSON

static void PrintJSON(SBCommandReturnObject &res) {
  std::lock_guard<std::mutex> lock(g_data_mutex);

  res.Printf("[\n");
  for (size_t i = 0; i < g_completed_calls.size(); i++) {
    const auto &r = g_completed_calls[i];
    res.Printf("  {\n");
    res.Printf("    \"function\": \"%s\",\n", r.function.c_str());
    res.Printf("    \"file\": \"%s\",\n", r.file.c_str());
    res.Printf("    \"line\": %u,\n", r.line);
    res.Printf("    \"args\": [\n");
    for (size_t j = 0; j < r.args.size(); j++) {
      const auto &pair = r.args[j];
      res.Printf("      { \"name\": \"%s\", \"value\": \"%s\" }",
                 pair.first.c_str(), pair.second.c_str());
      if (j + 1 < r.args.size())
        res.Printf(",");
      res.Printf("\n");
    }
    res.Printf("    ],\n");
    res.Printf("    \"return\": \"%s\"\n", r.return_value.c_str());
    res.Printf("  }");
    if (i + 1 < g_completed_calls.size())
      res.Printf(",");
    res.Printf("\n");
  }
  res.Printf("]\n");
}

static void WriteJSONToFile(const char *path) {
  std::lock_guard<std::mutex> lock(g_data_mutex);
  FILE *fp = fopen(path, "w");
  if (!fp) {
    fprintf(stderr, "Failed to open %s\n", path);
    return;
  }
  fprintf(fp, "[\n");
  for (size_t i = 0; i < g_completed_calls.size(); i++) {
    const auto &r = g_completed_calls[i];
    fprintf(fp, "  {\n");
    fprintf(fp, "    \"function\": \"%s\",\n", r.function.c_str());
    fprintf(fp, "    \"file\": \"%s\",\n", r.file.c_str());
    fprintf(fp, "    \"line\": %u,\n", r.line);
    fprintf(fp, "    \"args\": [\n");
    for (size_t j = 0; j < r.args.size(); j++) {
      const auto &pair = r.args[j];
      fprintf(fp, "      { \"name\": \"%s\", \"value\": \"%s\" }",
              pair.first.c_str(), pair.second.c_str());
      if (j + 1 < r.args.size())
        fprintf(fp, ",");
      fprintf(fp, "\n");
    }
    fprintf(fp, "    ],\n");
    fprintf(fp, "    \"return\": \"%s\"\n", r.return_value.c_str());
    fprintf(fp, "  }");
    if (i + 1 < g_completed_calls.size())
      fprintf(fp, ",");
    fprintf(fp, "\n");
  }
  fprintf(fp, "]\n");
  fclose(fp);
}

// -------------------------------------------------------------------------
// "stop" command: print out everything in g_completed_calls

class CallTraceStopCommand : public SBCommandPluginInterface {
public:
  bool DoExecute(SBDebugger dbg, char **command,
                 SBCommandReturnObject &result) override {
    result.Printf("\n--- LLDB Function Trace (JSON) ---\n");
    PrintJSON(result);
    result.Printf("----------------------------------\n");

    const char *out_path = "/tmp/lldb_function_trace.json";
    WriteJSONToFile(out_path);
    result.Printf("Trace data written to: %s\n", out_path);

    result.SetStatus(eReturnStatusSuccessFinishResult);
    return true;
  }
};

// -------------------------------------------------------------------------
// Plugin entrypoint

bool RegisterWalnutCommands(SBCommandInterpreter &interpreter) {
  SBCommand calltrace_cmd = interpreter.AddMultiwordCommand(
      "calltrace",
      "Function call tracing commands (captures returns via RET breakpoints)");
  if (!calltrace_cmd.IsValid()) {
    fprintf(stderr, "Failed to register calltrace multiword.\n");
    return false;
  }

  {
    auto *start_iface = new CallTraceStartCommand();
    SBCommand start_cmd = calltrace_cmd.AddCommand(
        "start", start_iface,
        "calltrace start [regex].  Captures function entry & returns.");
    if (!start_cmd.IsValid()) {
      fprintf(stderr, "Failed to register 'calltrace start'\n");
      return false;
    }
  }

  {
    auto *stop_iface = new CallTraceStopCommand();
    SBCommand stop_cmd = calltrace_cmd.AddCommand(
        "stop", stop_iface,
        "calltrace stop – prints JSON & writes /tmp/lldb_function_trace.json");
    if (!stop_cmd.IsValid()) {
      fprintf(stderr, "Failed to register 'calltrace stop'\n");
      return false;
    }
  }

  return true;
}
