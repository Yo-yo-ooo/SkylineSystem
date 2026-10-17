//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * elf_dyn.cpp —— 最小 ELF 动态链接器 (内核态, 加载期重定位)
 * ----------------------------------------------------------------------------
 * 目标: 让用户态程序不再各自静态链一份 libc, 而是共享一份 /mp/libsky.so。
 *
 * 设计取舍 (为什么"内核直接加载"而不是用户态 ld.so):
 *   * 链接时用 `ld --no-dynamic-linker` 生成主程序 —— 这样 ELF 里**没有**
 *     PT_INTERP, 内核不需要先把解释器跑起来再回头加载主程序 (那要求主程序
 *     自己能在未重定位的状态下执行, 复杂度高得多);
 *   * 主程序保持 ET_EXEC (固定地址 0x400000), 内核只需给 .so 选一个基址;
 *   * 全部在加载期解析 (等价 LD_BIND_NOW), 不做 PLT 延迟绑定 ——
 *     实现简单, 而且失败一定发生在"进程还没开始跑"的时候, 好定位。
 *
 * 支持的重定位 (足够覆盖 ld 为我们的用户态产生的全部种类):
 *   .so  自身: R_X86_64_RELATIVE / GLOB_DAT / JUMP_SLOT / TPOFF64
 *   主程序   : 同上 (JUMP_SLOT / GLOB_DAT 指向 .so 的导出符号)
 * 其它类型一律**拒绝加载并打印**, 绝不放一个未重定位的槽进用户态。
 *
 * TLS: libsky.so 带 STATIC_TLS (非零初值的 __thread 已改成零初值, 见
 *   lib/stdc/algo/rand.c), 所以 .so 的 TLS 必须与主程序的 TLS 一起放进
 *   同一个线程块 (variant II)。做法: 把 .so 的 TLS 段放在块首, 主程序 TLS
 *   顺延 —— exec.cpp 里 tls_offset/memsz 传入的正是"顺延后的偏移", 于是
 *   %fs 基址与 TPOFF 公式天然一致。
 * ==========================================================================*/
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/syscall.h>
#include <klib/errno.h>
#include <elf/elf.h>
#include <mem/pmm.h>
#include <arch/x86_64/cpu/smap.h>   // SmapGuard

/* elf.h 把类型放在 Elf64 命名空间里; 本文件里用到很多次, 统一引入 */
using Elf64::Elf64_Ehdr;
using Elf64::Elf64_Phdr;
using Elf64::Elf64_Dyn;
using Elf64::Elf64_Sym;
using Elf64::Elf64_Rela;

#include <klib/str.h>
#define __strcpy strcpy
#define __strcmp strcmp
/* 注意: 内核的 _memcpy 是**转置序** (src, dest, size), 与 memcpy 相反。
   本文件统一用 __memcpy 的正常顺序, 不要用 _memcpy。 */

/* 读取 DT_NEEDED 指向的共享库需要文件系统接口 */
#include <fs/fc.h>
#include <fs/fd.h>

#define DL_ENABLE_LOGS 1

/* ---------- ELF 动态段常量 (kernel/include/elf/elf.h 未定义这些) ---------- */
#define DT_NULL            0
#define DT_NEEDED          1
#define DT_PLTRELSZ        2
#define DT_HASH            4
#define DT_STRTAB          5
#define DT_SYMTAB          6
#define DT_RELA            7
#define DT_RELASZ          8
#define DT_RELAENT         9
#define DT_STRSZ           10
#define DT_SYMENT          11
#define DT_PLTREL          20
#define DT_JMPREL          23
#define DT_RELACOUNT       0x6ffffff9ULL
#define DT_FLAGS           30
#define DT_FLAGS_1         0x6ffffffbULL
#define DF_STATIC_TLS      0x10

#define R_X86_64_GLOB_DAT  6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_COPY      5
#define R_X86_64_TPOFF64   18

#define STB_LOCAL          0
#define STB_GLOBAL         1
#define STB_WEAK           2

/* 共享库的固定加载基址。选在用户栈 (高地址) 与主程序 (0x400000) 之间,
 * 远离二者, 也远离 PIE 的随机区间 [0x10000000, 0x40000000)。 */
#define DL_SO_BASE         0x0000700000000000ULL
#define DL_MAX_DEPS        4

/* 导出符号条目。
 * 名字**必须复制进来**, 不能只存指向 .so 里字符串表的指针 —— 那个字符串表
 * 在目标进程的地址空间里, 重定位结束、页表切回内核之后再去解引用就会以
 * (CS=0x8, CR2=0x700000000038) 的形式把内核打崩 (实测踩过)。
 * 名字内联存放在条目里 (而不是再单独分配字符串池): 一条记录一次分配,
 * 增长时整块 realloc, 不会产生碎片, 查找也不需要二次间接。
 * 长名截断 —— C++ 修饰名可能很长, 56 字节足够区分我们导出的符号。 */
#define DL_NAME_MAX      56
#define DL_EXPORTS_INIT  256      /* 初始容量 (libsky.so 实测 287 个导出) */

struct dl_sym {
    uint32_t hash;
    uint64_t value;
    char     name[DL_NAME_MAX];
};

struct dl_obj {
    char     name[64];
    uint64_t base;          /* 运行期基址: ET_DYN 的 load_bias; ET_EXEC 为 0 */
    uint64_t bias;          /* 加到"链接期虚拟地址"上得到运行期地址的增量 */
    uint64_t img;           /* 该对象 ELF 镜像在内核里的地址 (仅主程序用) */
    uint64_t img_len;
    uint64_t symtab, strtab, syment;
    uint64_t nsym;                               /* .dynsym 条目数 (COPY 用) */
    uint64_t rela, relasz, relaent, relacount, jmprel, pltrelsz;
    uint64_t tls_memsz, tls_filesz, tls_align;   /* 来自 PT_TLS */
    uint64_t tls_src_off;                        /* PT_TLS 在文件里的偏移 */
    uint64_t mapped_size;                        /* PT_LOAD 覆盖的实际字节数 */
    /* 导出表: **动态分配 + 按需增长**。
       原来的固定 `dl_sym exports[512]` 有两个毛病:
         1) libsky.so 现在就有 287 个导出, 余量不足一倍; 一旦超过 512 会
            **静默丢弃**后续符号, 表现成"某个符号解析不到"的诡异链接错误;
         2) 512 × 72B ≈ 36KB/对象, 5 个对象就是 180KB 常驻 BSS, 白占内存。
       现在改为 kmalloc 增长 (容量翻倍), 用完 kfree。 */
    dl_sym  *exports;
    uint32_t nexports;
    uint32_t exports_cap;
    uint64_t hash_tab;      /* DT_HASH 的**链接期虚拟地址** */
    bool     is_so;         /* true = 共享库 (用于区分 TPOFF 修正) */
};

