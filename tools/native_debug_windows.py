"""Exercise emitted PDBs with the Windows debug API and DbgHelp, not a text dump."""
import ctypes as c
from ctypes import wintypes as w
import pathlib


class Symbol(c.Structure):
    _fields_ = [("SizeOfStruct", w.ULONG), ("TypeIndex", w.ULONG), ("Reserved", c.c_uint64 * 2),
                ("Index", w.ULONG), ("Size", w.ULONG), ("ModBase", c.c_uint64), ("Flags", w.ULONG),
                ("Value", c.c_uint64), ("Address", c.c_uint64), ("Register", w.ULONG), ("Scope", w.ULONG),
                ("Tag", w.ULONG), ("NameLen", w.ULONG), ("MaxNameLen", w.ULONG), ("Name", c.c_char * 1)]


class Line(c.Structure):
    _fields_ = [("SizeOfStruct", w.DWORD), ("Key", c.c_void_p), ("LineNumber", w.DWORD),
                ("FileName", c.c_char_p), ("Address", c.c_uint64)]


class StackContext(c.Structure):
    _fields_ = [(name, c.c_uint64) for name in ("InstructionOffset", "ReturnOffset", "FrameOffset", "StackOffset", "BackingStoreOffset", "FuncTableEntry")] + [
        ("Params", c.c_uint64 * 4), ("Reserved", c.c_uint64 * 5), ("Virtual", w.BOOL)]


class ExceptionRecord(c.Structure):
    _fields_ = [("Code", w.DWORD), ("Flags", w.DWORD), ("Record", c.c_void_p), ("Address", c.c_void_p),
                ("ParameterCount", w.DWORD), ("Information", c.c_uint64 * 15)]


class ExceptionInfo(c.Structure):
    _fields_ = [("record", ExceptionRecord), ("firstChance", w.DWORD)]


class CreateInfo(c.Structure):
    _fields_ = [("file", w.HANDLE), ("process", w.HANDLE), ("thread", w.HANDLE), ("base", c.c_void_p),
                ("debugOffset", w.DWORD), ("debugSize", w.DWORD), ("tls", c.c_void_p), ("start", c.c_void_p),
                ("name", c.c_void_p), ("unicode", w.WORD)]


class EventUnion(c.Union):
    _fields_ = [("exception", ExceptionInfo), ("create", CreateInfo), ("exitCode", w.DWORD), ("file", w.HANDLE)]


class Event(c.Structure):
    _fields_ = [("code", w.DWORD), ("process", w.DWORD), ("thread", w.DWORD), ("data", EventUnion)]


class Startup(c.Structure):
    _fields_ = [("cb", w.DWORD), ("reserved", w.LPWSTR), ("desktop", w.LPWSTR), ("title", w.LPWSTR),
                *[(name, w.DWORD) for name in ("x", "y", "xSize", "ySize", "xChars", "yChars", "fill", "flags")],
                ("show", w.WORD), ("reservedSize", w.WORD), ("reserved2", c.c_void_p),
                ("stdin", w.HANDLE), ("stdout", w.HANDLE), ("stderr", w.HANDLE)]


class Process(c.Structure):
    _fields_ = [("process", w.HANDLE), ("thread", w.HANDLE), ("pid", w.DWORD), ("tid", w.DWORD)]


