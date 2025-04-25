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
#include <cstddef>
#include <iomanip>
#include <map>
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
  std::vector<size_t> stack;  // Stack of call IDs currently active
  size_t next_call_id = 1; // Start with 1 (0 means no parent)
  std::map<std::string, size_t> active_functions; // Map function base name to its call_id
};

static std::mutex g_trace_mutex;
static std::vector<CallRecord> g_trace_data;
// Use thread-specific storage for call stacks
static thread_local ThreadCallStack g_thread_call_stack;
static thread_local size_t g_base_depth = SIZE_MAX;

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

static std::string ExtractBaseName(const std::string &fn) {
  // Goal: Extract a meaningful identifier from Rust function names
  // Examples:
  //   crate::Module::Struct::method::h123abc -> Struct::method
  //   crate::function::h123abc -> function
  //   Struct::method::h123abc -> Struct::method
  //   function::h123abc -> function
  //   some::module::function -> function (if no hash)

  // First, remove the hash suffix if it exists
  std::string name = fn;
  auto last_sep = name.rfind("::");
  if (last_sep != std::string::npos && last_sep + 2 < name.length()) {
    // Check if what follows :: looks like a hash (h followed by hex)
    if (name[last_sep + 2] == 'h' && last_sep + 3 < name.length()) {
      bool is_hash = true;
      for (size_t i = last_sep + 3; i < name.length() && is_hash; i++) {
        char c = name[i];
        is_hash = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F');
      }
      if (is_hash) {
        // Remove the hash suffix
        name = name.substr(0, last_sep);
      }
    }
  }

  // Now extract the meaningful part
  // Count how many :: separators we have
  std::vector<size_t> separators;
  size_t pos = 0;
  while ((pos = name.find("::", pos)) != std::string::npos) {
    separators.push_back(pos);
    pos += 2;
  }

  if (separators.empty()) {
    // No separators, return as is
    return name;
  }

  // If we have exactly one separator, it might be Struct::method or
  // module::function
  if (separators.size() == 1) {
    // Check what comes before the separator
    std::string first_part = name.substr(0, separators[0]);
    std::string second_part = name.substr(separators[0] + 2);

    // If the first part looks like a crate/module name (contains underscore or
    // all lowercase), just return the second part (the function name)
    bool is_crate_or_module = false;
    if (first_part.find('_') != std::string::npos) {
      is_crate_or_module = true; // Crate names often have underscores
    } else {
      // Check if all lowercase (module) vs has uppercase (Type)
      bool has_upper = false;
      for (char c : first_part) {
        if (c >= 'A' && c <= 'Z') {
          has_upper = true;
          break;
        }
      }
      is_crate_or_module = !has_upper;
    }

    if (is_crate_or_module) {
      // It's crate::function or module::function, return just the function
      return second_part;
    } else {
      // It's Type::method, return both
      return name;
    }
  }

  // For multiple separators, we want the last two components (Type::method)
  // unless the second-to-last looks like a module (lowercase)
  if (separators.size() >= 2) {
    size_t start = separators[separators.size() - 2] + 2;
    std::string last_two = name.substr(start);

    // Check if this looks like Type::method (Type starts with uppercase)
    // or if it's module::function (module is lowercase)
    size_t mid = last_two.find("::");
    if (mid != std::string::npos && mid > 0) {
      char first_char = last_two[0];
      if (first_char >= 'A' && first_char <= 'Z') {
        // Looks like Type::method, keep both parts
        return last_two;
      }
    }

    // Otherwise just return the last component
    return name.substr(separators.back() + 2);
  }

  // Default: return last component after the last ::
  return name.substr(separators.back() + 2);
}