/* ------------------------------------------------------------------ */
/* 小工具                                                             */
/* ------------------------------------------------------------------ */
static inline uint64_t dl_align_down(uint64_t v, uint64_t a) { return v & ~(a - 1); }
static inline uint64_t dl_align_up(uint64_t v, uint64_t a) {
    return a ? ((v + a - 1) & ~(a - 1)) : v;
}

/* GNU/SysV ELF 符号名哈希 (与 ld 的 --hash-style=sysv 一致) */
static uint32_t dl_elf_hash(const char *s) {
    uint32_t h = 0, g;
    while (*s) {
        h = (h << 4) + (uint8_t)*s++;
        g = h & 0xF0000000u;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

/* 模块级共享状态 (文件作用域, elf_dyn_prepare 由 static 保护串行调用)。
   tls_so_base = .so 的 TLS 在合并块里的起始偏移。为什么需要它:
   - .so 用的是 initial-exec/GOTTPOFF, 它的 GOT 槽里放的是 TPOFF
     (相对**线程指针**的偏移, Variant II 下为负), 加载器可以自由修正;
   - **主程序**用的是 local-exec (编译器把偏移直接编进指令), 所以主程序的
     TLS 必须落在偏移 0 —— 否则它的 `%fs:off` 会读到 .so 的数据。
     实测: 桌面 (PIE, local-exec) 的 qsbr_slot 读到的是 .so 的 tls_data,
     于是 malloc 走了错误支路并崩溃。
   因此布局固定为 [ 主程序 TLS | .so TLS ]。 */
static uint64_t g_tls_so_base = 0;

/* g_tls_fs_off = 线程指针相对 tls_mem 的偏移, 即 ALIGN_UP(合并后总大小, align)。
 * TPOFF64 的正确算法是:
 *     TPOFF = (变量在块内的偏移) - (线程指针在块内的偏移)
 *           = (g_tls_so_base + addend) - g_tls_fs_off
 * 实测踩过的错误写法是 `addend - g_tls_so_base`: 它给出 -128, 而正确值是
 * -960 (so_base=128, 总大小=1088)。差 832 字节, 于是 `%fs + (-128)` 落进了
 * `tls_large_cache` 数组中间 —— 分配器的 `qsbr_slot` 与"某个缓存堆指针"共用
 * 同一块内存, 读出来的 slot 就是那个指针的低 32 位, malloc 于是走了错误支路
 * 并在 `lock xadd` 上崩。这个 bug 伪装成"TLS 被写坏", 查了好几轮。 */
static uint64_t g_tls_fs_off = 0;

/* 清空"指向镜像内部"的指针字段。dl_parse_dynamic 可能被调用两次 (先在内核
 * 缓冲上取 PT_TLS/DT_NEEDED, 再在目标进程页表里正式解析), 第二次之前必须
 * 把这些字段清掉 —— 否则第一次留下的"内核缓冲地址"会在用户页表下被解引用,
 * 直接内核 #PF (实测踩过: CS=0x8, CR2=0x700000000038)。 */
static void dl_reset_image_refs(dl_obj *o) {
    o->symtab = o->strtab = 0;
    o->rela = o->relasz = o->relaent = o->relacount = 0;
    o->jmprel = o->pltrelsz = 0;
    o->nsym = 0;
    /* 只清空"已用条数", **保留已分配的缓冲区** —— dl_parse_dynamic 会被调用
       两次 (先在内核缓冲上取 PT_TLS, 再在目标页表里正式解析), 保留容量可以
       让第二次不用重新 kmalloc, 也不会泄漏。缓冲区最终由 dl_exports_free 回收。 */
    o->nexports = 0;
}

/* 把"动态段里的地址"折算成运行期虚拟地址。
 * 关键点: **不要**用"小于 mapped_size 就当偏移"这种启发式 —— 实测 PIE 主程序
 * 的 DT_STRTAB/DT_RELA 是 0x40xxxx 这样的链接期虚拟地址, 而 mapped_size 只有
 * 几百 KB, 于是会被误判成偏移, 重定位就会往**文件偏移**对应的地址写, 直接把
 * 镜像写坏 (表现为用户态跳到 0x20B6 这种垃圾地址且没有任何日志)。
 * 正确做法: 一律按链接期虚拟地址处理, 加上对象的 bias。 */
static inline uint64_t dl_va(dl_obj *o, uint64_t v) { return v ? (o->bias + v) : 0; }

/* 链接期虚拟地址 -> 文件内偏移 (ELF 镜像在内存里, 只有主程序用得到) */
static uint64_t dl_vaddr_to_off(uint64_t img, uint64_t img_len, uint64_t va) {
    if (img_len < sizeof(Elf64_Ehdr)) return ~0ULL;
    Elf64_Ehdr *eh = (Elf64_Ehdr*)img;
    if (eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(Elf64_Phdr) > img_len) return ~0ULL;
    Elf64_Phdr *ph = (Elf64_Phdr*)(img + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != 1 /* PT_LOAD */) continue;
        if (va >= ph[i].p_vaddr && va < ph[i].p_vaddr + ph[i].p_filesz)
            return ph[i].p_offset + (va - ph[i].p_vaddr);
    }
    /* 也可能直接就是文件偏移 (ET_EXEC 且链接脚本如此) */
    if (va < img_len) return va;
    return ~0ULL;
}

/* 确保导出表至少能放下 need 条记录。返回 0 成功, <0 失败 (内存不足)。
 * 增长策略: 容量翻倍, 新块 kmalloc + 拷贝 + 释放旧块。
 * 注意这里跑在**目标进程页表**下 (dl_parse_dynamic 在 SwitchPageMap 之后
 * 调用), 但 kmalloc/kfree 只碰内核堆, 与 CR3 无关, 所以是安全的。 */
static int dl_exports_reserve(dl_obj *o, uint32_t need) {
    if (need <= o->exports_cap) return 0;
    uint32_t cap = o->exports_cap ? o->exports_cap : DL_EXPORTS_INIT;
    while (cap < need) {
        if (cap > 0x100000u) return -ENOMEM;      /* 上限, 防失控 */
        cap <<= 1;
    }
    dl_sym *n = (dl_sym*)kmalloc((uint64_t)cap * sizeof(dl_sym));
    if (!n) return -ENOMEM;
    if (o->exports && o->nexports)
        __memcpy(n, o->exports, (uint64_t)o->nexports * sizeof(dl_sym));
    if (o->exports) kfree(o->exports);
    o->exports = n;
    o->exports_cap = cap;
    return 0;
}

/* 释放某个对象的导出表 */
static void dl_exports_free(dl_obj *o) {
    if (o->exports) { kfree(o->exports); o->exports = nullptr; }
    o->nexports = 0;
    o->exports_cap = 0;
}

static void dl_add_export(dl_obj *o, const char *name, uint64_t value) {
    if (!name || !name[0]) return;
    uint32_t h = dl_elf_hash(name);
    for (uint32_t i = 0; i < o->nexports; i++)
        if (o->exports[i].hash == h && __strcmp(o->exports[i].name, name) == 0) return;
    /* 动态扩容: 满就翻倍, 不再有 512 条上限, 也不会静默丢符号 */
    if (dl_exports_reserve(o, o->nexports + 1) != 0) {
        kerrorln("[dynlink] %s: out of memory growing export table (%u)",
                 o->name, o->nexports);
        return;
    }
    dl_sym *e = &o->exports[o->nexports++];
    e->hash = h;
    e->value = value;
    uint32_t k = 0;
    while (name[k] && k < DL_NAME_MAX - 1) { e->name[k] = name[k]; k++; }
    e->name[k] = 0;
}

static uint64_t dl_lookup(dl_obj *objs, int nobj, const char *name) {
    if (!name || !name[0]) return 0;
    uint32_t h = dl_elf_hash(name);
    for (int k = 0; k < nobj; k++) {
        dl_obj *o = &objs[k];
        if (!o->exports) continue;      /* 该对象没有导出 (例如静态主程序) */
        for (uint32_t i = 0; i < o->nexports; i++)
            if (o->exports[i].hash == h && __strcmp(o->exports[i].name, name) == 0)
                return o->exports[i].value;
    }
    return 0;
}

/* 读取动态段; 同时把导出符号收进 exports[]。
 *
 * 寻址要点 (踩过的坑): DT_* 里的指针是**链接期虚拟地址**。对已映射进用户态
 * 的对象, 运行期地址 = bias + va; 对主程序, 它的 ELF 镜像还在内核缓冲区里,
 * 那些 VA 在**内核**里并不成立, 必须先用 PT_LOAD 反查出文件偏移, 再访问
 * o->img 里的字节。两条路都必须走对, 否则重定位会写坏镜像。 */
static void dl_parse_dynamic(dl_obj *o) {
    /* 取 ELF 头: 主程序在内核缓冲区, .so 在用户地址空间 */
    uint8_t *ehp = o->img ? (uint8_t*)o->img : (uint8_t*)(o->base);
    Elf64_Ehdr *eh = (Elf64_Ehdr*)ehp;
    /* 程序头表地址 = ELF 头地址 + e_phoff。**不能**写成 ehp + e_phoff 之外还
       再叠一次 base/bias —— 对已映射的对象, ehp 已经是运行期地址了。
       (踩过: 少加 bias 时会在 main_bias+0x38 处 #PF, 那正是 e_phoff。) */
    Elf64_Phdr *ph = (Elf64_Phdr*)(ehp + eh->e_phoff);

    auto phdr_va_to_ptr = [&](uint64_t va) -> uint8_t* {
        if (o->img) {
            uint64_t off = dl_vaddr_to_off(o->img, o->img_len, va);
            return (off == ~0ULL) ? nullptr : (uint8_t*)(o->img + off);
        }
        return (uint8_t*)dl_va(o, va);
    };

    Elf64_Dyn *d = nullptr;
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == 2 /* PT_DYNAMIC */)
            d = (Elf64_Dyn*)phdr_va_to_ptr(ph[i].p_vaddr);
    if (!d) return;

    uint64_t dt_strtab = 0, dt_symtab = 0, dt_hash = 0;
    uint64_t dt_rela = 0, dt_relasz = 0, dt_relaent = 24, dt_relacount = 0;
    uint64_t dt_jmprel = 0, dt_pltrelsz = 0;
    for (Elf64_Dyn *e = d; e->d_tag != DT_NULL; e++) {
        switch (e->d_tag) {
        case DT_STRTAB:   dt_strtab   = e->d_un; break;
        case DT_SYMTAB:   dt_symtab   = e->d_un; break;
        case DT_SYMENT:   o->syment   = e->d_un; break;
        case DT_HASH:     dt_hash     = e->d_un; break;
        case DT_RELA:     dt_rela     = e->d_un; break;
        case DT_RELASZ:   dt_relasz   = e->d_un; break;
        case DT_RELAENT:  dt_relaent  = e->d_un; break;
        case DT_RELACOUNT:dt_relacount= e->d_un; break;
        case DT_JMPREL:   dt_jmprel   = e->d_un; break;
        case DT_PLTRELSZ: dt_pltrelsz = e->d_un; break;
        default: break;
        }
    }
    if (o->syment == 0) o->syment = 24;

    o->strtab    = (uint64_t)phdr_va_to_ptr(dt_strtab);
    o->symtab    = (uint64_t)phdr_va_to_ptr(dt_symtab);
    o->rela      = (uint64_t)phdr_va_to_ptr(dt_rela);
    o->relasz    = dt_relasz;
    o->relaent   = dt_relaent;
    o->relacount = dt_relacount;
    o->jmprel    = (uint64_t)phdr_va_to_ptr(dt_jmprel);
    o->pltrelsz  = dt_pltrelsz;

    if (!o->symtab || !o->strtab) {
        kerrorln("[dynlink] %s: no symtab/strtab (strtab=0x%llx symtab=0x%llx)",
                 o->name, (unsigned long long)o->strtab, (unsigned long long)o->symtab);
        return;
    }

    /* 符号表长度: 优先用 DT_HASH 的 nchain, 否则退化为"扫描到空名" */
    uint64_t nsym = 0;
    uint8_t *hp = phdr_va_to_ptr(dt_hash);
    if (hp) {
        uint32_t *h = (uint32_t*)hp;
        nsym = h[1];    /* [0]=nbucket, [1]=nchain(=符号数) */
    }
    if (nsym == 0 || nsym > 65536) {
        Elf64_Sym *s = (Elf64_Sym*)o->symtab;
        for (nsym = 1; nsym < 65536; nsym++)
            if (s[nsym].st_name == 0 && s[nsym].st_value == 0) break;
    }

    Elf64_Sym *syms = (Elf64_Sym*)o->symtab;
    const char *strs = (const char*)o->strtab;
    o->nsym = nsym;
    for (uint64_t i = 0; i < nsym; i++) {
        Elf64_Sym *s = &syms[i];
        if (s->st_shndx == 0) continue;                    /* SHN_UNDEF */
        uint8_t bind = (uint8_t)(s->st_info >> 4);
        if (bind != STB_GLOBAL && bind != STB_WEAK) continue;
        if (s->st_name == 0) continue;
        dl_add_export(o, strs + s->st_name, o->bias + s->st_value);
    }
    /* cap 反映动态分配实际增长到的容量: 初始 256, 本例 287 个导出会翻倍到
       512。以前固定数组时这里恒为 512 且超限静默丢符号; 现在按需增长。 */
    kinfoln("[dynlink] %s: syms=%llu exports=%u cap=%u (dyn alloc, %llu bytes)",
            o->name, (unsigned long long)nsym, o->nexports, o->exports_cap,
            (unsigned long long)((uint64_t)o->exports_cap * sizeof(dl_sym)));
}

