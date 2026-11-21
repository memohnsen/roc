//! A local binding or parameter that shadows a definition of its own module
//! written later in the source is not reported, as before a type's members
//! were all put in scope at the start of its block. Every other shadowing
//! still is.

const std = @import("std");
const parse = @import("parse");
const Can = @import("../Can.zig");
const ModuleEnv = @import("../ModuleEnv.zig");
const BuiltinTestContext = @import("BuiltinTestContext.zig").BuiltinTestContext;
const CoreCtx = @import("ctx").CoreCtx;

fn countShadowingWarnings(source: []const u8, name: []const u8) std.mem.Allocator.Error!usize {
    const allocator = std.testing.allocator;
    var builtin_ctx = try BuiltinTestContext.init(allocator);
    defer builtin_ctx.deinit();

    var env = try ModuleEnv.init(allocator, source);
    defer env.deinit();
    try env.initCIRFields("Test");

    const ast = try parse.file(allocator, &env.common);
    defer ast.deinit();

    const roc_ctx = CoreCtx.testing(allocator, allocator);
    var can = try Can.initModule(roc_ctx, &env, ast, builtin_ctx.canInitContext());
    defer can.deinit();
    try can.canonicalizeFile();

    const diagnostics = try env.getDiagnostics();
    defer allocator.free(diagnostics);
    var count: usize = 0;
    for (diagnostics) |diag| {
        if (diag != .shadowing_warning) continue;
        if (std.mem.eql(u8, env.getIdent(diag.shadowing_warning.ident), name)) count += 1;
    }
    return count;
}

test "a parameter shadowing a later member of the module's type is not reported" {
    const source =
        \\T := [].{
        \\    keep = |items| items.drop_if(|option| option == "")
        \\    option = "x"
        \\}
    ;
    try std.testing.expectEqual(@as(usize, 0), try countShadowingWarnings(source, "option"));
}

test "a block local shadowing a later member of the module's type is not reported" {
    const source =
        \\T := [].{
        \\    bump = |n| {
        \\        count = n + 1
        \\        count
        \\    }
        \\    count = 3
        \\}
    ;
    try std.testing.expectEqual(@as(usize, 0), try countShadowingWarnings(source, "count"));
}

test "a pattern binding shadowing a later member of the module's type is not reported" {
    const source =
        \\T := [].{
        \\    query = |route| match route.split_first("?") {
        \\        Ok({ after, .. }) => after
        \\        Err(_) => ""
        \\    }
        \\    after = |text| text
        \\}
    ;
    try std.testing.expectEqual(@as(usize, 0), try countShadowingWarnings(source, "after"));
}

test "a local shadowing a later member of a nested type of the module is not reported" {
    const source =
        \\T := [].{
        \\    U := [].{
        \\        g = |name| name
        \\        name = "n"
        \\    }
        \\}
    ;
    try std.testing.expectEqual(@as(usize, 0), try countShadowingWarnings(source, "name"));
}

test "a parameter shadowing an earlier member of the module's type is reported" {
    const source =
        \\T := [].{
        \\    option = "x"
        \\    keep = |items| items.drop_if(|option| option == "")
        \\}
    ;
    try std.testing.expectEqual(@as(usize, 1), try countShadowingWarnings(source, "option"));
}

test "a parameter shadowing an earlier top-level definition is reported" {
    const source =
        \\limit = 10
        \\clamp = |limit| limit
    ;
    try std.testing.expectEqual(@as(usize, 1), try countShadowingWarnings(source, "limit"));
}

test "a local shadowing another local is reported" {
    const source =
        \\f = |x| {
        \\    y = x + 1
        \\    g = |y| y * 2
        \\    g(y)
        \\}
    ;
    try std.testing.expectEqual(@as(usize, 1), try countShadowingWarnings(source, "y"));
}

test "a nested type's member shadowing a later member of the enclosing type is still reported" {
    const source =
        \\T := [].{
        \\    U := [].{
        \\        to_num = |_| 1
        \\    }
        \\    to_num = |_| 2
        \\}
    ;
    try std.testing.expectEqual(@as(usize, 1), try countShadowingWarnings(source, "to_num"));
}
