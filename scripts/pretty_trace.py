#!/usr/bin/env python3

import json
import sys
from pathlib import Path
from collections import defaultdict

try:
    import colorama
    from colorama import Fore, Style
except ImportError:
    print("Please install colorama via `pip install colorama`.")
    sys.exit(1)

def build_call_tree(call_list):
    """
    Builds a call tree structure from the flat call list.
    Returns root calls and a mapping from parent to children.
    """
    # Create a mapping from call_id to call frame
    calls = {call['call_id']: call for call in call_list}
    
    # Build parent-child relationships
    tree = defaultdict(list)
    roots = []
    
    for call in call_list:
        parent_id = call['parent_call_id']
        if parent_id == 0:
            roots.append(call)
        else:
            tree[parent_id].append(call)
    
    return roots, tree, calls

def print_call_node(call, tree, level=0, is_last=False, prefix=""):
    """
    Recursively prints a call and its children with proper indentation.
    """
    # Tree visualization
    indent = "    " * level
    branch = "└── " if is_last else "├── "
    new_prefix = "    " if is_last else "│   "
    
    function_name = call.get("function", "<unknown function>")
    file_name = call.get("file", "<no file>")
    line = call.get("line", 0)
    args = call.get("args", [])

    # Print the frame header
    print(
        f"{prefix}{indent}{branch}"
        f"{Fore.GREEN}#{call['call_id']}{Style.RESET_ALL} "
        f"{Fore.YELLOW}{function_name}{Style.RESET_ALL} "
        f"({file_name}:{line})"
    )

    # Print arguments if present
    if args:
        for arg in args:
            arg_name = arg.get("name", "<arg>")
            arg_value = arg.get("value", "<unavailable>")
            print(f"{prefix}{indent}    {Fore.MAGENTA}{arg_name}{Style.RESET_ALL} = {arg_value}")

    # Recursively print children
    children = tree.get(call['call_id'], [])
    for i, child in enumerate(children):
        is_last_child = (i == len(children) - 1)
        print_call_node(child, tree, level + 1, is_last_child, prefix + new_prefix)

def pretty_print_trace(trace_file: Path):
    """
    Reads a JSON array of function call frames from `trace_file`
    and prints them in a hierarchical, colored format.
    """
    if not trace_file.is_file():
        print(f"ERROR: No such file: {trace_file}")
        sys.exit(1)

    with open(trace_file, "r") as f:
        try:
            call_list = json.load(f)
        except json.JSONDecodeError as e:
            print(f"ERROR: Failed to parse JSON: {e}")
            sys.exit(1)

    if not isinstance(call_list, list):
        print("ERROR: JSON root is not a list of frames.")
        sys.exit(1)

    # Print a header
    print(f"{Fore.CYAN}=== WALNUT FUNCTION CALL TREE ==={Style.RESET_ALL}")

    # Build the call tree
    roots, tree, calls = build_call_tree(call_list)

    # Print the tree starting from root calls
    for i, root in enumerate(roots):
        is_last_root = (i == len(roots) - 1)
        print_call_node(root, tree, is_last=is_last_root)

def main():
    colorama.init(autoreset=True)

    # Default to /tmp/lldb_function_trace.json if no arg given
    if len(sys.argv) > 1:
        trace_file = Path(sys.argv[1])
    else:
        trace_file = Path("/tmp/lldb_function_trace.json")

    pretty_print_trace(trace_file)

if __name__ == "__main__":
    main()
