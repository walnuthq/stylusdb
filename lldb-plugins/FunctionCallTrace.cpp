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

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/SmallString.h>
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
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// -----------------------------------------------------------------------------
// Data structures to hold trace info.

// -----------------------------------------------------------------------------

struct CallRecord {
  std::string function;
  std::string file;
  uint32_t line;
  size_t call_id;        // Unique ID for this call
  size_t parent_call_id; // ID of parent call (0 for root)
  // For each argument: (name, value)
  std::vector<std::pair<std::string, std::string>> args;
};

// Thread-local call stack to track hierarchy
struct ThreadCallStack {
  std::vector<size_t> stack;
  size_t next_call_id = 1; // Start with 1 (0 means no parent)
};

static std::mutex g_trace_mutex;
static std::vector<CallRecord> g_trace_data;
// Use thread-specific storage for call stacks
static thread_local ThreadCallStack g_thread_call_stack;

// Decoding functions.

// Try to read a FixedBytes<N> blob and return “0x…” hex.
static bool ExtractFixedBytesAsHex(lldb::SBValue &val, std::string &out_hex) {
  const char *cname = val.GetTypeName();
  if (!cname) return false;
  std::string type_name(cname);

  constexpr char Prefix[] = "alloy_primitives::bits::fixed::FixedBytes<";
  if (type_name.rfind(Prefix, 0) != 0)
    return false;

  auto start = type_name.find('<') + 1;
  auto end   = type_name.find('>');
  int byte_len = std::stoi(type_name.substr(start, end - start));

  std::vector<uint8_t> buf(byte_len);
  lldb::SBError err;
  size_t read_bytes = val.GetData().ReadRawData(err, 0, buf.data(), buf.size());
  if (err.Fail() || read_bytes != buf.size())
    return false;

  std::ostringstream oss;
  oss << "0x" << std::hex << std::setfill('0');
  for (uint8_t b : buf)
    oss << std::setw(2) << (int)b;
  out_hex = oss.str();
  return true;
}

// TODO: Handle I256 as well.
// Try to read a ruint::Uint<BITS,NLIMBS> and return its full decimal value.
static bool ExtractRuintAsDecimal(lldb::SBValue &val, std::string &out_dec) {
  // Get and wrap the C‑string type name
  const char *cname = val.GetTypeName();
  if (!cname)
    return false;
  std::string type_name(cname);

  // Quick‑check prefix
  constexpr char Prefix[] = "ruint::Uint<";
  if (type_name.rfind(Prefix, 0) != 0)
    return false;

  // Parse BITS and NLIMBS from "ruint::Uint<BITS, NLIMBS>"
  auto lt    = type_name.find('<') + 1;
  auto comma = type_name.find(',', lt);
  auto gt    = type_name.find('>', comma);
  int bits    = std::stoi(type_name.substr(lt, comma - lt));
  int limbsN  = std::stoi(type_name.substr(comma + 1, gt - comma - 1));

  // Fetch the "limbs" field
  lldb::SBValue limbs = val.GetChildMemberWithName("limbs");
  if (!limbs.IsValid() || (int)limbs.GetNumChildren() != limbsN)
    return false;

  // Build an APInt of width ‘bits’
  llvm::APInt api(bits, 0);
  for (int i = 0; i < limbsN; ++i) {
    lldb::SBValue elem = limbs.GetChildAtIndex(i);
    uint64_t limb = elem.GetValueAsUnsigned(0);
    llvm::APInt part(64, limb);
    api |= part.zext(bits).shl(64ull * i);
  }

  // Convert to decimal
  llvm::SmallString<128> buffer;
  api.toString(buffer, /*Radix=*/10, /*Signed=*/false);
  out_dec = buffer.c_str();

  return true;
}

/// Try to read a Signed<BITS,NLIMBS> and return its full signed decimal value.
/// Falls back (returns false) if the shape isn’t what we expect.
// Requires these includes at the top of your file:
//   #include <llvm/ADT/APInt.h>
//   #include <llvm/ADT/SmallString.h>