/* 应用一个对象自身的重定位。exports 尚未收集完时用 skip_undef 跳过
 * (两趟: 先收集所有导出, 再统一重定位)。 */
static int dl_relocate(dl_obj *o, dl_obj *objs, int nobj, bool collect_only) {
    if (!o->symtab || !o->strtab) return 0;
    Elf64_Sym *syms = (Elf64_Sym*)o->symtab;
    const char *strs = (const char*)o->strtab;

    uint64_t total = o->relasz + o->pltrelsz;
    if (total == 0) return 0;

    /* 两段重定位表连续遍历: .rela.dyn 与 .rela.plt */
    for (int pass = 0; pass < 2; pass++) {
        uint64_t addr = pass == 0 ? o->rela : o->jmprel;
        uint64_t size = pass == 0 ? o->relasz : o->pltrelsz;
        if (!addr || !size) continue;
        for (uint64_t off = 0; off < size; off += o->relaent) {
            Elf64_Rela *r = (Elf64_Rela*)(addr + off);
            uint32_t type = (uint32_t)ELF64_R_TYPE(r->r_info);
            uint32_t si   = (uint32_t)ELF64_R_SYM(r->r_info);
            uint64_t where = o->bias + r->r_offset;
            uint64_t val = 0;

            switch (type) {
            case R_X86_64_RELATIVE:
                val = o->bias + (uint64_t)r->r_addend;
                break;
            case R_X86_64_GLOB_DAT:
            case R_X86_64_JUMP_SLOT: {
                const char *nm = (si && syms[si].st_name) ? strs + syms[si].st_name : "";
                val = dl_lookup(objs, nobj, nm);
                if (val == 0) {
                    if (collect_only) continue;      /* 稍后再试 */
                    kerrorln("[dynlink] %s: unresolved symbol '%s'", o->name, nm);
                    return -ENOENT;
                }
                break;
            }
            case R_X86_64_TPOFF64: {
                /* STATIC_TLS + initial-exec: GOT 槽里放"相对线程指针"的偏移。
                 *
                 * ld 给出的 addend 是**模块内的** TLS 变量偏移 (以该模块的 TLS
                 * 段起点为 0)。要变成运行期可用的偏移, 必须补上"该模块 TLS 在
                 * 合并块里的起始位置", 再减去线程指针在块内的位置:
                 *
                 *     TPOFF = (module_tls_base + addend) - fs_off
                 *
                 * 其中 (Variant II, 线程指针在块尾):
                 *     module_tls_base = .so ? g_tls_so_base : 0
                 *     fs_off          = ALIGN_UP(合并后总大小, align)
                 *
                 * 两个都必须对:
                 *  - 漏掉 fs_off 那一项 (写成 addend - so_base) => 偏 832 字节,
                 *    `%fs+TPOFF` 落进 tls_large_cache[] 中间, 分配器的
                 *    qsbr_slot 与缓存指针共用内存, malloc 当场崩;
                 *  - 对主程序也加 module_tls_base => 主程序的 __thread 变量
                 *    整体高 g_tls_so_base 字节, 会读到 .so 的数据。
                 * 两者都是"每一层单独看都对"的那种 bug, 所以这里按模块区分,
                 * 不图省事。 */
                uint64_t module_tls_base = o->is_so ? g_tls_so_base : 0;
                val = (module_tls_base + (uint64_t)r->r_addend) - g_tls_fs_off;
                break;
            }
            case R_X86_64_COPY: {
                /* COPY 重定位: 主程序里为 .so 的某个大对象保留了一份存储
                 * (常见于非 PIE 主程序引用 .so 的全局变量), 需要在加载期把
                 * 数据从 .so 拷进主程序的 BSS 里。r_addend/sym 的 st_size
                 * 给出要拷贝的字节数。
                 * 注意: 完整 ELF 语义还要求 .so 内部对该符号的引用也改指
                 * 主程序那份 (否则出现两份状态)。我们已经按加载顺序先重定位
                 * 主程序、再重定位 .so —— 见 dl_relocate 的调用顺序 —— 但这
                 * 只对"第二次查找时能找到主程序导出"的情况成立。为安全起见
                 * 这里只做数据拷贝, 并把 COPY 的数量打印出来: 一旦用户态
                 * 出现"全局状态有两份"的怪现象, 先查这个数字。 */
                uint64_t sz = (si && si < o->nsym) ? syms[si].st_size : 0;
                const char *nm = (si && syms[si].st_name) ? strs + syms[si].st_name : "";
                if (sz == 0 || sz > (1u << 20)) {
                    kerrorln("[dynlink] %s: bad COPY size %llu for '%s'",
                             o->name, (unsigned long long)sz, nm);
                    return -EINVAL;
                }
                uint64_t src = dl_lookup(objs, nobj, nm);
                if (src == 0) {
                    if (collect_only) continue;
                    kerrorln("[dynlink] %s: COPY symbol '%s' unresolved", o->name, nm);
                    return -ENOENT;
                }
                if (!collect_only) {
                    SmapGuard ug;
                    __memcpy((void*)where, (void*)src, sz);
                    kinfoln("[dynlink] %s: COPY '%s' %llu bytes",
                            o->name, nm, (unsigned long long)sz);
                }
                continue;                       /* 已自行写入, 不走通用写回 */
            }
            default:
                kerrorln("[dynlink] %s: unsupported reloc type %u at 0x%llx",
                         o->name, type, (unsigned long long)r->r_offset);
                return -ENOTSUP;
            }
            if (collect_only && type != R_X86_64_RELATIVE) continue;
            { SmapGuard ug; *(uint64_t*)where = val; }
        }
    }
    return 0;
}

