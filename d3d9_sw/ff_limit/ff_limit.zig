//! Keep Firefox from ballooning, using only the kernel's own supervisory API.
//!
//! Windows job objects: the same primitive Chrome and Firefox use to build
//! their own sandboxes. Enforcement is done by the memory manager, not by us.
//! No injection, no remote threads, no writes into Firefox, no debugger attach.
//!
//! ONE JOB PER PROCESS, and that detail is not optional. Firefox sandboxes each
//! of its processes in a job of its own. Assigning a process that is already in
//! job J_old to our job J_new makes J_new a *child* of J_old, and a job can
//! have only one parent. So a single shared job nests under exactly one Firefox
//! sandbox job and every later AssignProcessToJobObject fails with
//! ERROR_ACCESS_DENIED. Measured on a live browser: a shared job captured 1 of
//! 17 processes, a job per process captured 17 of 17.
//!
//! The consequence is that there is no job-wide commit cap. A job that can only
//! ever hold one process cannot total anything. Per-process caps are the tier
//! that matters anyway: they make one gluttonous content process die alone,
//! with a "tab crashed" page, while the browser survives.
//!
//! Two tiers:
//!
//!   1. Working set cap (JOB_OBJECT_LIMIT_WORKINGSET)
//!      The kernel trims resident pages above the cap. Firefox may still
//!      commit, it just cannot keep it all resident. Nothing dies. Costs
//!      pagefile I/O if Firefox genuinely needs more than the cap. Needs
//!      SeIncreaseWorkingSetPrivilege, which we try to enable in our own token;
//!      if that fails the tier is skipped and the commit cap still applies.
//!
//!   2. Per-process commit cap (JOB_OBJECT_LIMIT_PROCESS_MEMORY)
//!      VirtualAlloc fails past the cap. Firefox's infallible allocator turns
//!      that into an OOM abort. This is the real puke, and it needs no
//!      privileges at all.
//!
//! Jobs are destroyed when this process exits, which silently removes every
//! limit. Firefox is NOT killed with them: JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
//! is deliberately never set.
//!
//! Caps count private commit, not reserved address space. Firefox reserves tens
//! of GB of VA it never commits; that is normal and is not what makes it fat.
//!
//! There is no --puke tier. Injecting jemalloc_free_dirty_pages via
//! CreateRemoteThread skipped every main-thread-only arena, where the DOM/JS
//! heap actually lives, and returned tens of MB out of many GB, while being the
//! only injection-shaped thing in the tool. Not worth carrying.
//!
//! Build: zig build-exe ff_limit.zig -O ReleaseSmall

const std = @import("std");
const windows = std.os.windows;

const HANDLE = windows.HANDLE;
const DWORD = windows.DWORD;
const BOOL = windows.BOOL;
const WINAPI = std.builtin.CallingConvention.winapi;

const GB: u64 = 1024 * 1024 * 1024;
const MB: u64 = 1024 * 1024;

const PROCESS_TERMINATE: DWORD = 0x0001;
const PROCESS_SET_QUOTA: DWORD = 0x0100;
const PROCESS_QUERY_LIMITED_INFORMATION: DWORD = 0x1000;

const JOB_OBJECT_LIMIT_WORKINGSET: DWORD = 0x0000_0001;
const JOB_OBJECT_LIMIT_PROCESS_MEMORY: DWORD = 0x0000_0100;
const JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION: DWORD = 0x0000_0400;

const JobObjectExtendedLimitInformation: c_int = 9;
const TH32CS_SNAPPROCESS: DWORD = 0x2;
const ERROR_NOT_ALL_ASSIGNED: DWORD = 1300;
const ERROR_PRIVILEGE_NOT_HELD: DWORD = 1314;

const TOKEN_QUERY: DWORD = 0x0008;
const TOKEN_ADJUST_PRIVILEGES: DWORD = 0x0020;
const SE_PRIVILEGE_ENABLED: DWORD = 0x0002;

const MAX_PROCS = 512;

const PROCESSENTRY32W = extern struct {
    dwSize: DWORD,
    cntUsage: DWORD,
    th32ProcessID: DWORD,
    th32DefaultHeapID: usize,
    th32ModuleID: DWORD,
    cntThreads: DWORD,
    th32ParentProcessID: DWORD,
    pcPriClassBase: i32,
    dwFlags: DWORD,
    szExeFile: [260]u16,
};

