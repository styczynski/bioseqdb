import lldb


def print_memory_contexts(debugger, command, result, internal_dict):
    """
    Print a hierarchical summary of all Postgres memory contexts.
    Usage: memory_contexts
    """
    target = debugger.GetSelectedTarget()
    if not target.IsValid():
        result.PutCString("No valid target")
        return

    process = target.GetProcess()
    if not process.IsValid():
        result.PutCString("No valid process")
        return

    # Find TopMemoryContext global variable
    top_context_var = target.FindFirstGlobalVariable("TopMemoryContext")
    if not top_context_var.IsValid():
        result.PutCString("TopMemoryContext not found - is this a Postgres process?")
        return

    result.PutCString(f"Found TopMemoryContext variable: {top_context_var}")

    # Get CurrentMemoryContext for marking
    current_context_var = target.FindFirstGlobalVariable("CurrentMemoryContext")
    current_context_addr = 0
    if current_context_var.IsValid():
        current_context_addr = current_context_var.GetValueAsUnsigned()
        result.PutCString(f"CurrentMemoryContext: 0x{current_context_addr:x}")
    else:
        result.PutCString("CurrentMemoryContext not found")

    top_context_addr = top_context_var.GetValueAsUnsigned()
    result.PutCString(f"TopMemoryContext address: 0x{top_context_addr:x}")

    if top_context_addr == 0:
        result.PutCString("TopMemoryContext is NULL")
        return

    # Try to find the MemoryContext type (it might be a typedef)
    context_type = target.FindFirstType("MemoryContextData")
    result.PutCString("Memory Context Hierarchy:")
    result.PutCString("=" * 50)

    def print_context_tree(context_addr, indent=0):
        if context_addr == 0:
            return

        # Create a value representing the struct at context_addr
        context = target.CreateValueFromAddress("context",
                                                lldb.SBAddress(context_addr, target),
                                                context_type)
        # Get name field properly through type definition
        ident_field = context.GetChildMemberWithName("name")
        if ident_field.IsValid():
            name = ident_field.GetSummary()
            if name:
                name = name.strip('"')  # Remove quotes from string summary
            else:
                name = "<unnamed>"
        else:
            name = "<no name field>"

        # Get name field properly through type definition
        ident_field = context.GetChildMemberWithName("ident")
        if ident_field.IsValid():
            ident = ident_field.GetSummary()
            if name:
                ident = name.strip('"')  # Remove quotes from string summary
            else:
                ident = "<undefintified>"
        else:
            ident = "<no name field>"

        if ident == name:
            ident = ""
        else:
            ident = f"<{ident}>"

        # Mark current context with a star
        marker = " *" if context_addr == current_context_addr else ""

        # Print this context
        indent_str = "  " * indent
        result.PutCString(f"{indent_str}{name} {ident} (0x{context_addr:x}){marker}")

        # Get first child and recurse
        firstchild_field = context.GetChildMemberWithName("firstchild")
        if firstchild_field.IsValid():
            child_addr = firstchild_field.GetValueAsUnsigned()
            if child_addr != 0:
                print_context_tree(child_addr, indent + 1)

        # Get next sibling and recurse at same level
        nextchild_field = context.GetChildMemberWithName("nextchild")
        if nextchild_field.IsValid():
            sibling_addr = nextchild_field.GetValueAsUnsigned()
            if sibling_addr != 0:
                print_context_tree(sibling_addr, indent)

    print_context_tree(top_context_addr)
    result.PutCString("\n* = Current Memory Context")


import lldb
import struct

def print_varlena(debugger, command, result, internal_dict):
    """Print a varlena (variable-length) value with full type decoding"""
    target = debugger.GetSelectedTarget()
    process = target.GetProcess()

    if not command.strip():
        result.PutCString("Usage: varlena <address_or_variable>")
        return

    # Parse the address or variable
    frame = process.GetSelectedThread().GetSelectedFrame()

    try:
        if command.startswith('0x'):
            addr = int(command, 16)
        else:
            expr_result = frame.EvaluateExpression(command)
            if not expr_result.IsValid():
                result.PutCString(f"Expression '{command}' not valid")
                return
            addr = expr_result.GetValueAsUnsigned()
    except:
        result.PutCString(f"Invalid address: {command}")
        return

    if addr == 0:
        result.PutCString("NULL varlena")
        return

    decode_varlena_at_address(target, process, addr, result)


