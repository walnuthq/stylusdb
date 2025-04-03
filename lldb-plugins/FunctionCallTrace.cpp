//
// LLDB Plugin: Function Call Tracing (Corrected for LLDB 19+ API)
//
// Multiword command: "calltrace"
//   - start [regex] : sets breakpoints on matching functions (or ".*" if
//   omitted)
//   - stop          : prints the JSON trace & writes to
//   /tmp/lldb_function_trace.json
//
// by djolertrk
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

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// -----------------------------------------------------------------------------
// Data structures to hold trace info.

struct CallRecord {
  std::string function;
  std::string file;
  uint32_t line;
  // For each argument: (name, value)
  std::vector<std::pair<std::string, std::string>> args;
};

static std::mutex g_trace_mutex;
static std::vector<CallRecord> g_trace_data;

static bool BreakpointHitCallback(void *baton, lldb::SBProcess &process,
                                  lldb::SBThread &thread,
                                  lldb::SBBreakpointLocation &location) {
  // Grab top-most stack frame
  lldb::SBFrame frame = thread.GetFrameAtIndex(0);
  if (!frame.IsValid())
    return false; // continue

  // Function name
  const char *func_name = frame.GetFunctionName();
  std::string fn = func_name ? func_name : "<unknown>";

  // File & line
  std::string file_name("<unknown>");
  uint32_t line_no = 0;
  {
    lldb::SBLineEntry le = frame.GetLineEntry();
    if (le.IsValid()) {
      line_no = le.GetLine();
      lldb::SBFileSpec fs = le.GetFileSpec();
      if (fs.IsValid()) {
        file_name = fs.GetFilename();
      }
    }
  }

  // Gather arguments (works best with -O0 debug builds)
  std::vector<std::pair<std::string, std::string>> arg_list;
  {
    lldb::SBValueList args = frame.GetVariables(/*arguments*/ true,
                                                /*locals*/ false,
                                                /*statics*/ false,
                                                /*in_scope_only*/ true);

    for (uint32_t i = 0; i < args.GetSize(); ++i) {
      lldb::SBValue arg = args.GetValueAtIndex(i);
      if (!arg.IsValid())
        continue;

      const char *name = arg.GetName(); // arg name
      const char *val =
          arg.GetValue(); // arg value (may be nullptr if not available)

      arg_list.push_back(
          {(name ? name : "<anon>"), (val ? val : "<unavailable>")});
    }
  }

  // Store record
  CallRecord rec;
  rec.function = std::move(fn);
  rec.file = std::move(file_name);
  rec.line = line_no;
  rec.args = std::move(arg_list);

  {
    std::lock_guard<std::mutex> lock(g_trace_mutex);
    g_trace_data.push_back(std::move(rec));
  }

  // Return false => automatically continue execution (do not pause).
  return false;
}

// -----------------------------------------------------------------------------
// Helper: print all records as naive JSON to an SBCommandReturnObject.

static void PrintJSON(lldb::SBCommandReturnObject &result) {
  std::lock_guard<std::mutex> lock(g_trace_mutex);

  result.Printf("[\n");
  for (size_t i = 0; i < g_trace_data.size(); ++i) {
    const auto &r = g_trace_data[i];
    result.Printf("  {\n");
    result.Printf("    \"function\": \"%s\",\n", r.function.c_str());
    result.Printf("    \"file\": \"%s\",\n", r.file.c_str());
    result.Printf("    \"line\": %u,\n", r.line);

    // Print arguments as an array
    result.Printf("    \"args\": [\n");
    for (size_t j = 0; j < r.args.size(); ++j) {
      const auto &arg = r.args[j];
      result.Printf("      { \"name\": \"%s\", \"value\": \"%s\" }",
                    arg.first.c_str(), arg.second.c_str());
      if (j + 1 < r.args.size())
        result.Printf(",");
      result.Printf("\n");
    }
    result.Printf("    ]\n");
    result.Printf("  }");
    if (i + 1 < g_trace_data.size())
      result.Printf(",");
    result.Printf("\n");
  }
  result.Printf("]\n");
}

// -----------------------------------------------------------------------------
// Helper: write same JSON to a file (e.g. /tmp/lldb_function_trace.json).