/* 把 .so 的 PT_LOAD 段映射进 pagemap, 返回 0 成功 */
static int dl_map_segments(dl_obj *o, uint8_t *img, uint64_t len, pagemap_t *pm) {
    Elf64_Ehdr *eh = (Elf64_Ehdr*)img;
    if (len < sizeof(Elf64_Ehdr) || eh->e_ident.c[0] != 0x7f) return -EINVAL;
    if (eh->e_type != 3 /* ET_DYN */) {
        kerrorln("[dynlink] %s: not ET_DYN (type=%d)", o->name, eh->e_type);
        return -EINVAL;
    }
    if (eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(Elf64_Phdr) > len) return -EINVAL;

    uint64_t min_vaddr = ~0ULL, max_vaddr = 0;
    Elf64_Phdr *ph = (Elf64_Phdr*)(img + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != 1 /* PT_LOAD */) continue;
        if (ph[i].p_vaddr < min_vaddr) min_vaddr = ph[i].p_vaddr;
        uint64_t e = ph[i].p_vaddr + ph[i].p_memsz;
        if (e > max_vaddr) max_vaddr = e;
    }
    if (min_vaddr == ~0ULL || max_vaddr == 0) return -EINVAL;

    min_vaddr = dl_align_down(min_vaddr, 0x1000);
    max_vaddr = dl_align_up(max_vaddr, 0x1000);

    o->base = DL_SO_BASE;
    o->bias = o->base - min_vaddr;

    pagemap_t *restore_pm = VMM::SwitchPageMap(pm);
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != 1) continue;
        uint64_t filesz = ph[i].p_filesz > ph[i].p_memsz ? ph[i].p_memsz : ph[i].p_filesz;
        if (ph[i].p_offset > len || filesz > len - ph[i].p_offset) {
            VMM::SwitchPageMap(restore_pm ? restore_pm : (pagemap_t*)kernel_pagemap);
            return -EINVAL;
        }
        uint64_t va  = ph[i].p_vaddr + o->bias;
        uint64_t end = dl_align_up(va + ph[i].p_memsz, 0x1000);
        uint64_t start = dl_align_down(va, 0x1000);
        uint64_t flags = MM_READ | MM_WRITE | MM_USER;
        for (uint64_t p = start; p < end; p += 0x1000) {
            if (VMM::GetPhysics(pm, p)) continue;
            uint64_t pg = (uint64_t)PMM::Request();
            if (!pg) {
                VMM::SwitchPageMap(restore_pm ? restore_pm : (pagemap_t*)kernel_pagemap);
                return -ENOMEM;
            }
            VMM::Map(pm, p, pg, flags);
        }
        VMM::NewMapping(pm, start, (end - start) / 0x1000, flags);
        VMM::VMA::AddRegion(pm, start, (end - start) / 0x1000, flags);
        { SmapGuard ug;
          __memcpy((void*)va, (void*)(img + ph[i].p_offset), filesz);
          if (ph[i].p_memsz > filesz)
              _memset((void*)(va + filesz), 0, ph[i].p_memsz - filesz);
        }
        /* W^X 收紧 (与 exec.cpp 同规则) */
        uint64_t prot = MM_READ | MM_USER |
                        ((ph[i].p_flags & 2) && !(ph[i].p_flags & 1) ? MM_WRITE : 0) |
                        ((ph[i].p_flags & 1) ? 0 : MM_NX);
        for (uint64_t p = start; p < end; p += 0x1000) {
            VMM::Internal::PageInfo pi = VMM::Internal::GetPageInfo(pm, p);
            if (pi.size == PAGE_2MB) {
                VMM::Map2M(pm, p, pi.phys, prot);
                __asm__ volatile("invlpg (%0)" : : "r"(p) : "memory");
                p += PAGE_2MB - 0x1000;
            } else if (pi.size == 0x1000) {
                VMM::Map4K(pm, p, pi.phys, prot);
                __asm__ volatile("invlpg (%0)" : : "r"(p) : "memory");
            }
        }
    }
    VMM::SwitchPageMap(restore_pm ? restore_pm : (pagemap_t*)kernel_pagemap);

    kinfoln("[dynlink] mapped %s at 0x%llx..0x%llx (bias 0x%llx)",
            o->name, (unsigned long long)o->base,
            (unsigned long long)(o->base + (max_vaddr - min_vaddr)),
            (unsigned long long)o->bias);
    o->mapped_size = max_vaddr - min_vaddr;   /* 供 dl_vaddr 判断 d_ptr 形态 */
    return 0;
}