def decode_varlena_at_address(target, process, addr, result):
    """Decode and print varlena structure at given address"""

    # Read the first byte to determine the format
    error = lldb.SBError()
    first_byte = process.ReadMemory(addr, 4, error)
    if not error.Success():
        result.PutCString(f"Failed to read varlena at 0x{addr:x}")
        return

    header_byte = first_byte[3]
    result.PutCString(f"Varlena at 0x{addr:x}:")

    # Determine endianness - assume little endian for now (most common)
    # In little endian: bit 0 determines 4B vs 1B format
    is_4b = (header_byte & 0x01) == 0x00
    is_1b = (header_byte & 0x01) == 0x01
    is_1b_external = header_byte == 0x01

    if is_4b:
        decode_4byte_varlena(process, addr, result)
    elif is_1b_external:
        decode_external_varlena(process, addr, result)
    elif is_1b:
        decode_1byte_varlena(process, addr, result)
    else:
        result.PutCString(f"  Unknown varlena format, header=0x{header_byte:02x}")


def decode_4byte_varlena(process, addr, result):
    """Decode 4-byte header varlena (normal or compressed)"""
    error = lldb.SBError()
    header_data = process.ReadMemory(addr, 4, error)
    if not error.Success() or len(header_data) < 4:
        result.PutCString("  Failed to read 4-byte header")
        return

    header = struct.unpack('<I', header_data)[0]  # Little endian

    # Check compression bit (bit 1 in little endian)
    is_compressed = (header & 0x02) != 0

    if is_compressed:
        # Compressed format
        length = (header >> 2) & 0x3FFFFFFF
        result.PutCString(f"  Type: Compressed 4-byte varlena")
        result.PutCString(f"  Total length: {length} bytes")

        # Read compression info (va_tcinfo)
        tcinfo_data = process.ReadMemory(addr + 4, 4, error)
        if error.Success():
            tcinfo = struct.unpack('<I', tcinfo_data)[0]

            # Extract original size and compression method
            VARLENA_EXTSIZE_BITS = 30
            VARLENA_EXTSIZE_MASK = (1 << VARLENA_EXTSIZE_BITS) - 1

            original_size = tcinfo & VARLENA_EXTSIZE_MASK
            compress_method = tcinfo >> VARLENA_EXTSIZE_BITS

            result.PutCString(f"  Original size: {original_size} bytes")
            result.PutCString(f"  Compression method: {get_compression_method_name(compress_method)}")

            # Read and display compressed data
            data_length = length - 8  # Subtract both headers
            if data_length > 0:
                data = process.ReadMemory(addr + 8, min(data_length, 64), error)
                if error.Success():
                    result.PutCString(f"  Compressed data ({data_length} bytes):")
                    print_hex_dump(data, result, indent=4)

    else:
        # Normal 4-byte format
        length = (header >> 2) & 0x3FFFFFFF
        result.PutCString(f"  Type: Normal 4-byte varlena")
        result.PutCString(f"  Total length: {length} bytes")

        # Read and display data
        data_length = length - 4  # Subtract header
        if data_length > 0:
            data = process.ReadMemory(addr + 4, min(data_length, 1024), error)
            if error.Success():
                result.PutCString(f"  Data ({data_length} bytes):")
                print_varlena_data(data, data_length, result)