const PROCESS_MEMORY_COUNTERS_EX = extern struct {
    cb: DWORD,
    PageFaultCount: DWORD,
    PeakWorkingSetSize: usize,
    WorkingSetSize: usize,
    QuotaPeakPagedPoolUsage: usize,
    QuotaPagedPoolUsage: usize,
    QuotaPeakNonPagedPoolUsage: usize,
    QuotaNonPagedPoolUsage: usize,
    PagefileUsage: usize,
    PeakPagefileUsage: usize,
    PrivateUsage: usize,
};

const JOBOBJECT_BASIC_LIMIT_INFORMATION = extern struct {
    PerProcessUserTimeLimit: i64,
    PerJobUserTimeLimit: i64,
    LimitFlags: DWORD,
    MinimumWorkingSetSize: usize,
    MaximumWorkingSetSize: usize,
    ActiveProcessLimit: DWORD,
    Affinity: usize,
    PriorityClass: DWORD,
    SchedulingClass: DWORD,
};

const IO_COUNTERS = extern struct {
    ReadOperationCount: u64,
    WriteOperationCount: u64,
    OtherOperationCount: u64,
    ReadTransferCount: u64,
    WriteTransferCount: u64,
    OtherTransferCount: u64,
};

const JOBOBJECT_EXTENDED_LIMIT_INFORMATION = extern struct {
    BasicLimitInformation: JOBOBJECT_BASIC_LIMIT_INFORMATION,
    IoInfo: IO_COUNTERS,
    ProcessMemoryLimit: usize,
    JobMemoryLimit: usize,
    PeakProcessMemoryUsed: usize,
    PeakJobMemoryUsed: usize,
};

const SYSTEMTIME = extern struct {
    wYear: u16,
    wMonth: u16,
    wDayOfWeek: u16,
    wDay: u16,
    wHour: u16,
    wMinute: u16,
    wSecond: u16,
    wMilliseconds: u16,
};

const LUID = extern struct { LowPart: DWORD, HighPart: i32 };
const LUID_AND_ATTRIBUTES = extern struct { Luid: LUID, Attributes: DWORD };
const TOKEN_PRIVILEGES = extern struct { PrivilegeCount: DWORD, Privileges: [1]LUID_AND_ATTRIBUTES };

extern "kernel32" fn CreateToolhelp32Snapshot(dwFlags: DWORD, th32ProcessID: DWORD) callconv(WINAPI) HANDLE;
extern "kernel32" fn Process32FirstW(hSnapshot: HANDLE, lppe: *PROCESSENTRY32W) callconv(WINAPI) BOOL;
extern "kernel32" fn Process32NextW(hSnapshot: HANDLE, lppe: *PROCESSENTRY32W) callconv(WINAPI) BOOL;
extern "kernel32" fn OpenProcess(dwDesiredAccess: DWORD, bInheritHandle: BOOL, dwProcessId: DWORD) callconv(WINAPI) ?HANDLE;
extern "kernel32" fn CreateJobObjectW(lpJobAttributes: ?*anyopaque, lpName: ?[*:0]const u16) callconv(WINAPI) ?HANDLE;
extern "kernel32" fn SetInformationJobObject(hJob: HANDLE, cls: c_int, info: *const anyopaque, len: DWORD) callconv(WINAPI) BOOL;
extern "kernel32" fn QueryInformationJobObject(hJob: ?HANDLE, cls: c_int, info: *anyopaque, len: DWORD, ret: ?*DWORD) callconv(WINAPI) BOOL;
extern "kernel32" fn AssignProcessToJobObject(hJob: HANDLE, hProcess: HANDLE) callconv(WINAPI) BOOL;
extern "kernel32" fn IsProcessInJob(hProcess: HANDLE, hJob: ?HANDLE, result: *BOOL) callconv(WINAPI) BOOL;
extern "kernel32" fn CloseHandle(h: HANDLE) callconv(WINAPI) BOOL;
extern "kernel32" fn GetLastError() callconv(WINAPI) DWORD;
extern "kernel32" fn Sleep(ms: DWORD) callconv(WINAPI) void;
extern "kernel32" fn GetLocalTime(t: *SYSTEMTIME) callconv(WINAPI) void;
extern "kernel32" fn GetCurrentProcess() callconv(WINAPI) HANDLE;
// K32* form lives in kernel32, so we avoid a psapi.dll import entirely.
extern "kernel32" fn K32GetProcessMemoryInfo(h: HANDLE, c: *PROCESS_MEMORY_COUNTERS_EX, cb: DWORD) callconv(WINAPI) BOOL;