/* ==================================================================
 * 统一入口 (1): 读取主程序 DT_NEEDED 指向的共享库
 * ------------------------------------------------------------------
 * 主程序通过 DT_NEEDED 声明它需要哪个 .so。内核按名字从 /mp/ 读出来。
 * 不做搜索路径 —— 镜像里 .so 就在根目录 (disk.img:/libsky.so), 名字与
 * DT_NEEDED 里的 SONAME 一致, 直接拼 /mp/<name> 即可; 找不到就是硬错误
 * (宁可启动失败, 也不要放一个未重定位的程序进用户态)。
 *
 * 返回: >0 成功 (已读入 *buf, *out_len 为长度), 0 = 主程序没有 DT_NEEDED,
 *       <0 失败。*buf 由调用方 kfree。
 * ================================================================== */
static int dyn_read_needed(uint8_t *main_img, uint64_t main_len,
                           uint8_t **buf, uint64_t *out_len,
                           char *name_out, uint64_t name_cap) {
    *buf = nullptr; *out_len = 0;
    if (name_out && name_cap) name_out[0] = 0;
    if (main_len < sizeof(Elf64_Ehdr)) return 0;
    Elf64_Ehdr *eh = (Elf64_Ehdr*)main_img;
    if (eh->e_ident.c[0] != 0x7f || eh->e_phoff == 0) return 0;
    if (eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(Elf64_Phdr) > main_len) return 0;

    Elf64_Phdr *ph = (Elf64_Phdr*)(main_img + eh->e_phoff);
    Elf64_Dyn *dyn = nullptr;
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == 2 /* PT_DYNAMIC */)
            dyn = (Elf64_Dyn*)(main_img + ph[i].p_offset);
    if (!dyn) return 0;                         /* 静态链接的程序: 正常情况 */

    uint64_t needed_off = 0, strtab = 0, strsz = 0;
    for (Elf64_Dyn *d = dyn; ; d++) {
        if ((uint8_t*)d + sizeof(Elf64_Dyn) > main_img + main_len) return 0;
        if (d->d_tag == DT_NULL) break;
        if (d->d_tag == DT_NEEDED && needed_off == 0) needed_off = d->d_un;
        else if (d->d_tag == DT_STRTAB && strtab == 0) strtab = d->d_un;
        else if (d->d_tag == DT_STRSZ) strsz = d->d_un;
    }
    if (!needed_off || !strtab || !strsz) return 0;

    /* DT_STRTAB 是**链接期虚拟地址** (PIE 主程序里形如 0x40xxxx), 不是文件
       偏移 —— 必须先用 PT_LOAD 反查文件偏移, 否则会把随机的镜像字节当字符串
       (实测表现: 完全读不到 DT_NEEDED, 动态链接静默不生效, 没有任何日志)。 */
    uint64_t str_off = dl_vaddr_to_off((uint64_t)main_img, main_len, strtab);
    if (str_off == ~0ULL) return 0;
    if (str_off >= main_len) return 0;
    uint64_t str_end = main_len - str_off;
    if (strsz && strsz < str_end) str_end = strsz;
    if (needed_off >= str_end) return 0;
    const char *nm = (const char*)(main_img + str_off + needed_off);

    uint64_t n = 0;
    while (n + 1 < name_cap && nm[n] &&
           (uint64_t)(nm + n - (const char*)main_img) < main_len) {
        name_out[n] = nm[n]; n++;
    }
    name_out[n] = 0;
    if (n == 0) return 0;

    /* ---- 拼 /mp/<name> 并读文件 ---- */
    char path[128];
    const char *pre = "/mp/";
    uint64_t k = 0;
    while (pre[k] && k < sizeof(path) - 1) { path[k] = pre[k]; k++; }
    for (uint64_t i = 0; i < n && k < sizeof(path) - 1; i++) path[k++] = name_out[i];
    path[k] = 0;

    __hmap_s_mp *MP = GetMount(path);
    if (!MP) { kerrorln("[dynlink] cannot resolve mount for '%s'", path); return -EINVAL; }
    void *FileDesc = kmalloc(MP->FSOPS->SIZEOF_FILE_DESC);
    if (!FileDesc) return -ENOMEM;
    _memset(FileDesc, 0, MP->FSOPS->SIZEOF_FILE_DESC);
    if (MP->FSOPS->open(FileDesc, path, O_RDONLY) != 0) {
        kfree(FileDesc);
        kerrorln("[dynlink] cannot open '%s' (DT_NEEDED '%s')", path, name_out);
        return -ENOENT;
    }
    uint64_t sz = MP->FSOPS->fsize(FileDesc);
    if (sz < sizeof(Elf64_Ehdr)) {
        MP->FSOPS->close(FileDesc); kfree(FileDesc);
        kerrorln("[dynlink] '%s' too small (%llu)", path, (unsigned long long)sz);
        return -EINVAL;
    }
    uint8_t *b = (uint8_t*)kmalloc(sz);
    if (!b) { MP->FSOPS->close(FileDesc); kfree(FileDesc); return -ENOMEM; }
    MP->FSOPS->read(FileDesc, b, sz, 0);
    MP->FSOPS->close(FileDesc);
    kfree(FileDesc);
    *buf = b; *out_len = sz;
    kinfoln("[dynlink] DT_NEEDED '%s' -> %s (%llu bytes)",
            name_out, path, (unsigned long long)sz);
    return 1;
}