def decode_1byte_varlena(process, addr, result):
    """Decode 1-byte header varlena (short format)"""
    error = lldb.SBError()
    header_data = process.ReadMemory(addr, 1, error)
    if not error.Success():
        result.PutCString("  Failed to read 1-byte header")
        return

    header = header_data[0]

    # In little endian: length is in upper 7 bits
    length = (header >> 1) & 0x7F

    result.PutCString(f"  Type: Short 1-byte varlena")
    result.PutCString(f"  Total length: {length} bytes")

    # Read and display data
    data_length = length - 1  # Subtract header
    if data_length > 0:
        data = process.ReadMemory(addr + 1, data_length, error)
        if error.Success():
            result.PutCString(f"  Data ({data_length} bytes):")
            print_varlena_data(data, data_length, result)


def decode_external_varlena(process, addr, result):
    """Decode external TOAST pointer"""
    error = lldb.SBError()

    # Read the tag byte
    tag_data = process.ReadMemory(addr + 1, 1, error)
    if not error.Success():
        result.PutCString("  Failed to read tag byte")
        return

    tag = tag_data[0]

    # VARTAG values from the header
    VARTAG_INDIRECT = 1
    VARTAG_EXPANDED_RO = 2
    VARTAG_EXPANDED_RW = 3
    VARTAG_ONDISK = 18

    tag_name = {
        VARTAG_INDIRECT: "INDIRECT",
        VARTAG_EXPANDED_RO: "EXPANDED_RO",
        VARTAG_EXPANDED_RW: "EXPANDED_RW",
        VARTAG_ONDISK: "ONDISK"
    }.get(tag, f"UNKNOWN({tag})")

    result.PutCString(f"  Type: External TOAST pointer ({tag_name})")

    if tag == VARTAG_ONDISK:
        decode_ondisk_toast(process, addr, result)
    elif tag == VARTAG_INDIRECT:
        decode_indirect_toast(process, addr, result)
    elif tag in (VARTAG_EXPANDED_RO, VARTAG_EXPANDED_RW):
        decode_expanded_toast(process, addr, tag, result)
    else:
        result.PutCString(f"  Unknown TOAST tag: {tag}")


def decode_ondisk_toast(process, addr, result):
    """Decode on-disk TOAST pointer (varatt_external)"""
    error = lldb.SBError()

    # Read the full varatt_external structure (16 bytes after the 2-byte header)
    toast_data = process.ReadMemory(addr + 2, 16, error)
    if not error.Success() or len(toast_data) < 16:
        result.PutCString("  Failed to read TOAST external data")
        return

    # Unpack varatt_external fields
    va_rawsize, va_extinfo, va_valueid, va_toastrelid = struct.unpack('<IIII', toast_data)

    # Extract size and compression info
    VARLENA_EXTSIZE_BITS = 30
    VARLENA_EXTSIZE_MASK = (1 << VARLENA_EXTSIZE_BITS) - 1

    ext_size = va_extinfo & VARLENA_EXTSIZE_MASK
    compress_method = va_extinfo >> VARLENA_EXTSIZE_BITS

    # Check if compressed (external size < raw size - header)
    VARHDRSZ = 4
    is_compressed = ext_size < (va_rawsize - VARHDRSZ)

    result.PutCString(f"  Raw size: {va_rawsize} bytes")
    result.PutCString(f"  External size: {ext_size} bytes")
    result.PutCString(f"  Compressed: {'Yes' if is_compressed else 'No'}")
    if is_compressed:
        result.PutCString(f"  Compression method: {get_compression_method_name(compress_method)}")
    result.PutCString(f"  Value ID: {va_valueid}")
    result.PutCString(f"  TOAST relation OID: {va_toastrelid}")


def decode_indirect_toast(process, addr, result):
    """Decode indirect TOAST pointer (varatt_indirect)"""
    error = lldb.SBError()

    # Read the pointer (8 bytes on 64-bit systems)
    ptr_data = process.ReadMemory(addr + 2, 8, error)
    if not error.Success() or len(ptr_data) < 8:
        result.PutCString("  Failed to read indirect pointer")
        return

    ptr_addr = struct.unpack('<Q', ptr_data)[0]  # 64-bit pointer
    result.PutCString(f"  Points to varlena at: 0x{ptr_addr:x}")

    # Optionally decode the pointed-to varlena
    if ptr_addr != 0:
        result.PutCString("  Pointed-to varlena:")
        decode_varlena_at_address(process.GetTarget(), process, ptr_addr, result, indent=4)


