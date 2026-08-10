# GhidraSQL headless bring-up — run notes

Followed `GhidraSQL/install-prompt.md` end to end on Ubuntu 24.04 / x86-64,
GCC 13.3, JDK 21.0.10, CMake 3.28.3, Gradle 8.14.3, Python 3.11.

Result: all 8 steps pass, but **step 6 crashes on a stock build**. Root cause
and workaround below.

## Environment actually used

| Item | Value |
|------|-------|
| Ghidra | 12.1.2 (`Ghidra_12.1.2_build`, asset `ghidra_12.1.2_PUBLIC_20260605.zip`) |
| `GHIDRA_INSTALL_DIR` | `/opt/ghidra_12.1.2_PUBLIC` |
| SHA-256 | `b62e81a0390618466c019c60d8c2f796ced2509c4c1aea4a37644a77272cf99d` (matches release page) |
| ghidrasql | v0.0.3, `33d5dea` |
| Test binary | `/bin/ls` (412 functions) |

Steps 0-5 and 7-8 passed their gates unmodified. The LibGhidraHost extension
built and installed with no `protoc` trouble, so the install-prompt's
"cannot find symbol" remediation was not needed.

## The crash (step 6)

A stock `cmake -B build -DGHIDRASQL_LIBGHIDRA_DIR=../libghidra/cpp
-DCMAKE_BUILD_TYPE=Release` build aborts on the first RPC, immediately after
the Java host prints `LIBGHIDRA_HEADLESS_READY`:

```
LIBGHIDRA_HEADLESS_READY bind=127.0.0.1 port=41551 ...
  headless RPC (resolved): http://127.0.0.1:41551
Fatal glibc error: pthread_mutex_lock.c:94 (___pthread_mutex_lock):
  assertion failed: mutex->__data.__owner == 0
```

It aborts before any query runs, and **leaves the Ghidra JVM orphaned** — the
host script sits in its `while (host.isRunning())` poll loop
(`LibGhidraHeadlessServer.java:147`) forever. Kill it by PID before relaunching.

gdb backtrace:

```
#8  httplib::ClientImpl::send_with_content_provider(...)
#9  libghidra::client::HttpClient::Impl::request_bytes(...)
#10 libghidra::client::HttpClient::Impl::call_rpc<ImportProgramRequest, ...>
#11 libghidra::client::HttpClient::ImportProgram(...)
#12 import_programs_and_open_active(...)
#13 main
```

### Root cause: ODR violation across vendored cpp-httplib copies

cpp-httplib is header-only in namespace `httplib`, so every TU in the link must
agree on `ClientImpl`'s layout. Two independent disagreements exist:

1. **Version skew.** `CMakeLists.txt:51-59` pre-fetches httplib **0.16.3** as
   target `httplib::httplib` specifically to preempt libxsql's 0.15.3, and
   `libghidra/cpp/CMakeLists.txt:68` honours it via
   `if(NOT TARGET httplib::httplib)`. But **fastmcpp** declares its own
   `FetchContent` named `cpp_httplib` pinned to **v0.15.3** with no such guard
   and propagates it `PUBLIC`, so the CLI compiled against 0.15.3 while
   `libghidra_client.a` used 0.16.3.

2. **Macro skew — the actual trigger.** `fastmcpp_core` compiles `httplib.h`
   with *no* `CPPHTTPLIB_OPENSSL_SUPPORT` / `_ZLIB_SUPPORT` / `_BROTLI_SUPPORT`,
   while `libghidra_client` and `ghidrasql` compile it with all three. Those
   macros add members to `ClientImpl`. The linker keeps one inline copy of the
   weak `send_with_content_provider`, which then locks a `std::mutex` at an
   offset that is not a mutex in the object actually constructed — hence the
   "owner != 0" assertion on freshly initialised memory.

Fixing only (1) does not stop the crash; (2) is required.

### Workaround (no source changes)

```bash
cmake -B build -DGHIDRASQL_LIBGHIDRA_DIR=../libghidra/cpp \
  -DCMAKE_BUILD_TYPE=Release \
  -DFETCHCONTENT_SOURCE_DIR_CPP_HTTPLIB="$PWD/build/_deps/httplib-src" \
  -DCMAKE_CXX_FLAGS="-DCPPHTTPLIB_OPENSSL_SUPPORT -DCPPHTTPLIB_ZLIB_SUPPORT -DCPPHTTPLIB_BROTLI_SUPPORT" \
  -DCMAKE_CXX_STANDARD_LIBRARIES="-lssl -lcrypto -lz -lbrotlienc -lbrotlidec -lbrotlicommon"
cmake --build build -j
```

`FETCHCONTENT_SOURCE_DIR_CPP_HTTPLIB` points fastmcpp at the same 0.16.3 tree
(the path only exists after a first configure has fetched it). The defines make
every TU agree on the layout; once fastmcpp compiles the SSL/zlib/brotli code
paths its own executable needs those libraries, and they must go in
`CMAKE_CXX_STANDARD_LIBRARIES` — `CMAKE_EXE_LINKER_FLAGS` places them before the
object files, where the linker discards them.

`-DGHIDRASQL_WITH_MCP=OFF` is the alternative: it drops fastmcpp, the only
source of the mismatch, at the cost of the MCP server.

An upstream fix belongs in fastmcpp: guard its `cpp_httplib` fetch with
`if(NOT TARGET httplib::httplib)` and inherit the parent's httplib config.

## Verification after the fix

`--analyze` over `/bin/ls`, HTTP serve mode on port 8081:

- `/health` → `{"status":"ok","success":true,"tool":"ghidrasql"}`
- `/health/deep` → `{"healthy":true,"active_queries":0,...}`
- `SELECT * FROM binary` → `language_id=x86:LE:64:default`, `revision=3`,
  `processor=x86`, `bits=64`, `image_base=0x100000`, `entry_point=0x106d30`
- `SELECT COUNT(*) FROM funcs` → **412**
- top-5-by-size returned 5 rows with `name` + `addr`
- `imports` → `__libc_start_main`, `getenv`, `sigprocmask`, … (`<EXTERNAL>`)
- `strings` → `/lib64/ld-linux-x86-64.so.2`, `obstack_alloc_failed_handler`, …
- `pseudocode WHERE func_addr=0x106D30` → decompiled `entry()` correctly

Step 8: `POST /shutdown` returned promptly, the client exited, the project
flushed to `boot.gpr` + `boot.rep/` with no `*.lock` files.

### Two runbook notes worth folding upstream

- Column names in the smoke-test area differ from the obvious guesses:
  `imports(addr, name, module)`, `strings(addr, length, type, …, content)` —
  not `library` / `value`; `pseudocode(func_addr, func_name, text, is_stale)` —
  not `line`. `funcs` has no `is_external`.
- Step 8's `pgrep -f 'ghidrasql|java'` false-positives on the **Gradle daemon**
  left over from step 3 (the runbook flags this) and, in a shell whose own
  command line contains the pattern, on the `pgrep` wrapper itself. Match by
  PID/start time before concluding shutdown failed.