def verify_debugger(executable, source, optimized=False, crash=False):
    expected_lines = [] if crash else [6] if optimized else [3, 6]
    crash_seen = False
    kernel = c.WinDLL("kernel32", use_last_error=True)
    debug = c.WinDLL("dbghelp", use_last_error=True)

    def api(library, name, result, *arguments):
        function = getattr(library, name)
        function.restype = result
        function.argtypes = arguments
        return function

    def check(value, operation):
        if not value:
            raise RuntimeError(f"Windows debug verification: {operation}: {c.WinError(c.get_last_error())}")
        return value

    create = api(kernel, "CreateProcessW", w.BOOL, w.LPCWSTR, w.LPWSTR, c.c_void_p, c.c_void_p, w.BOOL, w.DWORD, c.c_void_p, w.LPCWSTR, c.POINTER(Startup), c.POINTER(Process))
    wait = api(kernel, "WaitForDebugEvent", w.BOOL, c.POINTER(Event), w.DWORD)
    resume = api(kernel, "ContinueDebugEvent", w.BOOL, w.DWORD, w.DWORD, w.DWORD)
    close = api(kernel, "CloseHandle", w.BOOL, w.HANDLE)
    read = api(kernel, "ReadProcessMemory", w.BOOL, w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t, c.POINTER(c.c_size_t))
    write = api(kernel, "WriteProcessMemory", w.BOOL, w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t, c.POINTER(c.c_size_t))
    flush = api(kernel, "FlushInstructionCache", w.BOOL, w.HANDLE, c.c_void_p, c.c_size_t)
    get_context = api(kernel, "GetThreadContext", w.BOOL, w.HANDLE, c.c_void_p)
    set_context = api(kernel, "SetThreadContext", w.BOOL, w.HANDLE, c.c_void_p)
    terminate = api(kernel, "TerminateProcess", w.BOOL, w.HANDLE, w.UINT)
    initialize = api(debug, "SymInitializeW", w.BOOL, w.HANDLE, w.LPCWSTR, w.BOOL)
    cleanup = api(debug, "SymCleanup", w.BOOL, w.HANDLE)
    load = api(debug, "SymLoadModuleExW", c.c_uint64, w.HANDLE, w.HANDLE, w.LPCWSTR, w.LPCWSTR, c.c_uint64, w.DWORD, c.c_void_p, w.DWORD)
    options = api(debug, "SymSetOptions", w.DWORD, w.DWORD)
    line_by_name = api(debug, "SymGetLineFromName64", w.BOOL, w.HANDLE, c.c_char_p, c.c_char_p, w.DWORD, c.POINTER(w.LONG), c.POINTER(Line))
    line_by_address = api(debug, "SymGetLineFromAddr64", w.BOOL, w.HANDLE, c.c_uint64, c.POINTER(w.DWORD), c.POINTER(Line))
    scope = api(debug, "SymSetContext", w.BOOL, w.HANDLE, c.POINTER(StackContext), c.c_void_p)
    callback_type = c.WINFUNCTYPE(w.BOOL, c.POINTER(Symbol), w.ULONG, c.c_void_p)
    enumerate_symbols = api(debug, "SymEnumSymbols", w.BOOL, w.HANDLE, c.c_uint64, c.c_char_p, callback_type, c.c_void_p)
    symbol_at = api(debug, "SymFromAddr", w.BOOL, w.HANDLE, c.c_uint64, c.POINTER(c.c_uint64), c.POINTER(Symbol))
    type_info = api(debug, "SymGetTypeInfo", w.BOOL, w.HANDLE, c.c_uint64, w.ULONG, w.ULONG, c.c_void_p)
    walk = api(debug, "StackWalk64", w.BOOL, w.DWORD, w.HANDLE, w.HANDLE, c.c_void_p, c.c_void_p, c.c_void_p, c.c_void_p, c.c_void_p, c.c_void_p)
    startup, process = Startup(), Process()
    startup.cb = c.sizeof(startup)
    command = c.create_unicode_buffer('"' + str(executable) + '"')
    check(create(str(executable), command, None, None, False, 2, None, str(executable.parent), c.byref(startup), c.byref(process)), "launch debuggee")
    handles = {process.process, process.thread}
    symbols_loaded = False
    exited = False
    hits, steps, breakpoints = [], 0, {}
    stepping = False
    pending = False

    def memory(address, count):
        result = c.create_string_buffer(count)
        transferred = c.c_size_t()
        check(read(process.process, address, result, count, c.byref(transferred)), "read debuggee memory")
        if transferred.value != count:
            raise AssertionError("Partial debuggee memory read")
        return result.raw

    def put(address, data):
        value = c.create_string_buffer(data)
        transferred = c.c_size_t()
        check(write(process.process, address, value, len(data), c.byref(transferred)), "write breakpoint")
        if transferred.value != len(data):
            raise AssertionError("Partial breakpoint write")
        check(flush(process.process, address, len(data)), "flush breakpoint instruction cache")

    def name_at(address):
        storage = c.create_string_buffer(c.sizeof(Symbol) + 1024)
        symbol = c.cast(storage, c.POINTER(Symbol))
        symbol.contents.SizeOfStruct = c.sizeof(Symbol)
        symbol.contents.MaxNameLen = 1024
        displacement = c.c_uint64()
        if not symbol_at(process.process, address, c.byref(displacement), symbol):
            return None
        return c.string_at(c.addressof(symbol.contents) + Symbol.Name.offset, symbol.contents.NameLen).decode()

    def stack_names(context):
        frame = c.create_string_buffer(512)
        rip = c.c_uint64.from_address(context + 248).value
        rbp = c.c_uint64.from_address(context + 160).value
        rsp = c.c_uint64.from_address(context + 152).value
        for offset, address in ((0, rip), (32, rbp), (48, rsp)):
            c.c_uint64.from_buffer(frame, offset).value = address
            c.c_uint32.from_buffer(frame, offset + 12).value = 3
        names = [name_at(rip)]
        copied = c.create_string_buffer(c.string_at(context, 1232))
        for _ in range(8):
            if not walk(0x8664, process.process, process.thread, frame, copied, None,
                        c.cast(debug.SymFunctionTableAccess64, c.c_void_p), c.cast(debug.SymGetModuleBase64, c.c_void_p), None):
                break
            names.append(name_at(c.c_uint64.from_buffer(frame).value))
        return names

    try:
        for _ in range(256):
            event = Event()
            check(wait(c.byref(event), 15000), "wait for debug event")
            pending = True
            status = 0x10002
            if event.code == 3:
                info = event.data.create
                handles.update((info.process, info.thread))
                if info.file:
                    handles.add(info.file)
                options(0x212)
                check(initialize(process.process, str(executable.parent), False), "initialize symbols")
                symbols_loaded = True
                check(load(process.process, info.file, str(executable), None, info.base, 0, None, 0), "load emitted PDB")
                for number in expected_lines:
                    line, displacement = Line(), w.LONG()
                    line.SizeOfStruct = c.sizeof(line)
                    check(line_by_name(process.process, executable.stem.encode(), pathlib.Path(source).as_posix().encode(), number, c.byref(displacement), c.byref(line)), "resolve source breakpoint")
                    if line.LineNumber != number:
                        raise AssertionError("PDB relocated breakpoint onto an unrelated source line")
                    breakpoints[line.Address] = (number, memory(line.Address, 1))
                    put(line.Address, b"\xcc")
            elif event.code == 6 and event.data.file:
                check(close(event.data.file), "close loaded DLL handle")
            elif event.code == 1:
                exception = event.data.exception.record
                if exception.Code in (0x80000003, 0x80000004) or (crash and exception.Code == 0xC000001D):
                    storage = c.create_string_buffer(1248)
                    context = (c.addressof(storage) + 15) & ~15
                    c.c_uint32.from_address(context + 48).value = 0x10000B
                    check(get_context(process.thread, context), "read thread context")
                    rip = c.c_uint64.from_address(context + 248)
                    rsp = c.c_uint64.from_address(context + 152).value
                    rbp = c.c_uint64.from_address(context + 160).value
                    if exception.Code == 0x80000003 and exception.Address in breakpoints:
                        number, original = breakpoints.pop(exception.Address)
                        put(exception.Address, original)
                        rip.value = exception.Address
                        current = StackContext()
                        current.InstructionOffset, current.FrameOffset, current.StackOffset = rip.value, rbp, rsp
                        check(scope(process.process, c.byref(current), None), "select local variable scope")
                        found = {}

                        @callback_type
                        def visit(pointer, size, user):
                            symbol = pointer.contents
                            name = c.string_at(c.addressof(symbol) + Symbol.Name.offset, symbol.NameLen).decode()
                            if symbol.Flags & 0x30:
                                offset = c.c_int64(symbol.Address).value
                                found[name] = (rbp + offset, symbol.ModBase, symbol.TypeIndex)
                            return True

                        check(enumerate_symbols(process.process, 0, None, visit, None), "enumerate PDB locals")
                        expected = {} if optimized else {"value": 41, "result": 42} if number == 3 else {"answer": 42}
                        if optimized and found:
                            raise AssertionError(f"Optimized PDB advertises unproven variable locations: {found}")
                        for name, value in expected.items():
                            if name not in found:
                                raise AssertionError(f"Debugger cannot inspect {name}: {found}")
                            address, module, type_index = found[name]
                            length = c.c_uint64()
                            check(type_info(process.process, module, type_index, 2, c.byref(length)), "inspect debugger-local type")
                            if length.value != 4 or int.from_bytes(memory(address, length.value), "little", signed=True) != value:
                                raise AssertionError(f"Incorrect live debugger value for {name}: {found=}, {rbp=:#x}, size={length.value}, bytes={memory(address, 8).hex()}")
                        if number == 3:
                            names = stack_names(context)
                            if "work" not in names or "main" not in names:
                                raise AssertionError(f"Symbolic Windows stack walk failed: {names}")
                        hits.append(number)
                        c.c_uint32.from_address(context + 68).value |= 0x100
                        check(set_context(process.thread, context), "single step source instruction")
                        stepping = True
                    elif exception.Code == 0x80000004 and stepping:
                        line, displacement = Line(), w.DWORD()
                        line.SizeOfStruct = c.sizeof(line)
                        check(line_by_address(process.process, rip.value, c.byref(displacement), c.byref(line)), "resolve single-step source location")
                        if line.LineNumber != hits[-1]:
                            raise AssertionError("Single-step source mapping diverged from instruction provenance")
                        steps += 1
                        stepping = False
                    elif crash and exception.Code == 0xC000001D:
                        names = stack_names(context)
                        if "crash" not in names or "main" not in names:
                            raise AssertionError(f"Crash stack could not be symbolized: {names}, pc={rip.value:#x}, exception={exception.Address:#x}, image={info.base:#x}, first_chance={event.data.exception.firstChance}")
                        line, displacement = Line(), w.DWORD()
                        line.SizeOfStruct = c.sizeof(line)
                        check(line_by_address(process.process, rip.value, c.byref(displacement), c.byref(line)), "resolve crashing source location")
                        if line.LineNumber != 2:
                            raise AssertionError("Crash debug location does not identify the trapping expression")
                        crash_seen = True
                        status = 0x80010001
                    else:
                        status = 0x10002 if exception.Code == 0x80000003 else 0x80010001
                else:
                    status = 0x80010001
            elif event.code == 5:
                exited = True
                if event.data.exitCode != (0xC000001D if crash else 42) or hits != expected_lines or steps != len(expected_lines) or (crash and not crash_seen):
                    raise AssertionError(f"Debugger outcome mismatch: exit={event.data.exitCode}, {hits=}, {steps=}")
            check(resume(event.process, event.thread, status), "continue debug event")
            pending = False
            if exited:
                return
        raise AssertionError("Debugger event budget exceeded")
    finally:
        if not exited:
            check(terminate(process.process, 1), "terminate failed debuggee")
            if pending:
                check(resume(event.process, event.thread, 0x10002), "release failed debug event")
            for _ in range(64):
                remaining = Event()
                check(wait(c.byref(remaining), 5000), "drain debuggee termination")
                if remaining.code == 6 and remaining.data.file:
                    close(remaining.data.file)
                check(resume(remaining.process, remaining.thread, 0x10002), "continue debuggee termination")
                if remaining.code == 5:
                    break
            else:
                raise AssertionError("Debuggee termination event budget exceeded")
        if symbols_loaded:
            cleanup(process.process)
        for handle in handles:
            if handle:
                close(handle)
