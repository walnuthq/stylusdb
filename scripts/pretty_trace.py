#!/usr/bin/env python3
import json, sys
from pathlib import Path
from collections import defaultdict
import re

try:
    import colorama
    from colorama import Fore, Style
except ImportError:
    print("Please install colorama via `pip install colorama`.")
    sys.exit(1)

try:
    import requests
except ImportError:
    print("Please install requests via `pip install requests`.")
    sys.exit(1)

selector_cache = {}

def decode_selector(sel: str) -> str:
    """Decode a function selector (4byte) to its signature using 4byte.directory API"""
    if sel in selector_cache:
        return selector_cache[sel]

    sig = sel
    try:
        r = requests.get(
            "https://www.4byte.directory/api/v1/signatures/",
            params={"hex_signature": sel},
            timeout=2
        )
        r.raise_for_status()
        data = r.json()
        results = data.get("results", [])
        if results and "text_signature" in results[0]:
            sig = results[0]["text_signature"]
    except Exception:
        pass

    selector_cache[sel] = sig
    return sig


def build_call_tree(call_list):
    """Build a tree representation of the function call trace"""
    tree, roots = defaultdict(list), []
    for c in call_list:
        p = c["parent_call_id"]
        if p == 0:
            roots.append(c)
        else:
            tree[p].append(c)
    return roots, tree


def print_sol_node(sol_call, level, is_last, prefix):
    """Print a Solidity call node in the function call tree"""
    pad    = " " * (level * 2)
    branch = "└─ " if is_last else "├─ "
    newp   = prefix + ("  " if is_last else "│ ")
    frm    = sol_call.get("from")
    to     = sol_call.get("to")
    raw    = sol_call.get("input", "")[:10]
    decoded = decode_selector(raw)
    print(
        f"{prefix}{pad}{branch}"
        f"{Fore.CYAN}solidity➤{Style.RESET_ALL} "
        f"{Fore.GREEN}{frm}{Style.RESET_ALL} → {Fore.BLUE}{to}{Style.RESET_ALL} "
        f"(entry_point: {raw} <-> {Fore.MAGENTA}{decoded}{Style.RESET_ALL})"
    )
    for i, ch in enumerate(sol_call.get("calls", [])):
        print_sol_node(ch, level+1, i==len(sol_call["calls"])-1, newp)


def extract_function_name(symbol):
    """Extract just the function name from a fully qualified function name"""
    without_hash = symbol.rsplit("::", 1)[0]
    return without_hash.rsplit("::", 1)[-1]


def extract_arg_names(decoded_func):
    """Extract argument names from a decoded function signature"""
    if "(" not in decoded_func or ")" not in decoded_func:
        return []

    # Extract content between parentheses
    args_str = decoded_func.split("(")[1].split(")")[0]

    # No arguments
    if not args_str:
        return []

    # Split by comma and extract the argument names
    args = []
    for arg in args_str.split(","):
        parts = arg.strip().split(" ")
        if len(parts) >= 2:
            args.append(parts[1])  # The second part is usually the argument name
        else:
            args.append(parts[0])  # If there's only one part, use that

    return args


def get_argument_names(args):
    """Extract argument names from a list of argument objects"""
    return [arg.get("name", "") for arg in args if arg.get("name")]


def matches_argument_pattern(args, sol_call):
    """Check if the function arguments match the Solidity call pattern"""
    # Get the Solidity call details
    input_data = sol_call.get("input", "")

    num_of_sol_args = len(input_data) - 10
    # It should only have "context" and "self"
    num_of_rust_args = len(args) - 2

    # For functions with no arguments, check if any arg contains the target address
    # TODO: Check ABI and encoded hash instead!
    if num_of_sol_args == num_of_rust_args:
        return True
    else:
        return False


def print_call_node(call, tree, sol_function_map, level=0, is_last=False, prefix=""):
    """Print a function call node and its children in the tree"""
    pad    = " " * (level * 2)
    branch = "└─ " if is_last else "├─ "
    newp   = prefix + ("  " if is_last else "│ ")
    fn     = call.get("function","<unknown>")
    fl     = call.get("file","<no file>")
    ln     = call.get("line",0)
    args   = call.get("args", [])
    is_error = call.get("error", False)

    # Error marker and coloring
    error_marker = f" {Fore.RED}✗ ERROR{Style.RESET_ALL}" if is_error else ""
    fn_color = Fore.RED if is_error else Fore.YELLOW

    # Print the function call node
    print(
        f"{prefix}{pad}{branch}"
        f"{Fore.GREEN}#{call['call_id']}{Style.RESET_ALL} "
        f"{fn_color}{fn}{Style.RESET_ALL} "
        f"({fl}:{ln}){error_marker}"
    )

    # Print error message if present
    if is_error and call.get("error_message"):
        print(f"{prefix}{pad}  {Fore.RED}↳ {call['error_message']}{Style.RESET_ALL}")

    # Print function arguments with type info
    for arg in call.get("args", []):
        arg_name = arg.get('name', '<unknown>')
        arg_type = arg.get('type', '')
        arg_value = arg.get('value', '<unavailable>')

        # Simplify common Rust type names for readability
        if arg_type:
            # Shorten common prefixes
            short_type = arg_type.replace('alloy_primitives::bits::address::', '')
            short_type = short_type.replace('alloy_primitives::signed::int::', '')
            short_type = short_type.replace('alloy_primitives::bits::fixed::', '')
            short_type = short_type.replace('ruint::', '')
            short_type = short_type.replace('stylus_sdk::host::', '')
            short_type = short_type.replace('alloc::vec::', '')
            print(f"{prefix}{pad}  {Fore.MAGENTA}{arg_name}{Style.RESET_ALL}: {Fore.CYAN}{short_type}{Style.RESET_ALL} = {arg_value}")
        else:
            print(f"{prefix}{pad}  {Fore.MAGENTA}{arg_name}{Style.RESET_ALL} = {arg_value}")

    dfn = extract_function_name(fn)

    if dfn in sol_function_map:
        sol_call = sol_function_map[dfn]
        # TODO: Check against ABI instead.
        if matches_argument_pattern(args, sol_call):
            print_sol_node(sol_call, level+1, True, newp)

    # Process child nodes
    children = tree.get(call["call_id"], [])
    for i, ch in enumerate(children):
        print_call_node(
            ch, tree, sol_function_map,
            level+1,
            i==len(children)-1,
            newp
        )