static bool ExtractSintAsDecimal(lldb::SBValue &val, std::string &out_dec) {
  // 1) Get the C-string type name
  const char *cname = val.GetTypeName();
  if (!cname)
    return false; // not a Signed<…> at all

  std::string type_name(cname);
  constexpr char Prefix[] = "alloy_primitives::signed::int::Signed<";
  if (type_name.rfind(Prefix, 0) != 0)
    return false; // not the right template

  // 2) If LLDB itself says “<unavailable>”, respect that immediately:
  if (const char *summary = val.GetSummary()) {
    if (std::strcmp(summary, "<unavailable>") == 0) {
      out_dec = "<unavailable>";
      return true;  // we recognized Signed<…>, so return true
    }
  }

  // 3) Parse BITS and NLIMBS
  auto lt    = type_name.find('<') + 1;
  auto comma = type_name.find(',', lt);
  auto gt    = type_name.find('>', comma);
  int bits   = std::stoi(type_name.substr(lt,      comma - lt));
  int limbsN = std::stoi(type_name.substr(comma+1, gt    - comma - 1));

  // 4) Drill into the inner tuple "__0"
  lldb::SBValue inner = val.GetChildMemberWithName("__0");
  if (!inner.IsValid()) {
    out_dec = "<unavailable>";
    return true;
  }

  // 5) Fetch its "limbs" field
  lldb::SBValue limbs = inner.GetChildMemberWithName("limbs");
  if (!limbs.IsValid() || (int)limbs.GetNumChildren() != limbsN) {
    out_dec = "<unavailable>";
    return true;
  }

  // 6) Reconstruct into an APInt
  llvm::APInt api(bits, 0);
  for (int i = 0; i < limbsN; ++i) {
    lldb::SBValue e = limbs.GetChildAtIndex(i);
    uint64_t limb = e.GetValueAsUnsigned(0);
    llvm::APInt part(64, limb);
    api |= part.zext(bits).shl(static_cast<uint64_t>(64) * i);
  }

  // 7) Format as signed decimal
  llvm::SmallString<128> buffer;
  api.toString(buffer,
               /*Radix=*/10,
               /*Signed=*/true,
               /*formatAsCLiteral=*/false,
               /*UpperCase=*/false,
               /*InsertSeparators=*/false);
  out_dec = buffer.c_str();
  return true;
}

// ----------------------------------------------------------------------------
// Try to read a stylus_sdk::abi::bytes::Bytes and return “0x…” hex.
static bool ExtractBytesAsHex(lldb::SBValue &val, std::string &out_hex) {
  // Grab the Rust type name
  const char *cname = val.GetTypeName();
  if (!cname)
    return false;
  std::string type_name(cname);

  // Quick-check it’s the Bytes struct
  constexpr char Prefix[] = "stylus_sdk::abi::bytes::Bytes";
  if (type_name.rfind(Prefix, 0) != 0)
    return false;

  // Drill into the single child (the size=N array)
  if (val.GetNumChildren() < 1)
    return false;
  lldb::SBValue array = val.GetChildAtIndex(0);

  // Pull out each byte and hex-encode
  uint32_t len = array.GetNumChildren();
  std::ostringstream oss;
  oss << "0x" << std::hex << std::setfill('0');
  for (uint32_t i = 0; i < len; ++i) {
    lldb::SBValue byteVal = array.GetChildAtIndex(i);
    uint64_t b = byteVal.GetValueAsUnsigned(0) & 0xFF;
    oss << std::setw(2) << (unsigned)b;
  }

  out_hex = oss.str();
  return true;
}

// Try to read a &[u8]
static bool ExtractU8SliceAsHex(lldb::SBValue &val, std::string &out_hex) {
  const char *cname = val.GetTypeName();
  if (!cname) return false;
  std::string type_name(cname);
  if (type_name.find("[u8]") == std::string::npos)
    return false;

  uint32_t len = val.GetNumChildren();
  if (len == 0)
    return false;

  std::ostringstream oss;
  oss << "0x" << std::hex << std::setfill('0');
  for (uint32_t i = 0; i < len; ++i) {
    lldb::SBValue byteVal = val.GetChildAtIndex(i);
    uint64_t b = byteVal.GetValueAsUnsigned(0) & 0xFF;
    oss << std::setw(2) << (unsigned)b;
  }

  out_hex = oss.str();
  return true;
}

// Try to read a Vec<u8> and return its contents as 0xFFAA33…
static bool ExtractVecU8AsHex(lldb::SBValue &val, std::string &out_hex) {
  const char *cname = val.GetTypeName();
  if (!cname) return false;
  std::string type_name(cname);

  constexpr char Prefix[] =
    "alloc::vec::Vec<unsigned char, alloc::alloc::Global>";
  if (type_name.rfind(Prefix, 0) != 0)
    return false;

  uint32_t len = val.GetNumChildren();
  if (len == 0)
    return false;

  std::ostringstream oss;
  oss << "0x" << std::hex << std::setfill('0');
  for (uint32_t i = 0; i < len; ++i) {
    lldb::SBValue byteVal = val.GetChildAtIndex(i);
    uint8_t b = byteVal.GetValueAsUnsigned(0) & 0xFF;
    oss << std::setw(2) << (unsigned)b;
  }

  out_hex = oss.str();
  return true;
}

