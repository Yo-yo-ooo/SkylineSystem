# Lua 5.4.7（SkylineSystem desktop 内嵌版 / vendored）

本目录是 [Lua](https://www.lua.org/) 5.4.7 上游源码的 vendored 副本，供
`programs/desktop`（freestanding 用户态程序，动态链接 `libsky.so`）内嵌使用。
**除本 README/LICENSE/SHA256SUMS 外，目录内所有文件均逐字节来自上游 tarball，未做任何修改。**

## 来源与许可证

| 项目 | 值 |
| --- | --- |
| 上游 | https://www.lua.org/ftp/lua-5.4.7.tar.gz |
| 版本 | Lua 5.4.7（上游 release 日期 2024-06-13） |
| vendor 日期 | 2026-11-07 |
| tarball SHA-256 | `9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30` |
| 许可证 | MIT，Copyright (C) 1994–2024 Lua.org, PUC-Rio |
| 许可证全文 | 同目录 [`LICENSE`](LICENSE) |

说明：官方 `lua-5.4.7.tar.gz` 根目录**没有**独立 LICENSE 文件，许可证文本位于
tarball 内 `doc/readme.html` 的 "License" 一节（与 https://www.lua.org/license.html
一致）。同目录 `LICENSE` 即从该节提取的标准 MIT 文本（仅把 `©`/`–` 转为 ASCII），
未增删条款。

## 目录内容

- `*.c`：**28 个编译单元**（由 `programs/desktop/Makefile` 的 `find . -name '*.c'`
  自动收集编译，无需改动构建规则）：
  `lapi.c lauxlib.c lbaselib.c lcode.c lcorolib.c lctype.c ldblib.c ldebug.c ldo.c
  ldump.c lfunc.c lgc.c llex.c lmathlib.c lmem.c lobject.c lopcodes.c lparser.c
  lstate.c lstring.c lstrlib.c ltable.c ltablib.c ltm.c lundump.c lutf8lib.c lvm.c lzio.c`
- `*.h`：上游 `src/` 下**全部 27 个头文件**（`lua.h`/`luaconf.h`/`lauxlib.h`/`lualib.h`/
  `llimits.h`/`lobject.h`/`lstate.h`/… 原样保留，含仅被上游其他模块使用的头）。
- `LICENSE`：上述 MIT 文本。
- `SHA256SUMS`：本目录 28 个 `.c` + 27 个 `.h` 的 SHA-256 清单（审计用）。
- 规模：目录约 896 KiB（其中 `.c` 约 660 KiB / 21158 行，`.h` 约 212 KiB / 5435 行，
  其余为 `LICENSE`/`README.md`/`SHA256SUMS`）。

### 未 vendor 的上游文件及原因

明确**不拷贝、不编译**：

| 文件 | 原因 |
| --- | --- |
| `lua.c` | 上游独立 REPL 的 `main()`；桌面宿主入口是 `main.cpp`。 |
| `luac.c` | 上游字节码编译器；本系统不内置 luac。 |
| `linit.c` | 定义 `luaL_openlibs()` 一键注册全部标准库；桌面宿主按需逐个注册（见下），无此符号。 |
| `liolib.c` / `loslib.c` / `loadlib.c` | `io` / `os` / `package`(dlopen) 库；桌面不向脚本开放文件/环境/动态加载能力。 |
| `src/Makefile`、`lua.pc`、`doc/` | 构建系统/文档，由本仓库统一管理构建。 |

**宿主接入注意**：因为不含 `linit.c`，`luaL_openlibs()` 在本仓库中不存在。
宿主必须自行注册所需库，例如：

```c
static const luaL_Reg libs[] = {
    {LUA_GNAME, luaopen_base},     {LUA_STRLIBNAME, luaopen_string},
    {LUA_TABLIBNAME, luaopen_table}, {LUA_MATHLIBNAME, luaopen_math},
    {LUA_UTF8LIBNAME, luaopen_utf8}, {LUA_COLIBNAME, luaopen_coroutine},
    {LUA_DBLIBNAME, luaopen_debug},
};
luaL_requiref(L, libs[i].name, libs[i].fn, 1);
```

`io`/`os`/`package` 有意不提供。核对结果：`programs/desktop/lua_glue.cpp` 注册的正是
上述 7 个库，与 vendored 集合一致。

### C++ 宿主注意：上游 Lua 头文件是纯 C 头

Lua 5.4.7 的 `lua.h`/`lauxlib.h`/`lualib.h`/`luaconf.h` **没有 `extern "C"` guard**
（上游 tarball 即如此，也不提供 `lua.hpp`），属于上游设计而非本 vendored 目录的改动。
C++ 编译单元包含这些头文件时必须自行包 C linkage；并且本 freestanding 构建的 C++
不定义 `__STDC_VERSION__`，而 `luaconf.h` 以 `LLONG_MAX` 是否存在判断能否使用
`long long`（否则 `#error`），因此还需要在包含前补上该宏：

```cpp
#if defined(__cplusplus) && !defined(__STDC_VERSION__)
#  define __STDC_VERSION__ 201710L
#endif
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
```

否则 g++ 会为 Lua C API 生成 C++ mangled 引用（`_Z...lua*`），链接期表现为 undefined
（`libsky.so` 只提供 C 符号）。仓库内的 C++ 宿主接入文件（如
`programs/desktop/lua_glue.cpp`）必须按上述方式包含 Lua 头。

## 与上游的差异（patch 列表）

**零 patch（zero patch）。** 所有 `.c`/`.h` 与 `lua-5.4.7.tar.gz` 内 `src/` 逐字节一致
（已用 `cmp` 全量核对，并固化在 `SHA256SUMS` 中）。

freestanding 适配不在 Lua 源码内完成，而是由 `lib/stdc`（libc 任务）提供 Lua 需要的
标准 C 头与实现（`setjmp.h`/`signal.h`/`locale.h`/`time.h`、`stdio.h`/`string.h`/
`math.h` 的若干声明与函数）。Lua 内部**不重复定义任何 libc 函数**。

编译配置（均使用上游 `luaconf.h` 默认值，不额外传 `-D`）：

- 不定义 `LUA_USE_LINUX`/`LUA_USE_POSIX`/`LUA_USE_DLOPEN`：无 POSIX/`dlopen` 依赖，
  `ldo.c` 走 ISO C 的 `setjmp`/`longjmp` 路径，`lauxlib.c` 不会包含 `<sys/wait.h>`。
- `lua_Integer` = `long long`（64 位）、`lua_Number` = `double`、C99 接口（`l_sprintf`→
  `snprintf`、`lua_strx2number`→`strtod` 十六进制）。
- 未定义 `LUA_USE_C89`。

已知接口依赖（不是 patch，只是对 libc 的要求）：`luaconf.h` 中
`lua_getlocaledecpoint` 使用 `localeconv()->decimal_point[0]`，`lua_number2str`
使用 `snprintf`，`lauxlib.h` 的 `lua_writestring`/`lua_writeline`/`lua_writestringerror`
使用 `fwrite`/`fflush`/`fprintf`/`stdout`/`stderr`。详见下文"libc 依赖"。

## 构建接线

- `programs/desktop/Makefile` 的 `CFLAGS` 增加 `-I./lua`（`CXXFLAGS := $(CFLAGS) ...`
  自动继承），使 `lua_glue.cpp` 等宿主的 `#include <lua.h>` 可见；Lua `.c` 文件通过
  `find . -name '*.c'` 自动进入 `CFILES`，PIE 构建由 `../dynprog.mk` 的
  `$(PYOBJDIR)/%.c.o` 规则自动覆盖，**无需新增任何独立目标/库**。
- Lua 与 desktop 其余源码使用完全相同的编译选项
  （`-nostdlib -nostdinc -ffreestanding -fno-exceptions -mno-red-zone -ffunction-sections
  -fdata-sections -O2 -g3 -Wall -Wextra -fPIE` 等），对象产出在
  `programs/object/DESKTOP-pie/lua/*.o`（静态构建在 `programs/object/DESKTOP/lua/*.o`）。

## 编译验证

验证方式：在 `programs/desktop` 目录下，用与 `make` 完全相同的 flags 单独编译全部
28 个 `.c` 到 `/tmp`（不链接完整 desktop）：

```sh
cd programs/desktop
CFLAGS="-nostdlib -nostdinc \
  -I../../ablib/freestndchdrs/x86_64/include -I./include -I. -I./lua \
  -I../../lib/include/stdc -I../../lib/include -I../../lib/include/base/arch/x86_64 \
  -ffreestanding -fshort-wchar -mno-red-zone -fno-omit-frame-pointer -fno-exceptions \
  -ffunction-sections -fdata-sections -O2 -m64 -g3 -Wall -Wextra -fPIE"
for f in lua/*.c; do
  gcc $CFLAGS -c "$f" -o "/tmp/lua_desktop_obj/$(basename "${f%.c}").o" || echo "FAIL $f"
done
```

结果（gcc 14.2.0, GNU ld 2.45, 2026-11-07，**无任何临时 stub**，直接使用仓库内
`lib/include/stdc` 头文件）：**28/28 对象编译成功，0 error，0 warning。** 另外用
desktop 的 `CXXFLAGS` 单独编译宿主 `programs/desktop/lua_glue.cpp` 也是 0 error，且其
引用的全部 `lua*`/`luaL*` 符号都能被本目录 28 个对象解析（无缺失）。

`nm -u` 汇总：28 个对象共 362 个不同未定义符号，其中 304 个是 Lua 内部跨编译单元符号
（`luaC_*`/`luaD_*`/`luaH_*`/…，同目录内互相解析），58 个是外部符号：55 个 C 标准库
符号（见下表）+ `_ctype`/`_errno`/`snprintf_` 三个本仓库 `libsky.so` 自有符号。

同一批次还做了**独立集成验证**（全部编译产物只写 `/tmp`，不触碰 `programs/object`）：
把上述 28 个 Lua 对象 + desktop 全部 C++/`init.S` 重新编译后，用
`ld -entry=_start -pie -m elf_x86_64 -z noexecstack --no-dynamic-linker … --no-as-needed
-L../../lib/bin -l:libsky.so` 成功链接出 `DYN`(PIE) 的 desktop 可执行文件（`NEEDED
libsky.so`，当前无 `PT_INTERP` 的内核动态加载模型），151 个 Lua API 符号进入最终 ELF；
`nm -u` 剩余项全部是运行期由 `libsky.so` 解析的导入符号。即：在 `lib/stdc` 就绪后，
本项目对 Lua 的 vendor + 构建接线已可完整编译链接（QEMU 运行验证由 Lead 负责）。

### libc 依赖（`nm -u` 汇总，C 标准部分）

这些符号必须由 `libsky.so`（`lib/stdc`）提供/导出；下表即 Lua 编译产物 `nm -u` 中
全部 C 标准库外部符号的按头文件归类（编译期声明已由 `lib/include/stdc` 提供；
链接期实现由 `lib/stdc` 提供，构建时 `make -C lib` 重建 `libsky.so` 即可）：

| 头文件 | 符号 | 主要使用者 |
| --- | --- | --- |
| `stdio.h` | `FILE`, `fopen/fread/fclose/fseek`, `fwrite/fflush/fprintf`, `feof/ferror`, `freopen`, `fgets`, `getc`, `stdin/stdout/stderr`, `BUFSIZ` | lauxlib.c, lbaselib.c, ldblib.c |
| `string.h` | `strlen/strcmp/strncmp/strcpy/strchr`, `strstr/strspn/strpbrk`, `strerror`, `strcoll`, `memchr/memcmp/memcpy` | 多个编译单元 |
| `stdlib.h` | `abort`, `abs`, `free/realloc`, `strtod` | ldo.c, lcode.c, lauxlib.c, lobject.c |
| `math.h` | `sin/cos/tan/asin/acos/atan2`, `exp/log/log2/log10`, `sqrt/pow/fmod/floor/ceil/fabs`, `frexp/ldexp/modf`, `HUGE_VAL` | lmathlib.c, ltable.c, lcode.c, lvm.c, lobject.c, lstrlib.c |
| `time.h` | `time`, `clock` | lmathlib.c, lstate.c, ltablib.c |
| `locale.h` | `localeconv`（`struct lconv.decimal_point`） | lobject.c, lstrlib.c |
| `setjmp.h` | `setjmp`, `longjmp`, `jmp_buf` | ldo.c |
| `signal.h` | `sig_atomic_t`（仅类型） | lstate.h |

非 C 标准符号：`_ctype`、`_errno`、`snprintf_` 是本仓库 `libsky.so` 自有的数据/函数符号
（`ctype.h` 宏、`errno`、`snprintf` 宏的目标），不是外部 libc 缺口。

> 注：`sinh/cosh/tanh` 等不在 Lua 5.4.7 上述编译单元的实际引用清单内。

## 审计方法

```sh
# 1) 校验本目录未被改动
cd programs/desktop/lua && sha256sum -c SHA256SUMS

# 2) 与上游逐字节比对
cd /tmp && curl -sLO https://www.lua.org/ftp/lua-5.4.7.tar.gz
echo "9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30  lua-5.4.7.tar.gz" | sha256sum -c -
tar xzf lua-5.4.7.tar.gz
cd lua-5.4.7/src && sha256sum -c /path/to/SkylineSystem/programs/desktop/lua/SHA256SUMS
```

升级 Lua 版本时的流程：下载新 tarball → 校验 SHA-256 → 按上述文件清单 `cp` 覆盖 →
更新本 README（版本/哈希/日期）→ 重新生成 `SHA256SUMS` → 用 desktop flags 重跑全文编译
验证。**不要手工编辑 vendored 文件；如确需 patch，只允许最小改动并在本文件"patch 列表"记录。**

## 已知限制（来自 `lib/stdc` 实现，不影响编译/链接）

- 本仓库 printf 实现不支持 `%a`（十六进制浮点输出）：影响 `string.format("%a")` 与
  `string.format("%q", <非整数>)` 的浮点序列化（`lstrlib.c` 的 `lua_number2strx`）；
  普通数值输出（`tostring`/`"%.14g"`）与 `strtod` 的十六进制输入正常。
- 桌面环境 `stdin` 恒为 EOF：`luaL_loadfile(L, NULL)`、debug 库交互提示等会立即失败/退出
  而不阻塞；内嵌脚本应从文件或字符串加载。
- `stdout`/`stderr` 在动态链接主程序中是 COPY 重定位（内核加载器已支持，见
  `docs/dynamic-linking-plan.md`）；`print`/`lua_writestring` 输出到桌面控制台。

## 风险 / 依赖

- Lua 源码本身无 patch、可审计；编译/链接依赖 `lib/stdc` 的实现质量（`setjmp/longjmp`
  的 x86_64 ABI、`localeconv` C locale 语义、`strtod`/`snprintf` 的数值行为）。
- 动态链接下上述 libc 符号需从 `libsky.so` 导出；`_ctype`/`_errno`/`snprintf_`/`strtod`
  当前已验证在 `libsky.so` 动态符号表中。
- 本目录不提供 `io`/`os`/`package`/`loadlib` 能力，Lua 脚本只能使用宿主显式注册的库。