def create_sol_function_map(sol_calls):
    """
    Create a mapping from function names to Solidity calls by decoding the selectors
    """
    function_map = {}
    for call in sol_calls:
        input_data = call.get("input", "")
        if input_data and len(input_data) >= 10:  # 0x + 8 hex chars (4 bytes)
            selector = input_data[:10]
            decoded_func = decode_selector(selector)

            # Extract just the function name without parameters
            if "(" in decoded_func:
                function_name = decoded_func.split("(")[0]
                function_map[function_name] = call
                # print(f"Mapped function: {function_name} -> {selector}")

    return function_map


def print_solidity_calls(sol_calls):
    """Print all Solidity calls from the JSON file"""
    print(f"{Fore.CYAN}=== SOLIDITY CALLS FROM JSON ==={Style.RESET_ALL}")

    if not sol_calls:
        print("No Solidity calls found in the JSON file")
        return

    for i, call in enumerate(sol_calls):
        print(f"Call {i+1}:")
        print(f"  From: {call.get('from')}")
        print(f"  To: {call.get('to')}")

        input_data = call.get("input", "")
        selector = input_data[:10] if input_data and len(input_data) >= 10 else "<no input>"
        decoded_selector = decode_selector(selector) if selector != "<no input>" else "<no selector>"

        print(f"  Selector: {selector}")
        print(f"  Decoded: {decoded_selector}")

        # Extract function name for mapping
        function_name = decoded_selector.split("(")[0] if "(" in decoded_selector else selector
        print(f"  Function name: {function_name}")

        # Print any arguments if present
        if input_data and len(input_data) > 10:
            print(f"  Arguments: {input_data[10:]}")

        print(f"  Child calls: {len(call.get('calls', []))}")
        print()


def main():
    colorama.init(autoreset=True)
    if len(sys.argv) < 2:
        print("usage: pretty-print-trace WALNUT_JSON [SOL_JSON]")
        sys.exit(1)

    walnut_file = Path(sys.argv[1])
    if not walnut_file.is_file():
        print(f"ERROR: No such file: {walnut_file}")
        sys.exit(1)

    walnut_json = json.load(open(walnut_file))
    status = walnut_json.get("status", "success")
    walnut = walnut_json.get("calls", [])

    # Print error summary if status is error
    if status == "error":
        error_call = next((c for c in walnut if c.get("error")), None)
        if error_call:
            print(f"{Fore.RED}ERROR: Transaction reverted{Style.RESET_ALL}")
            print(f"  Location: {error_call.get('file', '')}:{error_call.get('line', 0)}")
            print(f"  Function: {Fore.RED}{error_call.get('function', 'unknown')}{Style.RESET_ALL}")
            if error_call.get("error_message"):
                print(f"  Message: {Fore.RED}{error_call['error_message']}{Style.RESET_ALL}")
                
    sol_calls = []
    sol_function_map = {}

    if len(sys.argv) > 2:
        sol_file = Path(sys.argv[2])
        if sol_file.is_file():
            sol_json = json.load(open(sol_file))
            sol_calls = sol_json.get("calls", [])

            # Print detailed information about all Solidity calls
            # This can be used for debugging:
            # print_solidity_calls(sol_calls)

            # Create mapping from function names to Solidity calls
            sol_function_map = create_sol_function_map(sol_calls)
            # This can be used for debugging:
            # print(f"Created function map with {len(sol_function_map)} entries: {list(sol_function_map.keys())}")

    # Build call tree
    roots, tree = build_call_tree(walnut)

    print(f"{Fore.CYAN}=== WALNUT FUNCTION CALL TREE ==={Style.RESET_ALL}")
    for i, root in enumerate(roots):
        print_call_node(root, tree, sol_function_map, 0, i==len(roots)-1, "")

if __name__ == "__main__":
    main()