extern "advapi32" fn OpenProcessToken(h: HANDLE, access: DWORD, tok: *HANDLE) callconv(WINAPI) BOOL;
extern "advapi32" fn LookupPrivilegeValueW(sys: ?[*:0]const u16, name: [*:0]const u16, luid: *LUID) callconv(WINAPI) BOOL;
extern "advapi32" fn AdjustTokenPrivileges(tok: HANDLE, disable_all: BOOL, new: ?*TOKEN_PRIVILEGES, len: DWORD, prev: ?*TOKEN_PRIVILEGES, ret: ?*DWORD) callconv(WINAPI) BOOL;

fn log(comptime fmt: []const u8, args: anytype) void {
    var st: SYSTEMTIME = undefined;
    GetLocalTime(&st);
    std.debug.print(
        "[{d:0>2}:{d:0>2}:{d:0>2}] " ++ fmt ++ "\n",
        .{ st.wHour, st.wMinute, st.wSecond } ++ args,
    );
}

fn gb(bytes: u64) f64 {
    return @as(f64, @floatFromInt(bytes)) / @as(f64, GB);
}

/// SeIncreaseWorkingSetPrivilege is held but disabled in a normal user token,
/// so enabling it is often possible without elevation. Returns false if the
/// token does not actually hold it.
fn enableWorkingSetPrivilege() bool {
    var tok: HANDLE = undefined;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok) == 0)
        return false;
    defer _ = CloseHandle(tok);

    var tp = std.mem.zeroes(TOKEN_PRIVILEGES);
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    const name = std.unicode.utf8ToUtf16LeStringLiteral("SeIncreaseWorkingSetPrivilege");
    if (LookupPrivilegeValueW(null, name, &tp.Privileges[0].Luid) == 0) return false;

    // Succeeds with ERROR_NOT_ALL_ASSIGNED if the token lacks the privilege.
    if (AdjustTokenPrivileges(tok, 0, &tp, @sizeOf(TOKEN_PRIVILEGES), null, null) == 0) return false;
    return GetLastError() != ERROR_NOT_ALL_ASSIGNED;
}

const Proc = struct { pid: DWORD, ppid: DWORD };

fn isFirefox(name: []const u16) bool {
    const want = "firefox.exe";
    var len: usize = 0;
    while (len < name.len and name[len] != 0) len += 1;
    if (len != want.len) return false;
    for (name[0..len], want) |c, wc| {
        const lower = if (c >= 'A' and c <= 'Z') c + 32 else c;
        if (lower != wc) return false;
    }
    return true;
}

/// Fills `buf` with every live firefox.exe. Fixed buffer, no allocation on the
/// poll path.
fn enumFirefox(buf: []Proc) []Proc {
    const snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == windows.INVALID_HANDLE_VALUE) return buf[0..0];
    defer _ = CloseHandle(snap);

    var pe: PROCESSENTRY32W = undefined;
    pe.dwSize = @sizeOf(PROCESSENTRY32W);
    var n: usize = 0;
    var ok = Process32FirstW(snap, &pe);
    while (ok != 0 and n < buf.len) : (ok = Process32NextW(snap, &pe)) {
        if (isFirefox(&pe.szExeFile)) {
            buf[n] = .{ .pid = pe.th32ProcessID, .ppid = pe.th32ParentProcessID };
            n += 1;
        }
    }
    return buf[0..n];
}

const Config = struct {
    max_process_gb: f64 = 4.0,
    max_ws_mb: f64 = 1024.0,
    min_ws_mb: f64 = 32.0,
    quiet_crash: bool = true,
    watch_only: bool = false,
    verbose: bool = false,
    interval_s: f64 = 2.0,
    status_s: f64 = 30.0,
};

