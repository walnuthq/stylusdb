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
  std::string directory;  // Directory path for full file path
  uint32_t line;
  size_t call_id;        // Unique ID for this call
  size_t parent_call_id; // ID of parent call (0 for root)
  // For each argument: (name, value)
  std::vector<std::pair<std::string, std::string>> args;
  // Error info (set if this call caused an error)
  bool is_error = false;
  std::string error_message;
};

// Thread-local call stack to track hierarchy
struct ThreadCallStack {
  std::vector<size_t> stack;  // Stack of call IDs currently active
  size_t next_call_id = 1; // Start with 1 (0 means no parent)
  std::map<std::string, size_t> active_functions; // Map function base name to its call_id
};

// Global flag to track if we hit a panic breakpoint
static std::atomic<bool> g_panic_detected{false};

// Forward declarations
lldb::SBFrame FindUserFrame(lldb::SBThread &thread);

// Execution status for JSON output
struct ExecutionStatus {
  bool is_error = false;
  std::string error_message;
  std::string error_function; 
  std::string error_file;     
  uint32_t error_line = 0;    
};

static std::mutex g_trace_mutex;
static std::vector<CallRecord> g_trace_data;
static ExecutionStatus g_execution_status;
// Use thread-specific storage for call stacks
static thread_local ThreadCallStack g_thread_call_stack;
static thread_local size_t g_base_depth = SIZE_MAX;

// Decoding functions.

// Try to read a FixedBytes<N> blob and return "0x…" hex.
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