// -----------------------------------------------------------------------------
// Helper: Recursively format an SBValue (structs, arrays, etc.) as a string.

static std::string FormatValueRecursive(lldb::SBValue &val, int depth = 0) {
  if (!val.IsValid()) {
    return "<invalid>";
  }

  // Try to decode special values related to Contracts.
  std::string hex;
  if (ExtractFixedBytesAsHex(val, hex)) {
    if (hex == "0")
      return "<unavailable>";
    return hex;
  }
  std::string dec;
  if (ExtractRuintAsDecimal(val, dec)) {
    if (dec == "0")
      return "<unavailable>";
    return dec;
  }
  std::string sdec;
  if (ExtractSintAsDecimal(val, sdec)) {
    if (sdec == "0")
      return "<unavailable>";
    return sdec;
  }
  std::string bhex;
  if (ExtractBytesAsHex(val, bhex)) {
    if (bhex == "0")
      return "<unavailable>";
    return bhex;
  }
  std::string udata;
  if (ExtractU8SliceAsHex(val, udata)) {
    if (udata == "0")
      return "<unavailable>";
    return udata;
  }
  std::string uvec;
  if (ExtractVecU8AsHex(val, uvec)) {
    if (uvec == "0")
      return "<unavailable>";
    return uvec;
  }

  if (const char *raw_val = val.GetValue()) {
    // Sometimes for complex types, raw_val = "<unavailable>" or nullptr
    if (std::strcmp(raw_val, "<unavailable>") != 0) {
      // If it's not literally "<unavailable>", we can return it
      return std::string(raw_val);
    }
  }

  if (const char *summary = val.GetSummary()) {
    // Summaries can be empty or something like "..." for incomplete data
    if (summary[0] != '\0' && std::strcmp(summary, "<unavailable>") != 0) {
      return std::string(summary);
    }
  }

  // If we have children, build a string from them
  uint32_t num_children = val.GetNumChildren();
  if (num_children > 0) {
    // Collect child fields in JSON-ish format
    std::ostringstream oss;
    oss << val.GetTypeName() << " { ";
    for (uint32_t i = 0; i < num_children; ++i) {
      lldb::SBValue child = val.GetChildAtIndex(i);
      if (!child.IsValid()) continue;

      const char *child_name = child.GetName();
      if (!child_name) child_name = "<anon>";
      oss << child_name << "=" << FormatValueRecursive(child, depth + 1);
      if (i + 1 < num_children) {
        oss << ", ";
      }
    }
    oss << " }";
    return oss.str();
  }

  // We have no value, summary, or children: fallback
  return "<unavailable>";
}

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

  // Gather arguments: enhanced to handle struct/complex types
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

      const char *name = arg.GetName();
      std::string val_str = FormatValueRecursive(arg);

      arg_list.push_back({
          (name ? name : "<anon>"),
          (val_str.empty() ? "<unavailable>" : val_str),
      });
    }
  }

  // Determine parent call ID
  size_t parent_id = 0;
  if (!g_thread_call_stack.stack.empty()) {
    parent_id = g_thread_call_stack.stack.back();
  }

  // Create and store record
  CallRecord rec;
  rec.function = std::move(fn);
  rec.file = std::move(file_name);
  rec.line = line_no;
  rec.args = std::move(arg_list);
  rec.parent_call_id = parent_id;
  rec.call_id = g_thread_call_stack.next_call_id++;

  // Push this call onto the thread's stack FIRST
  g_thread_call_stack.stack.push_back(rec.call_id);

  // Then store the record
  {
    std::lock_guard<std::mutex> lock(g_trace_mutex);
    g_trace_data.push_back(std::move(rec));
  }

  // We need to set another breakpoint for when this function returns
  // to properly track the call stack
  lldb::SBFrame return_frame = thread.GetFrameAtIndex(1);
  if (return_frame.IsValid()) {
    // Get the return address properly
    lldb::SBAddress return_address;
    {
      lldb::addr_t pc = return_frame.GetPC();
      return_address =
          return_frame.GetSymbolContext(lldb::eSymbolContextEverything)
              .GetSymbol()
              .GetStartAddress();
      if (!return_address.IsValid()) {
        // Fallback if symbol lookup fails
        lldb::SBTarget target = process.GetTarget();
        return_address = target.ResolveLoadAddress(pc);
      }
    }

    if (return_address.IsValid()) {
      lldb::SBTarget target = process.GetTarget();
      lldb::SBBreakpoint return_bp = target.BreakpointCreateByAddress(
          return_address.GetLoadAddress(target));
      return_bp.SetOneShot(true);
      return_bp.SetThreadID(thread.GetThreadID());
      return_bp.SetCallback(
          [](void *baton, lldb::SBProcess &proc, lldb::SBThread &thread,
             lldb::SBBreakpointLocation &loc) {
            // Pop the call stack when the function returns
            if (!g_thread_call_stack.stack.empty()) {
              g_thread_call_stack.stack.pop_back();
            }
            return false;
          },
          nullptr);
    }
  }

  return false;
}