// On each function-entry breakpoint, capture the current function and its args,
// then walk the real LLDB call stack to find the first frame in our crate
// (skipping over ABI/router layers). Extract that caller’s base name and link
// this call to the most recent matching record in g_trace_data. No hard-coded
// names or return-breakpoints needed—purely driven by LLDB’s backtrace.
static bool BreakpointHitCallback(void *baton, lldb::SBProcess &process,
                                  lldb::SBThread &thread,
                                  lldb::SBBreakpointLocation &location) {
  // Grab current frame
  lldb::SBFrame frame = thread.GetFrameAtIndex(0);
  if (!frame.IsValid())
    return false;

  // Extract our full function name
  const char *cfn = frame.GetFunctionName();
  std::string fn = cfn ? cfn : "<unknown>";

  // File & line
  lldb::SBLineEntry le = frame.GetLineEntry();
  std::string file = "<unknown>";
  uint32_t line = 0;
  if (le.IsValid()) {
    line = le.GetLine();
    if (auto fs = le.GetFileSpec(); fs.IsValid())
      file = fs.GetFilename();
  }

  // Gather arguments
  std::vector<std::pair<std::string, std::string>> args;
  auto vars = frame.GetVariables(/*args=*/true, false, false, true);
  for (uint32_t i = 0; i < vars.GetSize(); ++i) {
    auto v = vars.GetValueAtIndex(i);
    if (!v.IsValid())
      continue;
    const char *n = v.GetName();
    std::string val = FormatValueRecursive(v);
    args.emplace_back(n ? n : "<anon>", val.empty() ? "<unavailable>" : val);
  }

  // Compute our crate prefix (contract name)
  std::string crate_prefix;
  if (auto pos = fn.find("::"); pos != std::string::npos)
    crate_prefix = fn.substr(0, pos + 2);

  // Scan down the real backtrace to find the actual caller
  // This might be in the same crate OR a different crate (cross-contract call)
  std::string caller_base;
  std::string caller_full;
  size_t nframes = thread.GetNumFrames();

  // Debug: Print the full call stack
  if (getenv("DEBUG_TRACE")) {
    fprintf(stderr, "[DEBUG] Processing: %s (base: %s)\n", fn.c_str(),
            ExtractBaseName(fn).c_str());
    fprintf(stderr, "[DEBUG] Full call stack (%zu frames):\n", nframes);
    for (uint32_t i = 0; i < nframes && i < 10; ++i) {
      auto f = thread.GetFrameAtIndex(i);
      const char *cf = f.GetFunctionName();
      if (cf) {
        fprintf(stderr, "[DEBUG]   Frame %d: %s (base: %s)\n", i, cf,
                ExtractBaseName(cf).c_str());
      } else {
        fprintf(stderr, "[DEBUG]   Frame %d: <unknown>\n", i);
      }
    }
  }

  for (uint32_t i = 1; i < nframes; ++i) {
    auto f = thread.GetFrameAtIndex(i);
    const char *cf = f.GetFunctionName();
    if (!cf)
      continue;
    std::string s = cf;

    // Skip if it's the same function (recursive call)
    if (s == fn)
      continue;

    // Check if this is a valid Rust function from our contracts
    // (has :: separator and looks like a Rust function)
    if (s.find("::") == std::string::npos)
      continue;

    // Skip system/runtime functions that aren't user code
    if (s.find("std::") == 0 || s.find("core::") == 0 ||
        s.find("alloc::") == 0 || s.find("__rust") != std::string::npos)
      continue;

    // Skip generated router functions - they're implementation details
    // Look for the actual user function that called the router
    if (s.find("as$u20$stylus_sdk..abi..Router") != std::string::npos ||
        ExtractBaseName(s) == "route") {
      if (getenv("DEBUG_TRACE")) {
        fprintf(stderr, "[DEBUG] Skipping router function: %s\n", s.c_str());
      }
      continue; // Skip to find the real caller
    }

    // Found a potential caller - could be same crate or different
    caller_full = s;
    caller_base = ExtractBaseName(s);

    if (getenv("DEBUG_TRACE")) {
      fprintf(stderr, "[DEBUG] Found caller: %s (base: %s)\n",
              caller_full.c_str(), caller_base.c_str());
    }
    break;
  }

  // Determine parent ID
  size_t parent_id = 0;

  // Extract the base name for this function
  std::string fn_base = ExtractBaseName(fn);

  // Check if this function is already being tracked (prevent duplicates)
  auto existing_it = g_thread_call_stack.active_functions.find(fn_base);
  if (existing_it != g_thread_call_stack.active_functions.end()) {
    // This function was already added (probably as a parent for another
    // function) Don't add it again, just update its info if needed
    {
      std::lock_guard<std::mutex> lk(g_trace_mutex);
      for (auto &rec : g_trace_data) {
        if (rec.call_id == existing_it->second) {
          // Update with actual args and info since we now have them
          if (rec.args.empty()) {
            rec.args = std::move(args);
          }
          if (rec.file == "<unknown>") {
            rec.file = file;
            rec.line = line;
          }
          break;
        }
      }
    }
    return false; // Don't process this duplicate
  }

  // Generate call ID for this function
  size_t call_id = g_thread_call_stack.next_call_id++;

  // Find parent from active functions
  if (!caller_base.empty()) {
    // Check if the caller is already tracked as active
    auto it = g_thread_call_stack.active_functions.find(caller_base);
    if (it != g_thread_call_stack.active_functions.end()) {
      parent_id = it->second;
    } else {
      // Caller not yet tracked - only add it if it's from our contract
      // Check if it's actually a function we care about
      if (!crate_prefix.empty() &&
          caller_full.find(crate_prefix) != std::string::npos) {
        // Add just this immediate caller, not the whole stack
        CallRecord caller_rec;
        caller_rec.function = caller_full;
        caller_rec.file = "<unknown>";
        caller_rec.line = 0;

        // Don't assume the caller is root - check if IT has a parent too
        size_t caller_parent_id = 0;
        // Look for the caller's parent in the stack (frame i+1 from the caller)
        for (uint32_t i = 2; i < nframes;
             ++i) { // Start from 2 (skip current and direct caller)
          auto f = thread.GetFrameAtIndex(i);
          const char *cf = f.GetFunctionName();
          if (!cf)
            continue;

          std::string s = cf;
          if (s.find("::") == std::string::npos)
            continue;
          if (s.find("std::") == 0 || s.find("core::") == 0 ||
              s.find("alloc::") == 0 || s.find("__rust") != std::string::npos)
            continue;

          // Check if this is from our crate
          if (s.find(crate_prefix) != std::string::npos) {
            std::string parent_base = ExtractBaseName(s);
            auto parent_it =
                g_thread_call_stack.active_functions.find(parent_base);
            if (parent_it != g_thread_call_stack.active_functions.end()) {
              caller_parent_id = parent_it->second;
            }
          }
          break; // Only check immediate parent of the caller
        }

        caller_rec.parent_call_id = caller_parent_id;
        caller_rec.call_id = g_thread_call_stack.next_call_id++;

        // Try to get line info from the frame
        for (uint32_t i = 1; i < nframes; ++i) {
          auto f = thread.GetFrameAtIndex(i);
          const char *cf = f.GetFunctionName();
          if (cf && ExtractBaseName(cf) == caller_base) {
            lldb::SBLineEntry le = f.GetLineEntry();
            if (le.IsValid()) {
              caller_rec.line = le.GetLine();
              if (auto fs = le.GetFileSpec(); fs.IsValid())
                caller_rec.file = fs.GetFilename();
            }
            break;
          }
        }

        {
          std::lock_guard<std::mutex> lk(g_trace_mutex);
          g_trace_data.push_back(caller_rec);
        }

        g_thread_call_stack.active_functions[caller_base] = caller_rec.call_id;
        parent_id = caller_rec.call_id;

        // Regenerate call_id for current function since we used one
        call_id = g_thread_call_stack.next_call_id++;
      }
    }
  }

  // Track this function as active
  g_thread_call_stack.active_functions[fn_base] = call_id;

  // Emit our record
  CallRecord rec;
  rec.function = std::move(fn);
  rec.file = std::move(file);
  rec.line = line;
  rec.parent_call_id = parent_id;
  rec.call_id = call_id;
  rec.args = std::move(args);

  {
    std::lock_guard<std::mutex> lk(g_trace_mutex);
    g_trace_data.push_back(std::move(rec));
  }

  // No return breakpoints needed
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
// Command "format-enable" - enables pretty printing for contract types
bool FormatEnableCommand::DoExecute(lldb::SBDebugger debugger, char **command,
                                    lldb::SBCommandReturnObject &result) {
  lldb::SBCommandInterpreter ci = debugger.GetCommandInterpreter();

  // First, use expression-based formatters for simple cases
  // These work immediately without Python
  ci.HandleCommand("type summary add --summary-string \"${var.limbs[0]}\" "
                   "\"ruint::Uint<256, 4>\"",
                   result);
  ci.HandleCommand("type summary add --summary-string \"${var.limbs[0]}\" "
                   "\"ruint::Uint<128, 2>\"",
                   result);
  ci.HandleCommand("type summary add --summary-string \"${var.limbs[0]}\" "
                   "\"ruint::Uint<64, 1>\"",
                   result);

  result.Printf("Contract type formatters enabled:\n");
  result.Printf(
      "  - ruint::Uint<256, 4> → shows first limb (values up to 2^64)\n");
  result.Printf("  - ruint::Uint<128, 2> → shows first limb\n");
  result.Printf("  - ruint::Uint<64, 1> → shows value\n");
  result.Printf("\nNote: For values larger than 2^64, only the lower 64 bits "
                "are shown.\n");
  result.Printf(
      "To see the full value, use: expr (unsigned __int128)number.limbs[0] | "
      "((unsigned __int128)number.limbs[1] << 64)\n");

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

  // Add format-enable command
  {
    auto *format_iface = new FormatEnableCommand();
    lldb::SBCommand format_cmd = interpreter.AddCommand(
        "format-enable", format_iface, "Enable pretty printing for contract types");
    if (!format_cmd.IsValid()) {
      std::fprintf(stderr, "Failed to register 'format-enable'\n");
      return false;
    }
  }

  return true;
}