static void WriteJSONToFile(const char *path) {
  std::lock_guard<std::mutex> lock(g_trace_mutex);

  FILE *fp = std::fopen(path, "w");
  if (!fp) {
    std::fprintf(stderr, "Failed to open %s for writing\n", path);
    return;
  }
  std::fprintf(fp, "[\n");

  for (size_t i = 0; i < g_trace_data.size(); ++i) {
    const auto &r = g_trace_data[i];
    std::fprintf(fp, "  {\n");
    std::fprintf(fp, "    \"function\": \"%s\",\n", r.function.c_str());
    std::fprintf(fp, "    \"file\": \"%s\",\n", r.file.c_str());
    std::fprintf(fp, "    \"line\": %u,\n", r.line);

    std::fprintf(fp, "    \"args\": [\n");
    for (size_t j = 0; j < r.args.size(); ++j) {
      const auto &arg = r.args[j];
      std::fprintf(fp, "      { \"name\": \"%s\", \"value\": \"%s\" }",
                   arg.first.c_str(), arg.second.c_str());
      if (j + 1 < r.args.size())
        std::fprintf(fp, ",");
      std::fprintf(fp, "\n");
    }
    std::fprintf(fp, "    ]\n");
    std::fprintf(fp, "  }");
    if (i + 1 < g_trace_data.size())
      std::fprintf(fp, ",");
    std::fprintf(fp, "\n");
  }
  std::fprintf(fp, "]\n");
  std::fclose(fp);
}

// -----------------------------------------------------------------------------
// Subcommand "calltrace start [regex]"
bool CallTraceStartCommand::DoExecute(
    lldb::SBDebugger debugger, char **command,
    lldb::SBCommandReturnObject &result) {
  std::string regex = ".*"; // default
  if (command && command[0])
    regex = command[0];

  lldb::SBCommandInterpreter ci = debugger.GetCommandInterpreter();
  lldb::SBDebugger real_dbg =
      ci.GetDebugger(); // this is usually the main debugger
  lldb::SBTarget target = real_dbg.GetSelectedTarget();

  // lldb::SBTarget target = debugger.GetSelectedTarget();
  if (!target.IsValid()) {
    result.Printf("No valid target. Use `target create <binary>`.\n");
    result.SetStatus(lldb::eReturnStatusFailed);
    return false;
  }

  // Create breakpoint from regex
  lldb::SBBreakpoint bp = target.BreakpointCreateByRegex(regex.c_str());
  if (!bp.IsValid()) {
    result.Printf("Failed to create breakpoint for regex: %s\n", regex.c_str());
    result.SetStatus(lldb::eReturnStatusFailed);
    return false;
  }

  // Set the callback
  bp.SetCallback(BreakpointHitCallback, nullptr);
  bp.SetAutoContinue(true); // do not stop at break

  result.Printf("calltrace: Tracing functions matching '%s'\n", regex.c_str());
  result.Printf("Breakpoint ID: %d\n", bp.GetID());
  result.Printf("Run/continue to collect calls.\n");

  result.SetStatus(lldb::eReturnStatusSuccessFinishResult);
  return true;
}

// -----------------------------------------------------------------------------
// Subcommand "calltrace stop" – prints JSON & writes file
bool CallTraceStopCommand::DoExecute(
    lldb::SBDebugger debugger, char **command,
    lldb::SBCommandReturnObject &result) {
  result.Printf("\n--- LLDB Function Trace (JSON) ---\n");
  PrintJSON(result);
  result.Printf("----------------------------------\n");

  const char *out_path = "/tmp/lldb_function_trace.json";
  WriteJSONToFile(out_path);
  result.Printf("Trace data written to: %s\n", out_path);

  result.SetStatus(lldb::eReturnStatusSuccessFinishResult);
  return true;
}

// -----------------------------------------------------------------------------
// Plugin entry point: register "calltrace" multiword command + subcommands.

bool RegisterWalnutCommands(lldb::SBCommandInterpreter &interpreter) {
  // Create multiword command: "calltrace"
  lldb::SBCommand calltrace_cmd =
      interpreter.AddMultiwordCommand("calltrace", "Function call tracing commands");
  if (!calltrace_cmd.IsValid()) {
    std::fprintf(stderr, "Failed to create multiword command 'calltrace'\n");
    return false;
  }

  // Subcommand: "calltrace start"
  {
    auto *start_iface = new CallTraceStartCommand();
    lldb::SBCommand start_cmd = calltrace_cmd.AddCommand(
        "start", start_iface, "Start tracing: calltrace start [regex]");
    if (!start_cmd.IsValid()) {
      std::fprintf(stderr, "Failed to register 'calltrace start'\n");
      return false;
    }
  }

  // Subcommand: "calltrace stop"
  {
    auto *stop_iface = new CallTraceStopCommand();
    lldb::SBCommand stop_cmd = calltrace_cmd.AddCommand(
        "stop", stop_iface, "Stop tracing & print JSON (calltrace stop).");
    if (!stop_cmd.IsValid()) {
      std::fprintf(stderr, "Failed to register 'calltrace stop'\n");
      return false;
    }
  }

  return true;
}