//
// Escape a string for safe inclusion in JSON.
//
static std::string JsonEscape(const std::string &s) {
  std::ostringstream oss;
  oss << std::hex; // make sure we print hex for \u00xx

  for (char c : s) {
    switch (c) {
    case '\\':
      oss << "\\\\";
      break;
    case '"':
      oss << "\\\"";
      break;
    case '\b':
      oss << "\\b";
      break;
    case '\f':
      oss << "\\f";
      break;
    case '\n':
      oss << "\\n";
      break;
    case '\r':
      oss << "\\r";
      break;
    case '\t':
      oss << "\\t";
      break;
    default:
      // If outside normal printable range, emit \u00XX
      if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E) {
        oss << "\\u" << std::setw(4) << std::setfill('0')
            << (static_cast<unsigned int>((unsigned char)c) & 0xFF);
      } else {
        oss << c;
      }
      break;
    }
  }

  return oss.str();
}

// -----------------------------------------------------------------------------
// Updated JSON printing to include call hierarchy

static void PrintJSON(lldb::SBCommandReturnObject &result) {
  std::lock_guard<std::mutex> lock(g_trace_mutex);

  result.Printf("[\n");
  for (size_t i = 0; i < g_trace_data.size(); ++i) {
    const auto &r = g_trace_data[i];
    std::string esc_func = JsonEscape(r.function);
    std::string esc_file = JsonEscape(r.file);

    result.Printf("  {\n");
    result.Printf("    \"call_id\": %zu,\n", r.call_id);
    result.Printf("    \"parent_call_id\": %zu,\n", r.parent_call_id);
    result.Printf("    \"function\": \"%s\",\n", esc_func.c_str());
    result.Printf("    \"file\": \"%s\",\n", esc_file.c_str());
    result.Printf("    \"line\": %u,\n", r.line);

    result.Printf("    \"args\": [\n");
    for (size_t j = 0; j < r.args.size(); ++j) {
      const auto &arg = r.args[j];
      std::string esc_name  = JsonEscape(arg.first);
      std::string esc_value = JsonEscape(arg.second);

      result.Printf("      { \"name\": \"%s\", \"value\": \"%s\" }",
                    esc_name.c_str(), esc_value.c_str());
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
    std::string esc_func = JsonEscape(r.function);
    std::string esc_file = JsonEscape(r.file);

    std::fprintf(fp, "  {\n");
    std::fprintf(fp, "    \"call_id\": %zu,\n", r.call_id);
    std::fprintf(fp, "    \"parent_call_id\": %zu,\n", r.parent_call_id);
    std::fprintf(fp, "    \"function\": \"%s\",\n", esc_func.c_str());
    std::fprintf(fp, "    \"file\": \"%s\",\n", esc_file.c_str());
    std::fprintf(fp, "    \"line\": %u,\n", r.line);

    std::fprintf(fp, "    \"args\": [\n");
    for (size_t j = 0; j < r.args.size(); ++j) {
      const auto &arg = r.args[j];
      std::string esc_name  = JsonEscape(arg.first);
      std::string esc_value = JsonEscape(arg.second);

      std::fprintf(fp, "      { \"name\": \"%s\", \"value\": \"%s\" }",
                   esc_name.c_str(), esc_value.c_str());
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
bool CallTraceStartCommand::DoExecute(lldb::SBDebugger debugger, char **command,
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
bool CallTraceStopCommand::DoExecute(lldb::SBDebugger debugger, char **command,
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
  lldb::SBCommand calltrace_cmd = interpreter.AddMultiwordCommand(
      "calltrace", "Function call tracing commands");
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