const usage =
    \\Cap Firefox with per-process Windows job objects (no injection).
    \\
    \\  --max-process-gb F  per-process commit cap; a runaway tab dies alone [4]
    \\  --max-ws-mb F       per-process resident cap; kernel trims, nothing dies [1024]
    \\  --min-ws-mb F       per-process working set floor, must be nonzero  [32]
    \\  --no-quiet-crash    allow the Windows crash dialog on unhandled exceptions
    \\  --watch-only        measure and report, set no limits at all
    \\  --verbose           log every process as it is capped
    \\  --interval F        poll seconds                                    [2]
    \\  --status F          status line seconds                            [30]
    \\
    \\Either cap may be 0 to disable that tier. Limits vanish when this exits.
    \\
;

fn parseArgs(alloc: std.mem.Allocator) !?Config {
    var cfg = Config{};
    const argv = try std.process.argsAlloc(alloc);
    defer std.process.argsFree(alloc, argv);

    var i: usize = 1;
    while (i < argv.len) : (i += 1) {
        const a = argv[i];
        const Pair = struct { name: []const u8, field: *f64 };
        const pairs = [_]Pair{
            .{ .name = "--max-process-gb", .field = &cfg.max_process_gb },
            .{ .name = "--max-ws-mb", .field = &cfg.max_ws_mb },
            .{ .name = "--min-ws-mb", .field = &cfg.min_ws_mb },
            .{ .name = "--interval", .field = &cfg.interval_s },
            .{ .name = "--status", .field = &cfg.status_s },
        };
        var matched = false;
        for (pairs) |p| {
            if (std.mem.eql(u8, a, p.name)) {
                i += 1;
                if (i >= argv.len) {
                    std.debug.print("{s} needs a value\n", .{p.name});
                    return error.BadArgs;
                }
                p.field.* = std.fmt.parseFloat(f64, argv[i]) catch {
                    std.debug.print("{s}: not a number: {s}\n", .{ p.name, argv[i] });
                    return error.BadArgs;
                };
                matched = true;
                break;
            }
        }
        if (matched) continue;
        if (std.mem.eql(u8, a, "--no-quiet-crash")) {
            cfg.quiet_crash = false;
        } else if (std.mem.eql(u8, a, "--watch-only")) {
            cfg.watch_only = true;
        } else if (std.mem.eql(u8, a, "--verbose")) {
            cfg.verbose = true;
        } else if (std.mem.eql(u8, a, "-h") or std.mem.eql(u8, a, "--help")) {
            std.debug.print("{s}", .{usage});
            return null;
        } else {
            std.debug.print("unknown option: {s}\n\n{s}", .{ a, usage });
            return error.BadArgs;
        }
    }

    if (cfg.max_ws_mb > 0 and cfg.min_ws_mb <= 0) {
        std.debug.print("--min-ws-mb must be nonzero when --max-ws-mb is set\n", .{});
        return error.BadArgs;
    }
    if (cfg.max_process_gb <= 0 and cfg.max_ws_mb <= 0 and !cfg.watch_only) {
        std.debug.print("both caps are 0; nothing to enforce (use --watch-only?)\n", .{});
        return error.BadArgs;
    }
    return cfg;
}

