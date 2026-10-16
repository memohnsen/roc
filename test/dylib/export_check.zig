//! Verifies that an ELF shared library's dynamic symbol table defines exactly
//! the expected exports. ELF linkers export every default-visibility
//! definition unless told otherwise, so this catches compiler builtins and
//! other support code leaking into (and becoming interposable through) a Roc
//! shared library's public interface.
//!
//! Usage: export_check <path> <expected-export>...

const std = @import("std");
const elf = std.elf;

pub fn main(init: std.process.Init) anyerror!void {
    var arena_impl = std.heap.ArenaAllocator.init(std.heap.page_allocator);
    defer arena_impl.deinit();
    const arena = arena_impl.allocator();

    var arg_iter = try std.process.Args.Iterator.initAllocator(init.minimal.args, arena);
    defer arg_iter.deinit();
    _ = arg_iter.skip();
    const path = arg_iter.next() orelse {
        std.debug.print("Usage: export_check <path> <expected-export>...\n", .{});
        return error.MissingPath;
    };
    var expected = std.StringArrayHashMapUnmanaged(void).empty;
    while (arg_iter.next()) |name| try expected.put(arena, name, {});

    const bytes = try std.Io.Dir.cwd().readFileAlloc(init.io, path, arena, .unlimited);
    const exports = try definedDynamicSymbols(arena, bytes);

    var failed = false;
    for (exports) |name| {
        if (!expected.contains(name)) {
            std.debug.print("FAILED: {s} exports unexpected symbol {s}\n", .{ path, name });
            failed = true;
        }
    }
    for (expected.keys()) |name| {
        if (!containsName(exports, name)) {
            std.debug.print("FAILED: {s} does not export {s}\n", .{ path, name });
            failed = true;
        }
    }
    if (failed) return error.UnexpectedExportSet;

    std.debug.print("SUCCESS: {s} exports exactly the expected {d} symbols\n", .{ path, expected.count() });
}

fn containsName(names: []const []const u8, wanted: []const u8) bool {
    for (names) |name| {
        if (std.mem.eql(u8, name, wanted)) return true;
    }
    return false;
}

/// Names of the global and weak symbols `.dynsym` defines.
fn definedDynamicSymbols(arena: std.mem.Allocator, bytes: []const u8) (std.mem.Allocator.Error || error{ NotElf, UnsupportedElf, MalformedElf, MissingDynamicSymbolTable })![]const []const u8 {
    if (bytes.len < @sizeOf(elf.Elf64_Ehdr)) return error.NotElf;
    if (!std.mem.eql(u8, bytes[0..4], elf.MAGIC)) return error.NotElf;
    if (bytes[elf.EI.CLASS] != elf.ELFCLASS64 or bytes[elf.EI.DATA] != elf.ELFDATA2LSB) return error.UnsupportedElf;
    const ehdr = std.mem.bytesToValue(elf.Elf64_Ehdr, bytes[0..@sizeOf(elf.Elf64_Ehdr)]);

    const shdrs = try sectionHeaders(bytes, ehdr);
    for (shdrs) |shdr| {
        if (shdr.sh_type != elf.SHT_DYNSYM) continue;
        if (shdr.sh_link >= shdrs.len) return error.MalformedElf;
        const strtab = try sectionBytes(bytes, shdrs[shdr.sh_link]);
        const symtab = try sectionBytes(bytes, shdr);
        const count = symtab.len / @sizeOf(elf.Elf64_Sym);

        var names = std.ArrayList([]const u8).empty;
        for (0..count) |index| {
            const sym = std.mem.bytesToValue(elf.Elf64_Sym, symtab[index * @sizeOf(elf.Elf64_Sym) ..][0..@sizeOf(elf.Elf64_Sym)]);
            if (sym.st_shndx == elf.SHN_UNDEF) continue;
            const bind = sym.st_info >> 4;
            if (bind != elf.STB_GLOBAL and bind != elf.STB_WEAK) continue;
            if (sym.st_name >= strtab.len) return error.MalformedElf;
            try names.append(arena, std.mem.sliceTo(strtab[sym.st_name..], 0));
        }
        return names.items;
    }
    return error.MissingDynamicSymbolTable;
}

fn sectionHeaders(bytes: []const u8, ehdr: elf.Elf64_Ehdr) error{MalformedElf}![]align(1) const elf.Elf64_Shdr {
    const start = std.math.cast(usize, ehdr.e_shoff) orelse return error.MalformedElf;
    const size = @as(usize, ehdr.e_shnum) * @sizeOf(elf.Elf64_Shdr);
    if (ehdr.e_shentsize != @sizeOf(elf.Elf64_Shdr) or start > bytes.len or size > bytes.len - start) return error.MalformedElf;
    return std.mem.bytesAsSlice(elf.Elf64_Shdr, bytes[start..][0..size]);
}

fn sectionBytes(bytes: []const u8, shdr: elf.Elf64_Shdr) error{MalformedElf}![]const u8 {
    const start = std.math.cast(usize, shdr.sh_offset) orelse return error.MalformedElf;
    const size = std.math.cast(usize, shdr.sh_size) orelse return error.MalformedElf;
    if (start > bytes.len or size > bytes.len - start) return error.MalformedElf;
    return bytes[start..][0..size];
}