// Try to read an alloy_primitives::bits::address::Address and return "0x…" hex.
static bool ExtractAddressAsHex(lldb::SBValue &val, std::string &out_hex) {
  const char *cname = val.GetTypeName();
  if (!cname) return false;
  std::string type_name(cname);

  constexpr char Prefix[] = "alloy_primitives::bits::address::Address";
  if (type_name.rfind(Prefix, 0) != 0)
    return false;

  // Navigate through the nested structure: Address.0.0
  lldb::SBValue inner = val.GetChildAtIndex(0);
  if (!inner.IsValid()) return false;
  
  lldb::SBValue bytes_field = inner.GetChildAtIndex(0);
  if (!bytes_field.IsValid()) return false;

  // Read the 20 bytes of an Ethereum address
  std::vector<uint8_t> buf(20);
  lldb::SBError err;
  size_t read_bytes = bytes_field.GetData().ReadRawData(err, 0, buf.data(), buf.size());
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
  std::string addr_hex;
  if (ExtractAddressAsHex(val, addr_hex)) {
    if (addr_hex == "0x0000000000000000000000000000000000000000")
      return "<zero address>";
    return addr_hex;
  }
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

// Helper: Read a source line from file and extract error message
static std::string ReadSourceLine(const char *filepath, uint32_t line_num) {
   if (!filepath || line_num == 0) return "";

   FILE *src = std::fopen(filepath, "r");
   if (!src) return "";

   char line_buf[1024];
   uint32_t current_line = 0;
   std::string result;

   while (std::fgets(line_buf, sizeof(line_buf), src)) {
     current_line++;
     if (current_line == line_num) {
       result = line_buf;
       // Trim whitespace
       size_t start = result.find_first_not_of(" \t\n\r");
       size_t end = result.find_last_not_of(" \t\n\r");
       if (start != std::string::npos && end != std::string::npos) {
         result = result.substr(start, end - start + 1);
       }
       break;
     }
   }
   std::fclose(src);

   return result;
}

// Helper: Extract panic message from panic_fmt or assert_failed frame arguments
// The from_str function returns { ptr, ptr } where:
//   - First ptr: points to the string data (the alloc_XXX static string)
//   - Second ptr: metadata pointer (for slices this contains length info)
// However, in practice for simple string panics, we often get:
//   arg0 = pointer to string data
//   arg1 = length or slice metadata
//   arg2 = location pointer

// TODO: Need to be main implementation, currently it does not works
static std::string ExtractPanicMessage(lldb::SBThread &thread, lldb::SBProcess &process) {
  lldb::SBFrame frame0 = thread.GetFrameAtIndex(0);
  if (!frame0.IsValid()) return "";

  const char *fn = frame0.GetFunctionName();
  if (!fn) return "";
  std::string func_name(fn);

  // Get function arguments
  lldb::SBValueList args = frame0.GetVariables(/*arguments=*/true, /*locals=*/false,
                                               /*statics=*/false, /*in_scope_only=*/true);
  
  // Try interpreting first two args as (data_ptr, length) for &str
  if (args.GetSize() >= 2) {
    lldb::SBValue arg0 = args.GetValueAtIndex(0);
    lldb::SBValue arg1 = args.GetValueAtIndex(1);
    
    if (arg0.IsValid() && arg1.IsValid()) {
      uint64_t ptr = arg0.GetValueAsUnsigned(0);
      uint64_t len = arg1.GetValueAsUnsigned(0);
      
      // Sanity check - len should be reasonable for a message (1-4096 chars)
      if (ptr != 0 && len > 0 && len < 4096) {
        std::vector<char> buf(len + 1, 0);
        lldb::SBError err;
        size_t read = process.ReadMemory(ptr, buf.data(), len, err);
        
        if (!err.Fail() && read == len) {
          // Validate it looks like a string (printable ASCII/UTF-8)
          bool valid = true;
          for (size_t i = 0; i < len && valid; ++i) {
            unsigned char c = buf[i];
            // Allow printable ASCII, common control chars, and UTF-8 continuation bytes
            valid = (c >= 0x20 && c < 0x7F) || c == '\n' || c == '\r' || c == '\t' || (c >= 0x80);
          }
          if (valid) {
            std::string msg(buf.data(), len);
            return msg;
          }
        }
      }
    }
  }

  // For fmt::Arguments structure - look for pieces field
  for (uint32_t i = 0; i < args.GetSize(); ++i) {
    lldb::SBValue arg = args.GetValueAtIndex(i);
    if (!arg.IsValid()) continue;

    const char *type_name = arg.GetTypeName();
    if (!type_name) continue;
    std::string type_str(type_name);
    
    if (type_str.find("fmt::Arguments") != std::string::npos) {
      // Arguments has a 'pieces' field which contains the static string parts
      lldb::SBValue pieces = arg.GetChildMemberWithName("pieces");
      if (pieces.IsValid() && pieces.GetNumChildren() > 0) {
        lldb::SBValue piece0 = pieces.GetChildAtIndex(0);
        if (piece0.IsValid()) {
          lldb::SBValue data_ptr = piece0.GetChildMemberWithName("data_ptr");
          lldb::SBValue length = piece0.GetChildMemberWithName("length");
          
          if (!data_ptr.IsValid()) {
            data_ptr = piece0.GetChildAtIndex(0);
            length = piece0.GetChildAtIndex(1);
          }
          
          if (data_ptr.IsValid() && length.IsValid()) {
            uint64_t ptr = data_ptr.GetValueAsUnsigned(0);
            uint64_t len = length.GetValueAsUnsigned(0);
            
            if (ptr != 0 && len > 0 && len < 4096) {
              std::vector<char> buf(len + 1, 0);
              lldb::SBError err;
              size_t read = process.ReadMemory(ptr, buf.data(), len, err);
              if (!err.Fail() && read == len) {
                std::string msg(buf.data(), len);
                return msg;
              }
            }
          }
        }
      }
    }
    
    // Check for direct &str arguments
    if (type_str.find("&str") != std::string::npos || 
        type_str.find("str *") != std::string::npos) {
      lldb::SBValue data_ptr = arg.GetChildAtIndex(0);
      lldb::SBValue length = arg.GetChildAtIndex(1);
      
      if (data_ptr.IsValid() && length.IsValid()) {
        uint64_t ptr = data_ptr.GetValueAsUnsigned(0);
        uint64_t len = length.GetValueAsUnsigned(0);
        
        if (ptr != 0 && len > 0 && len < 4096) {
          std::vector<char> buf(len + 1, 0);
          lldb::SBError err;
          size_t read = process.ReadMemory(ptr, buf.data(), len, err);
          if (!err.Fail() && read == len) {
            std::string msg(buf.data(), len);
            return msg;
          }
        }
      }
    }
  }

  // Try to dereference first arg as pointer to { ptr, len } structure
  // This handles cases where Arguments is passed by reference
  if (args.GetSize() >= 1) {
    lldb::SBValue arg0 = args.GetValueAtIndex(0);
    if (arg0.IsValid()) {
      uint64_t args_ptr = arg0.GetValueAsUnsigned(0);
      
      if (args_ptr != 0) {
        // Read two pointers (for 32-bit Wasm: 2x4 bytes, for 64-bit: 2x8 bytes)
        // Try 32-bit first (Wasm32)
        uint32_t data[2] = {0, 0};
        lldb::SBError err;
        size_t read = process.ReadMemory(args_ptr, data, sizeof(data), err);
        
        if (!err.Fail() && read == sizeof(data)) {
          uint64_t str_ptr = data[0];
          uint64_t str_len = data[1];
          
          if (str_ptr != 0 && str_len > 0 && str_len < 4096) {
            std::vector<char> buf(str_len + 1, 0);
            read = process.ReadMemory(str_ptr, buf.data(), str_len, err);
            if (!err.Fail() && read == str_len) {
              bool valid = true;
              for (size_t i = 0; i < str_len && valid; ++i) {
                unsigned char c = buf[i];
                valid = (c >= 0x20 && c < 0x7F) || c == '\n' || c == '\r' || c == '\t' || (c >= 0x80);
              }
              if (valid) {
                std::string msg(buf.data(), str_len);
                return msg;
              }
            }
          }
        }
      }
    }
  }

  return "";
}

// Callback for panic/assert breakpoints - stops execution and records error
static bool PanicBreakpointCallback(void *baton, lldb::SBProcess &process,
                                    lldb::SBThread &thread,
                                    lldb::SBBreakpointLocation &location) {
  // Only process the first panic hit
  bool expected = false;
  if (!g_panic_detected.compare_exchange_strong(expected, true)) {
    return true; // Already detected, still stop
  }

  lldb::SBFrame user_frame = FindUserFrame(thread);

  std::string panic_file;
  std::string panic_func;
  std::string full_path;
  uint32_t panic_line = 0;

  if (user_frame.IsValid()) {
    const char *fn = user_frame.GetFunctionName();
    if (fn) panic_func = fn;

    lldb::SBLineEntry le = user_frame.GetLineEntry();
    if (le.IsValid()) {
      panic_line = le.GetLine();
      lldb::SBFileSpec fs = le.GetFileSpec();
      if (fs.IsValid()) {
        const char *filename = fs.GetFilename();
        const char *dir = fs.GetDirectory();
        if (filename) panic_file = filename;
        if (dir && filename) {
          full_path = std::string(dir) + "/" + filename;
        }
      }
    }
  }

  // Try to extract the actual panic message from frame arguments/memory
  // THE FOLLWOING FUNCTION NEED TO BE FIXED
  std::string panic_msg = ExtractPanicMessage(thread, process);
  
  // Fallback to source line if we couldn't extract the message
  if (panic_msg.empty() && !full_path.empty() && panic_line > 0) {
    std::string source_line = ReadSourceLine(full_path.c_str(), panic_line);
    if (!source_line.empty()) {
      // Try to extract just the message from assert! macro if present
      // Pattern: assert!(condition, "message") -> extract "message"
      size_t quote_start = source_line.find('"');
      size_t quote_end = source_line.rfind('"');
      if (quote_start != std::string::npos && quote_end != std::string::npos && quote_end > quote_start) {
        panic_msg = source_line.substr(quote_start + 1, quote_end - quote_start - 1);
      } else {
        panic_msg = source_line;
      }
    }
  }

   // Update global execution status
   std::lock_guard<std::mutex> lk(g_trace_mutex);
   g_execution_status.is_error = true;
   g_execution_status.error_message = panic_msg.empty() ? "Rust panic/assert" : panic_msg;
   g_execution_status.error_file = panic_file;
   g_execution_status.error_line = panic_line;
   g_execution_status.error_function = panic_func;

   return true; // Stop execution
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
  std::string directory;
  uint32_t line = 0;
  if (le.IsValid()) {
    line = le.GetLine();
    if (auto fs = le.GetFileSpec(); fs.IsValid()) {
      if (fs.GetFilename()) file = fs.GetFilename();
      if (fs.GetDirectory()) directory = fs.GetDirectory();
    }
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
              if (auto fs = le.GetFileSpec(); fs.IsValid()) {
                if (fs.GetFilename()) caller_rec.file = fs.GetFilename();
                if (fs.GetDirectory()) caller_rec.directory = fs.GetDirectory();
              }
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
  rec.directory = std::move(directory);
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

// Helper: find first user frame
lldb::SBFrame FindUserFrame(lldb::SBThread &thread) {
    uint32_t n = thread.GetNumFrames();

    for (uint32_t i = 0; i < n; i++) {
        lldb::SBFrame frame = thread.GetFrameAtIndex(i);
        if (!frame.IsValid()) continue;

        // Check function name - skip core/std/alloc functions
        const char *fn = frame.GetFunctionName();
        if (fn) {
            std::string func(fn);
            if (func.find("core::") == 0 ||
                func.find("std::") == 0 ||
                func.find("alloc::") == 0 ||
                func.find("__rust") != std::string::npos) {
                continue;
            }
        }

        lldb::SBLineEntry line = frame.GetLineEntry();
        if (!line.IsValid()) continue;

        lldb::SBFileSpec fs = line.GetFileSpec();
        if (!fs.IsValid()) continue;

        const char *filename = fs.GetFilename();
        const char *directory = fs.GetDirectory();

        // Skip rust standard library files
        std::string full_path;
        if (directory) full_path = directory;
        if (filename) {
            if (!full_path.empty()) full_path += "/";
            full_path += filename;
        }

        if (full_path.find("/rustc/") != std::string::npos ||
            full_path.find("/library/core/") != std::string::npos ||
            full_path.find("/library/std/") != std::string::npos ||
            full_path.find("/library/alloc/") != std::string::npos) {
            continue;
        }

        // Found a user frame
        return frame;
    }

    return lldb::SBFrame();
}


// Detect Rust or C/C++ assert/panic
bool IsAssertOrPanic(lldb::SBThread &thread, ExecutionStatus &status) {
    uint32_t num_frames = thread.GetNumFrames();

    for (uint32_t i = 0; i < num_frames; i++) {
        lldb::SBFrame frame = thread.GetFrameAtIndex(i);
        if (!frame.IsValid()) continue;

        lldb::SBSymbolContext sc = frame.GetSymbolContext(lldb::eSymbolContextFunction);
        lldb::SBFunction func = sc.GetFunction();

        const char *name = nullptr;

        if (func.IsValid()) {
            name = func.GetName();
        } else {
            lldb::SBSymbol symbol = frame.GetSymbol();
            if (symbol.IsValid()) {
                name = symbol.GetName();
            }
        }

        if (!name) continue;
        std::string fn(name);

        // Rust assert/panic
        if (fn.find("core::panicking::assert_failed") != std::string::npos ||
            fn.find("core::panicking::panic_fmt") != std::string::npos ||
            fn.find("rust_begin_unwind") != std::string::npos) {

            status.is_error = true;

            // Use the last traced call as error location
            {
                std::lock_guard<std::mutex> lk(g_trace_mutex);
                if (!g_trace_data.empty()) {
                    const CallRecord &last_call = g_trace_data.back();
                    status.error_file = last_call.file;
                    status.error_line = last_call.line;
                    status.error_function = last_call.function;

                    // Read source line as error message
                    if (!last_call.directory.empty() && !last_call.file.empty() && last_call.line > 0) {
                        std::string full_path = last_call.directory + "/" + last_call.file;
                        std::string source_line = ReadSourceLine(full_path.c_str(), last_call.line);
                        if (!source_line.empty()) {
                            status.error_message = source_line;
                        }
                    }

                    if (status.error_message.empty()) {
                        status.error_message = "Panic in " + ExtractBaseName(last_call.function);
                    }
                }
            }

            if (status.error_message.empty()) {
                status.error_message = "Rust panic/assert detected";
            }

            return true;
        }

        // C/C++ assert (macOS) or Rust abort
        if (fn.find("__assert_rtn") != std::string::npos ||
            fn.find("__assert_fail") != std::string::npos ||
            fn.find("abort") != std::string::npos ||
            fn.find("__builtin_trap") != std::string::npos) {

            status.is_error = true;

            // Use the last traced call as error location (it's closest to where panic occurred)
            {
                std::lock_guard<std::mutex> lk(g_trace_mutex);
                if (!g_trace_data.empty()) {
                    const CallRecord &last_call = g_trace_data.back();
                    status.error_file = last_call.file;
                    status.error_line = last_call.line;
                    status.error_function = last_call.function;

                    // Read source line as error message
                    if (!last_call.directory.empty() && !last_call.file.empty() && last_call.line > 0) {
                        std::string full_path = last_call.directory + "/" + last_call.file;
                        std::string source_line = ReadSourceLine(full_path.c_str(), last_call.line);
                        if (!source_line.empty()) {
                            status.error_message = source_line;
                        }
                    }

                    if (status.error_message.empty()) {
                        status.error_message = "Panic in " + ExtractBaseName(last_call.function);
                    }
                }
            }

            if (status.error_message.empty()) {
                status.error_message = "Assert or abort detected";
            }

            return true;
        }
    }

    return false;
}

// Main function: get execution status
ExecutionStatus GetExecutionStatus(lldb::SBDebugger &debugger) {
    // If we already detected a panic via breakpoint, use that status
    if (g_panic_detected.load()) {
        std::lock_guard<std::mutex> lk(g_trace_mutex);
        return g_execution_status;
    }
    ExecutionStatus status;

    lldb::SBTarget target = debugger.GetSelectedTarget();
    if (!target.IsValid()) return status;

    lldb::SBProcess process = target.GetProcess();
    if (!process.IsValid()) return status;

    lldb::StateType state = process.GetState();

    // Process exited or crashed
    if (state == lldb::eStateCrashed || state == lldb::eStateExited) {
        int exit_status = process.GetExitStatus();
        if (exit_status != 0) {
            status.is_error = true;
            status.error_message = "Process exited with status " + std::to_string(exit_status);
        }
    }

    // Process stopped (maybe panic/assert)
    if (state == lldb::eStateStopped) {
        lldb::SBThread thread = process.GetSelectedThread();
        if (thread.IsValid()) {
            lldb::StopReason stop_reason = thread.GetStopReason();

            if (stop_reason == lldb::eStopReasonSignal) {
                uint64_t signal_num = thread.GetStopReasonDataAtIndex(0);
                if (signal_num == 6) { // SIGABRT
                    IsAssertOrPanic(thread, status);
                } else {
                    status.is_error = true;
                    status.error_message = "Stopped by signal " + std::to_string(signal_num);
                }
            } else if (stop_reason == lldb::eStopReasonException) {
                status.is_error = true;
                status.error_message = "Exception occurred";
            } else if (stop_reason == lldb::eStopReasonBreakpoint) {
                IsAssertOrPanic(thread, status);
            }
        }
    }

    return status;
}

// -----------------------------------------------------------------------------
// Updated JSON printing to include call hierarchy and status

// Helper to check if a call record matches the error location
static bool IsErrorCall(const CallRecord &r, const ExecutionStatus &exec_status) {
  if (!exec_status.is_error) return false;

  // Match by file and line if available
  if (!exec_status.error_file.empty() && exec_status.error_line > 0) {
    if (r.file == exec_status.error_file && r.line == exec_status.error_line) {
      return true;
    }
  }

  // Match by function name (partial match since function names may have hash suffixes)
  if (!exec_status.error_function.empty()) {
    if (r.function.find(exec_status.error_function) != std::string::npos ||
        exec_status.error_function.find(r.function) != std::string::npos) {
      return true;
    }
    // Also try matching the base function name (without module path)
    size_t last_colon = r.function.rfind("::");
    if (last_colon != std::string::npos) {
      std::string base_name = r.function.substr(last_colon + 2);
      // Remove hash suffix if present
      size_t hash_pos = base_name.rfind("::h");
      if (hash_pos != std::string::npos) {
        base_name = base_name.substr(0, hash_pos);
      }
      if (exec_status.error_function.find(base_name) != std::string::npos) {
        return true;
      }
    }
  }

  return false;
}

static void PrintJSON(lldb::SBCommandReturnObject &result, const ExecutionStatus &exec_status) {
  std::lock_guard<std::mutex> lock(g_trace_mutex);

  result.Printf("{\n");
  result.Printf("  \"status\": \"%s\",\n", exec_status.is_error ? "error" : "success");
  result.Printf("  \"calls\": [\n");

  // Find which call is the error call (last matching call, since errors bubble up)
  size_t error_call_idx = SIZE_MAX;
  if (exec_status.is_error) {
    for (size_t i = g_trace_data.size(); i > 0; --i) {
      if (IsErrorCall(g_trace_data[i - 1], exec_status)) {
        error_call_idx = i - 1;
        break;
      }
    }
    // If no match found, mark the last call as the error
    if (error_call_idx == SIZE_MAX && !g_trace_data.empty()) {
      error_call_idx = g_trace_data.size() - 1;
    }
  }

  for (size_t i = 0; i < g_trace_data.size(); ++i) {
    const auto &r = g_trace_data[i];
    std::string esc_func = JsonEscape(r.function);
    std::string esc_file = JsonEscape(r.file);
    bool is_error_call = (i == error_call_idx);

    result.Printf("    {\n");
    result.Printf("      \"call_id\": %zu,\n", r.call_id);
    result.Printf("      \"parent_call_id\": %zu,\n", r.parent_call_id);
    result.Printf("      \"function\": \"%s\",\n", esc_func.c_str());
    result.Printf("      \"file\": \"%s\",\n", esc_file.c_str());
    result.Printf("      \"line\": %u,\n", r.line);

    result.Printf("      \"args\": [\n");
    for (size_t j = 0; j < r.args.size(); ++j) {
      const auto &arg = r.args[j];
      std::string esc_name  = JsonEscape(arg.first);
      std::string esc_value = JsonEscape(arg.second);

      result.Printf("        { \"name\": \"%s\", \"value\": \"%s\" }",
                    esc_name.c_str(), esc_value.c_str());
      if (j + 1 < r.args.size())
        result.Printf(",");
      result.Printf("\n");
    }
    result.Printf("      ]");

    // Add error info if this is the error call
    if (is_error_call) {
      result.Printf(",\n");
      result.Printf("      \"error\": true,\n");
      std::string esc_msg = JsonEscape(exec_status.error_message);
      result.Printf("      \"error_message\": \"%s\"\n", esc_msg.c_str());
    } else {
      result.Printf("\n");
    }

    result.Printf("    }");
    if (i + 1 < g_trace_data.size())
      result.Printf(",");
    result.Printf("\n");
  }
  result.Printf("  ]\n");
  result.Printf("}\n");
}

// -----------------------------------------------------------------------------
// Helper: write same JSON to a file (e.g. /tmp/lldb_function_trace.json).

static void WriteJSONToFile(const char *path, const ExecutionStatus &exec_status) {
  std::lock_guard<std::mutex> lock(g_trace_mutex);

  FILE *fp = std::fopen(path, "w");
  if (!fp) {
    std::fprintf(stderr, "Failed to open %s for writing\n", path);
    return;
  }

  std::fprintf(fp, "{\n");
  std::fprintf(fp, "  \"status\": \"%s\",\n", exec_status.is_error ? "error" : "success");
  std::fprintf(fp, "  \"calls\": [\n");

  // Find which call is the error call (last matching call)
  size_t error_call_idx = SIZE_MAX;
  if (exec_status.is_error) {
    for (size_t i = g_trace_data.size(); i > 0; --i) {
      if (IsErrorCall(g_trace_data[i - 1], exec_status)) {
        error_call_idx = i - 1;
        break;
      }
    }
    // If no match found, mark the last call as the error
    if (error_call_idx == SIZE_MAX && !g_trace_data.empty()) {
      error_call_idx = g_trace_data.size() - 1;
    }
  }

  for (size_t i = 0; i < g_trace_data.size(); ++i) {
    const auto &r = g_trace_data[i];
    std::string esc_func = JsonEscape(r.function);
    std::string esc_file = JsonEscape(r.file);
    bool is_error_call = (i == error_call_idx);

    std::fprintf(fp, "    {\n");
    std::fprintf(fp, "      \"call_id\": %zu,\n", r.call_id);
    std::fprintf(fp, "      \"parent_call_id\": %zu,\n", r.parent_call_id);
    std::fprintf(fp, "      \"function\": \"%s\",\n", esc_func.c_str());
    std::fprintf(fp, "      \"file\": \"%s\",\n", esc_file.c_str());
    std::fprintf(fp, "      \"line\": %u,\n", r.line);

    std::fprintf(fp, "      \"args\": [\n");
    for (size_t j = 0; j < r.args.size(); ++j) {
      const auto &arg = r.args[j];
      std::string esc_name  = JsonEscape(arg.first);
      std::string esc_value = JsonEscape(arg.second);

      std::fprintf(fp, "        { \"name\": \"%s\", \"value\": \"%s\" }",
                   esc_name.c_str(), esc_value.c_str());
      if (j + 1 < r.args.size())
        std::fprintf(fp, ",");
      std::fprintf(fp, "\n");
    }
    std::fprintf(fp, "      ]");

    // Add error info if this is the error call
    if (is_error_call) {
      std::fprintf(fp, ",\n");
      std::fprintf(fp, "      \"error\": true,\n");
      std::string esc_msg = JsonEscape(exec_status.error_message);
      std::fprintf(fp, "      \"error_message\": \"%s\"\n", esc_msg.c_str());
    } else {
      std::fprintf(fp, "\n");
    }

    std::fprintf(fp, "    }");
    if (i + 1 < g_trace_data.size())
      std::fprintf(fp, ",");
    std::fprintf(fp, "\n");
  }
  std::fprintf(fp, "  ]\n");
  std::fprintf(fp, "}\n");
  std::fclose(fp);
}

// -----------------------------------------------------------------------------
// Subcommand "calltrace start [regex]"
bool CallTraceStartCommand::DoExecute(lldb::SBDebugger debugger, char **command,
                                      lldb::SBCommandReturnObject &result) {
  // Clear previous trace data
  {
    std::lock_guard<std::mutex> lock(g_trace_mutex);
    g_trace_data.clear();
    g_execution_status = ExecutionStatus(); // Reset to success state
  }
  g_panic_detected.store(false);

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
  // Get execution status (detect panics/crashes)
  ExecutionStatus exec_status = GetExecutionStatus(debugger);

  result.Printf("\n--- LLDB Function Trace (JSON) ---\n");
  PrintJSON(result, exec_status);
  result.Printf("----------------------------------\n");

  const char *out_path = "/tmp/lldb_function_trace.json";
  WriteJSONToFile(out_path, exec_status);
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
  
  ci.HandleCommand("script import lldb", result);
  ci.HandleCommand("script def format_address(valobj, internal_dict):\n"
                   "    try:\n"
                   "        inner = valobj.GetChildAtIndex(0)\n"
                   "        if not inner:\n"
                   "            return '0x0000000000000000000000000000000000000000'\n"
                   "        bytes_field = inner.GetChildAtIndex(0)\n"
                   "        if not bytes_field:\n"
                   "            return '0x0000000000000000000000000000000000000000'\n"
                   "        error = lldb.SBError()\n"
                   "        data = bytes_field.GetData()\n"
                   "        if not data:\n"
                   "            return '0x0000000000000000000000000000000000000000'\n"
                   "        bytes_raw = data.ReadRawData(error, 0, 20)\n"
                   "        if error.Fail() or not bytes_raw:\n"
                   "            return '0x0000000000000000000000000000000000000000'\n"
                   "        return '0x' + ''.join(format(b, '02x') for b in bytes_raw)\n"
                   "    except:\n"
                   "        return '0x0000000000000000000000000000000000000000'",
                   result);
  
  // Register the formatter
  ci.HandleCommand("type summary add -F format_address "
                   "\"alloy_primitives::bits::address::Address\"",
                   result);

  result.Printf("Contract type formatters enabled\n");

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