const Limiter = struct {
    cfg: Config,
    max_proc: usize,
    max_ws: usize,
    min_ws: usize,
    ws_ok: bool,
    /// One job per process; the handle must stay open or the limits vanish.
    jobs: std.AutoHashMap(DWORD, HANDLE),
    failed: std.AutoHashMap(DWORD, u8),

    fn init(alloc: std.mem.Allocator, cfg: Config) Limiter {
        return .{
            .cfg = cfg,
            .max_proc = @intFromFloat(cfg.max_process_gb * @as(f64, GB)),
            .max_ws = @intFromFloat(cfg.max_ws_mb * @as(f64, MB)),
            .min_ws = @intFromFloat(cfg.min_ws_mb * @as(f64, MB)),
            .ws_ok = cfg.max_ws_mb > 0,
            .jobs = std.AutoHashMap(DWORD, HANDLE).init(alloc),
            .failed = std.AutoHashMap(DWORD, u8).init(alloc),
        };
    }

    fn limitFlags(self: *Limiter, with_ws: bool) JOBOBJECT_EXTENDED_LIMIT_INFORMATION {
        var info = std.mem.zeroes(JOBOBJECT_EXTENDED_LIMIT_INFORMATION);
        var flags: DWORD = 0;
        if (self.max_proc > 0) {
            flags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
            info.ProcessMemoryLimit = self.max_proc;
        }
        if (with_ws and self.max_ws > 0) {
            // Both bounds must be nonzero together, per winnt.h.
            flags |= JOB_OBJECT_LIMIT_WORKINGSET;
            info.BasicLimitInformation.MinimumWorkingSetSize = self.min_ws;
            info.BasicLimitInformation.MaximumWorkingSetSize = self.max_ws;
        }
        if (self.cfg.quiet_crash) flags |= JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
        info.BasicLimitInformation.LimitFlags = flags;
        return info;
    }

    /// Creates a dedicated job for `pid`, applies the limits, then assigns.
    /// Limits go on before the assign so the process is capped the instant it
    /// joins. Returns the job handle, which the caller must keep alive.
    fn capture(self: *Limiter, pid: DWORD) ?HANDLE {
        const h = OpenProcess(
            PROCESS_SET_QUOTA | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
            0,
            pid,
        ) orelse {
            self.bumpFail(pid, GetLastError(), "OpenProcess");
            return null;
        };
        defer _ = CloseHandle(h);

        const job = CreateJobObjectW(null, null) orelse {
            self.bumpFail(pid, GetLastError(), "CreateJobObject");
            return null;
        };

        var info = self.limitFlags(self.ws_ok);
        var ok = SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, @sizeOf(@TypeOf(info)));
        if (ok == 0 and self.ws_ok and GetLastError() == ERROR_PRIVILEGE_NOT_HELD) {
            // Enabling SeIncreaseWorkingSetPrivilege in our own token is not
            // enough; measured on Win11, JOB_OBJECT_LIMIT_WORKINGSET is only
            // accepted from an elevated process. The commit cap needs no
            // privilege and is the tier that actually stops growth, so drop
            // the working set tier and keep going.
            log("working set tier refused; run elevated for it. Commit cap still applies.", .{});
            self.ws_ok = false;
            info = self.limitFlags(false);
            ok = SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, @sizeOf(@TypeOf(info)));
        }
        if (ok == 0) {
            self.bumpFail(pid, GetLastError(), "SetInformationJobObject");
            _ = CloseHandle(job);
            return null;
        }

        if (AssignProcessToJobObject(job, h) == 0) {
            self.bumpFail(pid, GetLastError(), "AssignProcessToJobObject");
            _ = CloseHandle(job);
            return null;
        }
        return job;
    }

    fn attach(self: *Limiter, procs: []const Proc) void {
        if (self.cfg.watch_only) return;
        for (procs) |p| {
            if (self.jobs.contains(p.pid)) continue;
            if ((self.failed.get(p.pid) orelse 0) >= 3) continue;
            const job = self.capture(p.pid) orelse continue;
            self.jobs.put(p.pid, job) catch {
                _ = CloseHandle(job);
                continue;
            };
            if (self.cfg.verbose) log("capped pid {d} (ppid {d})", .{ p.pid, p.ppid });
        }
    }

    fn bumpFail(self: *Limiter, pid: DWORD, err: DWORD, what: []const u8) void {
        const n = (self.failed.get(pid) orelse 0) + 1;
        self.failed.put(pid, n) catch return;
        // Retry a couple of times first: a process can refuse right at spawn.
        if (n >= 3) log("pid {d} stays uncapped: {s} err {d}", .{ pid, what, err });
    }

    fn snapshot(self: *Limiter, procs: []const Proc) struct { ws: u64, priv: u64, peak: u64 } {
        var ws: u64 = 0;
        var priv: u64 = 0;
        for (procs) |p| {
            const h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, p.pid) orelse continue;
            var c = std.mem.zeroes(PROCESS_MEMORY_COUNTERS_EX);
            c.cb = @sizeOf(PROCESS_MEMORY_COUNTERS_EX);
            if (K32GetProcessMemoryInfo(h, &c, c.cb) != 0) {
                ws += c.WorkingSetSize;
                priv += c.PrivateUsage;
            }
            _ = CloseHandle(h);
        }
        self.prune(procs);
        return .{ .ws = ws, .priv = priv, .peak = self.peak() };
    }

    /// Worst single-process commit any job has seen. With one process per job,
    /// PeakProcessMemoryUsed is that process's high-water mark.
    fn peak(self: *Limiter) u64 {
        var worst: u64 = 0;
        var it = self.jobs.valueIterator();
        while (it.next()) |job| {
            var info = std.mem.zeroes(JOBOBJECT_EXTENDED_LIMIT_INFORMATION);
            var ret: DWORD = 0;
            if (QueryInformationJobObject(job.*, JobObjectExtendedLimitInformation, &info, @sizeOf(@TypeOf(info)), &ret) != 0) {
                if (info.PeakProcessMemoryUsed > worst) worst = info.PeakProcessMemoryUsed;
            }
        }
        return worst;
    }

    /// Drops bookkeeping for exited processes. Closing the job handle is what
    /// releases the kernel object, so this is a real leak if skipped.
    fn prune(self: *Limiter, procs: []const Proc) void {
        var dead: [MAX_PROCS]DWORD = undefined;
        var n: usize = 0;
        var jit = self.jobs.keyIterator();
        while (jit.next()) |k| {
            if (!isLive(procs, k.*) and n < dead.len) {
                dead[n] = k.*;
                n += 1;
            }
        }
        for (dead[0..n]) |pid| {
            if (self.jobs.fetchRemove(pid)) |kv| _ = CloseHandle(kv.value);
        }

        n = 0;
        var fit = self.failed.keyIterator();
        while (fit.next()) |k| {
            if (!isLive(procs, k.*) and n < dead.len) {
                dead[n] = k.*;
                n += 1;
            }
        }
        for (dead[0..n]) |pid| _ = self.failed.remove(pid);
    }

    fn isLive(procs: []const Proc, pid: DWORD) bool {
        for (procs) |p| if (p.pid == pid) return true;
        return false;
    }

    fn run(self: *Limiter) void {
        var buf: [MAX_PROCS]Proc = undefined;
        const interval_ms: DWORD = @intFromFloat(self.cfg.interval_s * 1000.0);
        const status_ms: i64 = @intFromFloat(self.cfg.status_s * 1000.0);
        var last_status: i64 = std.time.milliTimestamp() - status_ms;

        while (true) {
            const procs = enumFirefox(&buf);
            self.attach(procs);
            const m = self.snapshot(procs);
            const now = std.time.milliTimestamp();

            if (now - last_status >= status_ms) {
                last_status = now;
                if (procs.len == 0) {
                    log("no firefox.exe running", .{});
                } else {
                    log("n={d} capped={d}  private={d:.2} GB  ws={d:.2} GB  worst-proc={d:.2} GB", .{
                        procs.len, self.jobs.count(), gb(m.priv), gb(m.ws), gb(m.peak),
                    });
                }
            }
            Sleep(interval_ms);
        }
    }
};

pub fn main() !void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    const alloc = gpa.allocator();

    const cfg = (try parseArgs(alloc)) orelse return;
    var lim = Limiter.init(alloc, cfg);

    if (cfg.watch_only) {
        log("ff_limit watch-only: measuring, setting nothing", .{});
    } else {
        if (lim.max_ws > 0 and !enableWorkingSetPrivilege()) {
            log("SeIncreaseWorkingSetPrivilege unavailable; working set tier will be skipped", .{});
            lim.ws_ok = false;
        }
        log("ff_limit enforcing, one job per process; limits vanish when this exits", .{});
        if (lim.max_proc > 0)
            log("  commit cap {d:.1} GB per process", .{gb(lim.max_proc)});
        if (lim.ws_ok)
            log("  working set {d:.0}-{d:.0} MB per process", .{
                @as(f64, @floatFromInt(lim.min_ws)) / @as(f64, MB),
                @as(f64, @floatFromInt(lim.max_ws)) / @as(f64, MB),
            });
        if (cfg.quiet_crash) log("  no crash dialog on unhandled exceptions", .{});
    }
    lim.run();
}