/* ------------------------------------------------------------------ */
/* 对外的唯一入口                                                     */
/* ------------------------------------------------------------------ */
/* 在 elf_load_impl 之后调用, 完成 .so 的加载与全部重定位。
 *
 *   main_img/main_len : 主程序 ELF 原始字节 (只读, 函数内不改写)
 *   pm                : 目标进程 pagemap
 *   tls_*             : 传入主程序 TLS 描述; 若 .so 也带 TLS, 就地改写为
 *                       "合并后"的描述, 并把 *tls_main_off 设为"主程序 TLS
 *                       数据在合并块中的偏移"(调用方据此摆放数据)
 *   so_img/so_len     : /mp/libsky.so 的原始字节 (可为 NULL -> 不做动态链接)
 *
 * 返回 0 = 成功 (含"本来就没有动态段"的情况)。 */
int elf_dyn_prepare(uint8_t *main_img, uint64_t main_len, pagemap_t *pm,
                    uint64_t *tls_offset, uint64_t *tls_memsz,
                    uint64_t *tls_filesz, uint64_t *tls_align,
                    uint8_t *so_img, uint64_t so_len, const char *so_name,
                    uint64_t *tls_main_off,
                    uint8_t *so_tls_out, uint64_t so_tls_out_cap,
                    uint64_t main_bias) {
    if (tls_main_off) *tls_main_off = 0;
    /* 对象表较大 (每个 dl_obj 带 512 条导出, 共几十 KB), 放栈上不安全 ——
       内核栈只有几页。用文件级 static: elf_dyn_prepare 只在进程加载期被调用,
       而加载是在调用者上下文里同步完成的, 不存在并发进入。
       (若将来要在运行期并发 dlopen, 这里必须改成加锁或改成堆分配。) */
    static dl_obj objs[DL_MAX_DEPS + 1];
    _memset(objs, 0, sizeof(objs));
    int nobj = 0;

    /* 导出表是 kmalloc 出来的, 每条退出路径都必须回收 —— 内核堆没有"进程
       退出自动清账"这回事, 漏一次就是每个进程泄漏几十 KB。
       objs[] 本身是 static, 所以这里用 lambda 统一处理, 不依赖作用域析构。 */
    auto dl_cleanup = [&]() {
        for (int i = 0; i < nobj; i++) dl_exports_free(&objs[i]);
    };

    /* ---- 主程序 (索引 0) ----
     * PIE 主程序的链接期地址要加 main_bias 才是运行期地址 (实测 kernel 给的
     * bias 形如 0x19368cd000); 非 PIE 的 ET_EXEC 则 main_bias==0。
     * o->img 指向内核里的 ELF 镜像 —— 主程序的 DT_* 虚拟地址在内核态并不成立,
     * 必须靠 PT_LOAD 反查文件偏移 (见 dl_parse_dynamic)。 */
    dl_obj *mo = &objs[nobj++];
    mo->base = main_bias;
    mo->bias = main_bias;
    mo->img = (uint64_t)main_img;
    mo->img_len = main_len;
    __strcpy(mo->name, "main");
    mo->tls_memsz = (tls_memsz ? *tls_memsz : 0);
    mo->tls_filesz = (tls_filesz ? *tls_filesz : 0);
    mo->tls_align = (tls_align && *tls_align ? *tls_align : 8);
    dl_parse_dynamic(mo);
    kinfoln("[dynlink] main: bias=0x%llx exports=%u syms=%llu",
            (unsigned long long)main_bias, mo->nexports,
            (unsigned long long)mo->nsym);

    /* ---- 共享库 ---- */
    bool have_so = false;
    if (so_img && so_len) {
        dl_obj *so = &objs[nobj];
        _memset(so, 0, sizeof(*so));
        so->tls_align = 8;
        so->is_so = true;              /* 用于 TPOFF64 的 so_base 修正 */
        uint64_t n = 0;
        for (const char *p = so_name; *p && n < 63; p++) so->name[n++] = *p;
        so->name[n] = 0;

        Elf64_Ehdr *seh = (Elf64_Ehdr*)so_img;
        if (so_len < sizeof(Elf64_Ehdr) ||
            seh->e_phoff + (uint64_t)seh->e_phnum * sizeof(Elf64_Phdr) > so_len) {
            kerrorln("[dynlink] %s: bad ELF header", so->name);
            dl_cleanup();
            return -EINVAL;
        }
        Elf64_Phdr *sph = (Elf64_Phdr*)(so_img + seh->e_phoff);
        auto so_read_tls = [&]() {
            so->tls_memsz = 0; so->tls_filesz = 0; so->tls_align = 8;
            so->tls_src_off = 0;
            for (int i = 0; i < seh->e_phnum; i++)
                if (sph[i].p_type == 7 /* PT_TLS */) {
                    so->tls_memsz   = sph[i].p_memsz;
                    so->tls_filesz  = sph[i].p_filesz;
                    so->tls_align   = sph[i].p_align ? sph[i].p_align : 8;
                    so->tls_src_off = sph[i].p_offset;
                }
        };
        so_read_tls();

        if (dl_map_segments(so, so_img, so_len, pm) != 0) {
            kerrorln("[dynlink] failed to map %s", so->name);
            dl_cleanup();
            return -EINVAL;
        }
        so_read_tls();                       /* 重取 PT_TLS (映射逻辑不碰它, 保险) */
        so->mapped_size = so->mapped_size;   /* 保留 dl_map_segments 记下的值 */
        have_so = true;
        nobj++;
    }

    /* ==================================================================
     * 从这里开始**整段都在目标进程的地址空间里**执行。
     * --------------------------------------------------------------
     * .so 的 PT_LOAD 段是映射进 pm 的, 内核当前 CR3 里没有 0x7000_0000_0000
     * 这些地址 —— 在外部解引用会以 (CS=0x8, CR2=0x700000000038) 直接把内核
     * 打崩。实测踩过两次: 一次是 dl_parse_dynamic, 一次是 dl_relocate。
     * 所以解析 + 两趟重定位必须全部包在这里面; 期间不要做任何可能切走页表的
     * 操作 (kmalloc 等一律提到切换之前)。
     * ================================================================== */
    pagemap_t *restore_pm = VMM::SwitchPageMap(pm);
    /* SMAP: .so 的页是用户页 (MM_USER), 内核读它们的字节必须显式开 AC 位,
       否则就是 #PF。exec.cpp 的 ELF 拷贝也是这么做的。 */
    SmapGuard dyn_ug;

    /* 主程序的动态段: 现在它已经映射在 pm 里了, 直接用运行期地址解析
       (比走内核缓冲 + PT_LOAD 反查更直接, 也顺带验证重定位目标可达)。
       必须先清掉上一次在内核缓冲上解析留下的指针, 见 dl_reset_image_refs。 */
    dl_reset_image_refs(mo);
    mo->img = 0;
    mo->img_len = 0;
    dl_parse_dynamic(mo);
    kinfoln("[dynlink] main: bias=0x%llx exports=%u syms=%llu",
            (unsigned long long)main_bias, mo->nexports,
            (unsigned long long)mo->nsym);

    if (have_so) {
        dl_reset_image_refs(&objs[1]);
        /* 注意: 不要在这里重取 PT_TLS —— so_read_tls 读的是内核里的 so_img,
           与页表无关, 已经取好了; 清空只针对"镜像内部指针"。 */
        dl_parse_dynamic(&objs[1]);
        kinfoln("[dynlink] %s: exports=%u syms=%llu",
                objs[1].name, objs[1].nexports, (unsigned long long)objs[1].nsym);
    }

    /* ==================================================================
     * TLS 布局必须在**重定位之前**定下来
     * --------------------------------------------------------------
     * 原因: TPOFF64 的修正量要参与重定位计算, 而它由"各模块 TLS 多大、
     * 谁在块首"决定 —— 这些信息在解析完 PT_TLS 后就有, 不需要等重定位。
     * 见 dl_relocate 的 R_X86_64_TPOFF64 分支。
     * ================================================================== */

    /* 默认布局: 只有主程序 TLS (没有 .so, 或 .so 不带 TLS)。
       主程序占块首, 线程指针在 ALIGN_UP(main_mem, align)。
       必须**无条件**先算出来 —— 曾经只在".so 有 TLS"的分支里算
       g_tls_fs_off, 于是"主程序有 TLS、.so 没有"时它会保持 0,
       主程序的 TPOFF64 就会整体偏移 ALIGN_UP(main_mem,align) 字节。 */
    {
        uint64_t a = (tls_align && *tls_align > 1) ? *tls_align : 8;
        g_tls_so_base = 0;
        g_tls_fs_off  = dl_align_up(tls_memsz ? *tls_memsz : 0, a);
    }

    if (have_so && objs[1].tls_memsz > 0) {
        uint64_t so_align = objs[1].tls_align > 1 ? objs[1].tls_align : 8;
        uint64_t main_mem = (tls_memsz ? *tls_memsz : 0);
        uint64_t so_mem = objs[1].tls_memsz;
        uint64_t so_file = objs[1].tls_filesz;
        if (tls_align) *tls_align = so_align;
        if (main_mem == 0) {
            /* 主程序没有 TLS: .so 独占整块, 偏移就是它自己的 0 */
            g_tls_so_base = 0;
            if (tls_memsz) *tls_memsz = so_mem + 8;
            if (tls_offset) *tls_offset = 0;
            if (tls_filesz) *tls_filesz = so_file;
            if (tls_main_off) *tls_main_off = 0;
            kinfoln("[dynlink] TLS: only .so (%llu bytes)", (unsigned long long)so_mem);
        } else {
            /* 布局 [ 主程序 TLS | .so TLS ]: 主程序是 local-exec,
               它的偏移不能动, 所以主程序占块首, .so 顺延。 */
            g_tls_so_base = dl_align_up(main_mem, so_align);
            if (tls_memsz) *tls_memsz = g_tls_so_base + so_mem + 8;
            if (tls_main_off) *tls_main_off = 0;
            kinfoln("[dynlink] TLS merged: main=%llu @0, so=%llu @%llu, total=%llu, align=%llu",
                    (unsigned long long)main_mem,
                    (unsigned long long)so_mem,
                    (unsigned long long)g_tls_so_base,
                    (unsigned long long)(tls_memsz ? *tls_memsz : 0),
                    (unsigned long long)so_align);
        }
        /* .so 的 TLS 初始镜像交给调用方 (STATIC_TLS 数据必须先落在块里) */
        if (so_tls_out && so_tls_out_cap >= so_file && so_file > 0) {
            __memcpy(so_tls_out, (void*)(so_img + objs[1].tls_src_off), so_file);
            if (so_tls_out_cap > so_file)
                _memset((uint8_t*)so_tls_out + so_file, 0, so_tls_out_cap - so_file);
        }
        /* 关键: 线程指针相对块首的偏移。消费方 (task.cpp / exec.cpp) 建块时
           用的是 `tcb_base = tls_mem + ALIGN_UP(tls_memsz, tls_align)`, 所以这
           里必须用同一个式子, 否则 TPOFF 会整体偏掉 (实测差 832 字节)。 */
        g_tls_fs_off = dl_align_up(tls_memsz ? *tls_memsz : 0, so_align);
    }

    /* 无论有没有 .so TLS, 都把最终布局打出来 —— 排查 TLS 偏移问题时这是
       第一手资料 (当年就是靠"算出期望地址 vs 实测地址"才看出差 832 字节)。
       `total` 是合并后的 tls_memsz (消费方还会再 ALIGN_UP 一次加 8 字节 TCB)。 */
    kinfoln("[dynlink] TLS layout: total=%llu, so_base=%llu, fs_off=%llu, "
            "main TPOFF@addend0=%lld, so TPOFF@addend0=%lld",
            (unsigned long long)(tls_memsz ? *tls_memsz : 0),
            (unsigned long long)g_tls_so_base,
            (unsigned long long)g_tls_fs_off,
            (long long)(0 - g_tls_fs_off),
            (long long)(g_tls_so_base - g_tls_fs_off));

    /* 第一趟: 只做 RELATIVE (让字符串表/GOT 自身可用) */
    for (int k = 0; k < nobj; k++) {
        if (dl_relocate(&objs[k], objs, nobj, true) != 0) {
            VMM::SwitchPageMap(restore_pm ? restore_pm : (pagemap_t*)kernel_pagemap);
            dl_cleanup();
            return -EINVAL;
        }
    }
    /* 第二趟: 解析跨对象符号 (JUMP_SLOT / GLOB_DAT / TPOFF64 / COPY) */
    for (int k = 0; k < nobj; k++) {
        int r = dl_relocate(&objs[k], objs, nobj, false);
        if (r != 0) {
            VMM::SwitchPageMap(restore_pm ? restore_pm : (pagemap_t*)kernel_pagemap);
            dl_cleanup();
            return r;
        }
    }
    VMM::SwitchPageMap(restore_pm ? restore_pm : (pagemap_t*)kernel_pagemap);
    /* 至此 exports 里的名字都已是内核副本, 后续不再需要目标页表。 */

    kinfoln("[dynlink] done: %d object(s), main exports=%u",
            nobj, mo->nexports);

    /* 重定位做完就不再需要导出表 (后续没有 dlopen / 延迟绑定) —— 立刻还回
       内核堆。表容量按需增长过, 这里一并释放。 */
    dl_cleanup();
    return 0;
}