def decode_expanded_toast(process, addr, tag, result):
    """Decode expanded TOAST pointer (varatt_expanded)"""
    error = lldb.SBError()

    # Read the ExpandedObjectHeader pointer (8 bytes on 64-bit systems)
    ptr_data = process.ReadMemory(addr + 2, 8, error)
    if not error.Success() or len(ptr_data) < 8:
        result.PutCString("  Failed to read expanded object header pointer")
        return

    eoh_ptr = struct.unpack('<Q', ptr_data)[0]  # 64-bit pointer

    readonly_str = "read-only" if tag == 2 else "read-write"
    result.PutCString(f"  Expanded object ({readonly_str})")
    result.PutCString(f"  ExpandedObjectHeader at: 0x{eoh_ptr:x}")

    # Could potentially read ExpandedObjectHeader fields here if needed


def get_compression_method_name(method):
    """Convert compression method ID to name"""
    # These constants are typically defined in toast internals
    if method == 0:
        return "None"
    elif method == 1:
        return "PGLZ"
    elif method == 2:
        return "LZ4"
    else:
        return f"Unknown({method})"


def print_varlena_data(data_bytes, length, result):
    """Print varlena data in the most appropriate format"""

    # Try to interpret as UTF-8 text first
    try:
        text = data_bytes.decode('utf-8')
        if all(c.isprintable() or c.isspace() for c in text):
            if len(text) <= 100:
                result.PutCString(f"    Text: \"{text}\"")
            else:
                result.PutCString(f"    Text (first 100 chars): \"{text[:100]}...\"")
            return
    except:
        pass

    # Try to interpret as null-terminated C string
    try:
        null_pos = data_bytes.find(b'\x00')
        if null_pos > 0:
            text = data_bytes[:null_pos].decode('utf-8')
            if all(c.isprintable() or c.isspace() for c in text):
                result.PutCString(f"    C String: \"{text}\"")
                if null_pos + 1 < len(data_bytes):
                    result.PutCString(f"    Remaining {len(data_bytes) - null_pos - 1} bytes after null:")
                    print_hex_dump(data_bytes[null_pos + 1:], result, indent=6)
                return
    except:
        pass

    # Check if it looks like numeric data
    if length == 4:
        val = struct.unpack('<I', data_bytes[:4])[0]
        result.PutCString(f"    Possible int32: {val} (0x{val:x})")
    elif length == 8:
        val = struct.unpack('<Q', data_bytes[:8])[0]
        result.PutCString(f"    Possible int64: {val} (0x{val:x})")

        # Also try as double
        try:
            dval = struct.unpack('<d', data_bytes[:8])[0]
            result.PutCString(f"    Possible double: {dval}")
        except:
            pass

    # Fall back to hex dump
    result.PutCString(f"    Binary data ({length} bytes):")
    print_hex_dump(data_bytes, result, indent=4)


def print_hex_dump(data, result, indent=0):
    """Print data as a formatted hex dump"""
    indent_str = " " * indent

    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        hex_part = ' '.join(f'{b:02x}' for b in chunk)
        ascii_part = ''.join(chr(b) if 32 <= b <= 126 else '.' for b in chunk)
        result.PutCString(f"{indent_str}{i:04x}: {hex_part:<48} |{ascii_part}|")


def __lldb_init_module(debugger, internal_dict):
    debugger.HandleCommand('command script add --overwrite -f lldb.print_memory_contexts memory_contexts')
    result = lldb.SBCommandReturnObject()
    debugger.GetCommandInterpreter().HandleCommand('memory_contexts', result)
    print("Memory contexts command installed.")
    debugger.HandleCommand('command script add --overwrite -f lldb.print_varlena varlena')
    result = lldb.SBCommandReturnObject()
    debugger.GetCommandInterpreter().HandleCommand('varlena', result)
    print("varlena command installed.")