/* ==================================================================
 * 统一的"加载期动态链接"入口 —— 两个加载器 (sys_load 与
 * Schedule::NewThread) 都调它, 避免逻辑重复。
 * 调用点: elf_load*() 之后、分配 TLS 块之前。
 * 返回 0 = 成功 (含"本来就没有 DT_NEEDED"的情况); <0 表示必须放弃启动。
 * ================================================================== */
int elf_dyn_apply(uint8_t *main_img, uint64_t main_len, pagemap_t *pm,
                  uint64_t *tls_offset, uint64_t *tls_memsz,
                  uint64_t *tls_filesz, uint64_t *tls_align,
                  uint64_t main_bias,
                  uint8_t **out_so_tls_img, uint64_t *out_so_tls_len,
                  uint64_t *out_tls_main_data_off) {
    if (out_so_tls_img)      *out_so_tls_img = nullptr;
    if (out_so_tls_len)      *out_so_tls_len = 0;
    if (out_tls_main_data_off) *out_tls_main_data_off = 0;

    uint8_t *so_img = nullptr;
    uint64_t so_len = 0;
    char so_name[96];
    uint64_t tls_main_off = 0;

    int dr = dyn_read_needed(main_img, main_len, &so_img, &so_len,
                             so_name, sizeof(so_name));
    kinfoln("[dynlink] apply: dr=%d name='%s' so_len=%llu main_bias=0x%llx",
            dr, so_name, (unsigned long long)so_len,
            (unsigned long long)main_bias);
    if (dr < 0) { if (so_img) kfree(so_img); return dr; }
    if (dr == 0) return 0;                      /* 静态程序: 什么都不做 */

    /* .so TLS 初始镜像暂存区 (STATIC_TLS: 数据必须先落在合并块首部) */
    uint64_t cap = 4096;
    uint8_t *timg = (uint8_t*)kmalloc(cap);
    if (!timg) { kfree(so_img); return -ENOMEM; }
    _memset(timg, 0, cap);

    int r2 = elf_dyn_prepare(main_img, main_len, pm,
                             tls_offset, tls_memsz, tls_filesz, tls_align,
                             so_img, so_len, so_name,
                             &tls_main_off, timg, cap, main_bias);
    kfree(so_img); so_img = nullptr;
    if (r2 != 0) {
        kerrorln("[dynlink] prepare failed: %d", r2);
        kfree(timg);
        return r2;
    }
    if (out_so_tls_img) *out_so_tls_img = timg;
    else                kfree(timg);
    if (out_so_tls_len) *out_so_tls_len = cap;
    if (out_tls_main_data_off) *out_tls_main_data_off = tls_main_off;
    return 1;                                   /* >0 = 真的做了动态链接 */
}
