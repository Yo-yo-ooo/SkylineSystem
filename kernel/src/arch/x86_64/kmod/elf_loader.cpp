//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * elf_loader.cpp —??.ko (ET_REL) 内核模块加载器（Tier 0 固定机制?? * ----------------------------------------------------------------------------
 * 职责边界（与 docs/kernel-modules.md 一致）?? *   1. 解析 ELF64 可重定位目标（ET_REL / x86_64），??*严格**边界校验?? *   2. ??SHF_ALLOC 段分配内核内存镜像并??sh_addralign 布局?? *   3. 应用受限重定位集（见下表），遇到未知类型整体拒绝 —??绝不把未重定?? *      的槽放进内核?? *   4. 解析 .kmod_manifest / .out / .in 三个契约段，做结??字符??布局校验?? *   5. 不解释模块间业务语义：provider/symbol/contract 只是字符??+ 机械校验?? *
 * 支持的重定位（x86_64）：
 *   R_X86_64_NONE(0)            跳过
 *   R_X86_64_64(1)              S + A
 *   R_X86_64_PC32(2)            S + A - P
 *   R_X86_64_PLT32(4)           S + A - P（模块内调用?? *   R_X86_64_32(10)             S + A，结果须??32 位无符号
 *   R_X86_64_32S(11)            S + A，结果须可符号扩展回 64 ?? * 其余（GOT/RELATIVE/TLS/COPY 等）一??KMOD_ERR_RELOC?? *
 * 镜像执行属性：内核 HHDM 直接映射??RW 且未??NX??ko 代码可直接执行；
 * 这是低隔离内核态的既有事实。加载器能保证的??不被未重定位/未解析的
 * 指针执行??，不能保??W^X —??该限制在文档中如实声明（治理层靠能力?? * 回滚补足可控性）?? * ==========================================================================*/
#include <kmod/kmod_internal.h>

/* 本文件刻意不引用内核 arch 头：宿主测试（tests/kmod）直接编译同一份代码??*/
#include <stdint.h>
#include <stddef.h>

namespace {

/* ---------------- ELF 常量（只保留本文件用到的??---------------- */
constexpr uint8_t  ELFCLASS64      = 2;
constexpr uint8_t  ELFDATA2LSB     = 1;
constexpr uint16_t ET_REL          = 1;
constexpr uint16_t EM_X86_64       = 62;

constexpr uint32_t SHT_PROGBITS    = 1;
constexpr uint32_t SHT_SYMTAB      = 2;
constexpr uint32_t SHT_STRTAB      = 3;
constexpr uint32_t SHT_RELA        = 4;
constexpr uint32_t SHT_NOBITS      = 8;
constexpr uint32_t SHT_REL         = 9;
constexpr uint32_t SHT_INIT_ARRAY  = 14;
constexpr uint32_t SHT_FINI_ARRAY  = 15;
constexpr uint32_t SHT_NOTE        = 7;

constexpr uint64_t SHF_ALLOC       = 0x2;

constexpr uint16_t SHN_UNDEF       = 0;
constexpr uint16_t SHN_ABS         = 0xfff1;
constexpr uint16_t SHN_COMMON      = 0xfff2;

constexpr uint8_t  STB_WEAK        = 2;

constexpr uint32_t R_X86_64_NONE   = 0;
constexpr uint32_t R_X86_64_64     = 1;
constexpr uint32_t R_X86_64_PC32   = 2;
constexpr uint32_t R_X86_64_PLT32  = 4;
constexpr uint32_t R_X86_64_32     = 10;
constexpr uint32_t R_X86_64_32S    = 11;

constexpr uint64_t LOADER_MAX_ALIGN = 16;     /* 段对齐上限（ABI 约束??*/

/* 头部/表项布局（与 x86_64 SysV ELF64 一致；packed 避免对齐假设??*/
struct Ehdr {
    uint8_t  ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} __attribute__((packed));

struct Shdr {
    uint32_t name;
    uint32_t type;
    uint64_t flags;
    uint64_t addr;
    uint64_t offset;
    uint64_t size;
    uint32_t link;
    uint32_t info;
    uint64_t addralign;
    uint64_t entsize;
} __attribute__((packed));

struct Rela {
    uint64_t offset;
    uint64_t info;
    int64_t  addend;
} __attribute__((packed));

struct Sym {
    uint32_t name;
    uint8_t  info;
    uint8_t  other;
    uint16_t shndx;
    uint64_t value;
    uint64_t size;
} __attribute__((packed));

static_assert(sizeof(Ehdr) == 64, "ELF64 Ehdr layout");
static_assert(sizeof(Shdr) == 64, "ELF64 Shdr layout");
static_assert(sizeof(Rela) == 24, "ELF64 Rela layout");
static_assert(sizeof(Sym)  == 24, "ELF64 Sym layout");

/* ABI 尺寸冻结：任何布局变更必须同时提升 KMOD_ABI_VERSION/指纹??*/
static_assert(sizeof(kmod_manifest_t) == 256, "kmod_manifest_t ABI freeze");
static_assert(sizeof(kmod_export_t)   == 48,  "kmod_export_t ABI freeze");
static_assert(sizeof(kmod_import_t)   == 48,  "kmod_import_t ABI freeze");

/* ---------------- 安全读取辅助 ---------------- */
inline bool u64_add(uint64_t a, uint64_t b, uint64_t *out)
{
    if (b > UINT64_MAX - a)
        return false;
    *out = a + b;
    return true;
}

inline bool u64_mul(uint64_t a, uint64_t b, uint64_t *out)
{
    if (a != 0 && b > UINT64_MAX / a)
        return false;
    *out = a * b;
    return true;
}

inline bool range_ok(uint64_t off, uint64_t len, uint64_t limit)
{
    uint64_t end;
    if (!u64_add(off, len, &end))
        return false;
    return end <= limit;
}

inline uint64_t align_up_u64(uint64_t v, uint64_t align)
{
    if (align <= 1)
        return v;
    uint64_t rem = v % align;
    return rem == 0 ? v : (v + (align - rem));
}

inline bool is_pow2(uint64_t v)
{
    return v != 0 && (v & (v - 1)) == 0;
}

/* ??[off, off+len) 从源文件拷到本地（已做边界检查） */
inline bool read_bytes(const uint8_t *src, uint64_t src_size, uint64_t off,
                       void *dst, uint64_t len)
{
    if (!range_ok(off, len, src_size))
        return false;
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = src + off;
    for (uint64_t i = 0; i < len; i++)
        d[i] = s[i];
    return true;
}

/* 错误信息：只保留短字符串 + 可选的十六进制值，??libc 依赖 */
struct Err {
    char *buf;
    uint64_t cap;
};

void err_clear(Err *e)
{
    if (e && e->buf && e->cap)
        e->buf[0] = '\0';
}

void err_set(Err *e, const char *msg, uint64_t value, bool with_value)
{
    if (!e || !e->buf || e->cap < 2)
        return;
    uint64_t pos = 0;
    for (const char *p = msg; *p && pos + 1 < e->cap; p++)
        e->buf[pos++] = *p;
    if (with_value && pos + 3 < e->cap) {
        e->buf[pos++] = ':';
        e->buf[pos++] = '0';
        e->buf[pos++] = 'x';
        char tmp[16];
        uint32_t n = 0;
        if (value == 0) {
            tmp[n++] = '0';
        } else {
            while (value && n < sizeof(tmp)) {
                uint32_t d = (uint32_t)(value & 0xf);
                tmp[n++] = (char)(d < 10 ? ('0' + d) : ('a' + d - 10));
                value >>= 4;
            }
        }
        while (n > 0 && pos + 1 < e->cap)
            e->buf[pos++] = tmp[--n];
    }
    e->buf[pos] = '\0';
}

void err_rc(Err *e, const char *msg, int32_t rc)
{
    if (!e || !e->buf || e->cap < 2)
        return;
    uint64_t pos = 0;
    for (const char *p = msg; *p && pos + 1 < e->cap; p++)
        e->buf[pos++] = *p;
    if (pos + 4 < e->cap) {
        e->buf[pos++] = ' ';
        e->buf[pos++] = 'r';
        e->buf[pos++] = 'c';
        e->buf[pos++] = '=';
    }
    /* rc 为小负数，转??1..99 打印，足够定??*/
    uint64_t v = (uint64_t)(rc < 0 ? -rc : rc);
    char tmp[8];
    uint32_t n = 0;
    if (v == 0)
        tmp[n++] = '0';
    while (v && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0 && pos + 1 < e->cap)
        e->buf[pos++] = tmp[--n];
    e->buf[pos] = '\0';
}

/* ---------------- 解析上下??---------------- */
struct ParseCtx {
    const uint8_t *file;
    uint64_t fsize;
    Ehdr eh;
    uint32_t shnum;
    uint32_t order[KMOD_MAX_SECTIONS];       /* SHF_ALLOC 段下标（文件顺序??*/
    uint32_t n_order;
    uint64_t sec_off[KMOD_MAX_SECTIONS];     /* 段在镜像内的偏移 */
    bool     sec_loaded[KMOD_MAX_SECTIONS];
    const char *shstr;
    uint64_t shstr_size;
    uint64_t image_size;
    uint64_t max_align;
    const kmod_kernel_export_t *kexp;
    uint32_t nkexp;
};

Shdr shdr_at(const ParseCtx *c, uint32_t idx)
{
    Shdr s;
    uint64_t off = c->eh.shoff + (uint64_t)idx * sizeof(Shdr);
    Shdr zero;
    for (uint64_t i = 0; i < sizeof(Shdr); i++)
        ((uint8_t *)&zero)[i] = 0;
    if (!read_bytes(c->file, c->fsize, off, &s, sizeof(s)))
        return zero;
    return s;
}

const char *sec_name(const ParseCtx *c, const Shdr &s)
{
    if (!c->shstr || s.name >= c->shstr_size)
        return nullptr;
    const char *p = c->shstr + s.name;
    uint64_t remain = c->shstr_size - s.name;
    for (uint64_t i = 0; i < remain; i++) {
        if (p[i] == '\0')
            return p;
    }
    return nullptr;
}

bool sec_is_alloc(const Shdr &s)
{
    return (s.flags & SHF_ALLOC) != 0;
}

/* 校验 file 范围内的 ELF ??+ 段表 + 段名字符串表。返??KMOD_OK / 负码 */
int32_t parse_header(ParseCtx *c, const uint8_t *data, uint64_t size, Err *err)
{
    if (!data || size < sizeof(Ehdr)) {
        err_set(err, "elf too small", size, true);
        return KMOD_ERR_FORMAT;
    }
    for (uint64_t i = 0; i < sizeof(ParseCtx); i++)
        ((uint8_t *)c)[i] = 0;
    c->file = data;
    c->fsize = size;

    if (!read_bytes(data, size, 0, &c->eh, sizeof(c->eh))) {
        err_set(err, "elf header truncated", 0, false);
        return KMOD_ERR_FORMAT;
    }
    const uint8_t *id = c->eh.ident;
    if (id[0] != 0x7f || id[1] != 'E' || id[2] != 'L' || id[3] != 'F') {
        err_set(err, "bad elf magic", 0, false);
        return KMOD_ERR_FORMAT;
    }
    if (id[4] != ELFCLASS64 || id[5] != ELFDATA2LSB || id[6] != 1) {
        err_set(err, "bad elf class/endian/version", 0, false);
        return KMOD_ERR_FORMAT;
    }
    if (c->eh.type != ET_REL) {
        err_set(err, "elf not ET_REL", c->eh.type, true);
        return KMOD_ERR_FORMAT;
    }
    if (c->eh.machine != EM_X86_64) {
        err_set(err, "elf not x86_64", c->eh.machine, true);
        return KMOD_ERR_FORMAT;
    }
    if (c->eh.version != 1u || c->eh.ehsize != sizeof(Ehdr) ||
        c->eh.shentsize != sizeof(Shdr) || c->eh.shnum == 0) {
        err_set(err, "bad elf header sizes", 0, false);
        return KMOD_ERR_FORMAT;
    }
    if (c->eh.shnum > KMOD_MAX_SECTIONS) {
        err_set(err, "too many sections", c->eh.shnum, true);
        return KMOD_ERR_FORMAT;
    }
    uint64_t shbytes;
    if (!u64_mul((uint64_t)c->eh.shnum, sizeof(Shdr), &shbytes) ||
        !range_ok(c->eh.shoff, shbytes, size)) {
        err_set(err, "section table out of range", c->eh.shoff, true);
        return KMOD_ERR_FORMAT;
    }
    c->shnum = c->eh.shnum;

    /* 段名字符串表 */
    if (c->eh.shstrndx == 0 || c->eh.shstrndx >= c->shnum) {
        err_set(err, "bad shstrndx", c->eh.shstrndx, true);
        return KMOD_ERR_FORMAT;
    }
    if (c->eh.shstrndx >= KMOD_MAX_SECTIONS) {
        err_set(err, "shstrndx too large", c->eh.shstrndx, true);
        return KMOD_ERR_FORMAT;
    }
    Shdr strh = shdr_at(c, c->eh.shstrndx);
    if (strh.type != SHT_STRTAB || strh.size == 0 || strh.size > size ||
        !range_ok(strh.offset, strh.size, size)) {
        err_set(err, "bad shstrtab", strh.offset, true);
        return KMOD_ERR_FORMAT;
    }
    c->shstr = (const char *)(data + strh.offset);
    c->shstr_size = strh.size;

    /* 段级校验 + 分配段布局预计??*/
    c->max_align = 1;
    uint64_t cursor = 0;
    for (uint32_t i = 1; i < c->shnum; i++) {
        Shdr s = shdr_at(c, i);
        if (s.addralign > LOADER_MAX_ALIGN || (s.addralign != 0 && !is_pow2(s.addralign))) {
            err_set(err, "bad section align", s.addralign, true);
            return KMOD_ERR_FORMAT;
        }
        if (s.type == SHT_NOTE)
            continue;   /* 工具链元数据??note.gnu.property/CET、build-id）不进入镜像 */
        if (!sec_is_alloc(s))
            continue;
        if (c->n_order >= KMOD_MAX_SECTIONS) {
            err_set(err, "too many alloc sections", c->n_order, true);
            return KMOD_ERR_FORMAT;
        }
        if (s.type != SHT_PROGBITS && s.type != SHT_NOBITS &&
            s.type != SHT_INIT_ARRAY && s.type != SHT_FINI_ARRAY) {
            err_set(err, "unsupported alloc section type", s.type, true);
            return KMOD_ERR_FORMAT;
        }
        if (s.type != SHT_NOBITS && !range_ok(s.offset, s.size, size)) {
            err_set(err, "section data out of range", s.offset, true);
            return KMOD_ERR_FORMAT;
        }
        if (s.size > KMOD_MAX_ELF_SIZE) {
            err_set(err, "section too large", s.size, true);
            return KMOD_ERR_FORMAT;
        }
        if (s.addralign > c->max_align)
            c->max_align = s.addralign;
        uint64_t aligned = align_up_u64(cursor, s.addralign < 1 ? 1 : s.addralign);
        if (aligned < cursor) {
            err_set(err, "alignment overflow", 0, false);
            return KMOD_ERR_FORMAT;
        }
        cursor = aligned;
        c->sec_off[i] = cursor;
        c->order[c->n_order++] = i;
        if (!u64_add(cursor, s.size, &cursor)) {
            err_set(err, "image size overflow", 0, false);
            return KMOD_ERR_FORMAT;
        }
        if (cursor > KMOD_MAX_ELF_SIZE) {
            err_set(err, "image too large", cursor, true);
            return KMOD_ERR_FORMAT;
        }
    }
    if (c->n_order == 0 || cursor == 0) {
        err_set(err, "no alloc sections", 0, false);
        return KMOD_ERR_FORMAT;
    }
    c->image_size = cursor;
    return KMOD_OK;
}

/* 在已解析段表中按名找段；段名必须唯一命中。返回段下标??-1 */
int32_t find_section(const ParseCtx *c, const char *name, uint32_t *out)
{
    int32_t found = -1;
    for (uint32_t i = 1; i < c->shnum; i++) {
        Shdr s = shdr_at(c, i);
        const char *n = sec_name(c, s);
        if (!n)
            continue;
        const char *a = n;
        const char *b = name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0') {
            if (found >= 0)
                return -2;  /* 重名??*/
            found = (int32_t)i;
        }
    }
    if (found < 0)
        return -1;
    *out = (uint32_t)found;
    return 0;
}

/* 检查模块镜像内绝对指针 + NUL 结尾字符??*/
const char *checked_str(const uint8_t *mem, uint64_t size, const void *p,
                        uint64_t max_len, uint64_t *out_len)
{
    uint64_t addr = (uint64_t)(uintptr_t)p;
    uint64_t base = (uint64_t)(uintptr_t)mem;
    if (addr < base || addr >= base + size)
        return nullptr;
    uint64_t off = addr - base;
    uint64_t remain = size - off;
    uint64_t limit = remain < (max_len + 1) ? remain : (max_len + 1);
    const char *s = (const char *)(mem + off);
    for (uint64_t i = 0; i < limit; i++) {
        if (s[i] == '\0') {
            if (out_len)
                *out_len = i;
            return s;
        }
    }
    return nullptr;
}

/* ---------------- 清单校验 ---------------- */
bool inline_str_ok(const char *field, uint64_t cap)
{
    if (!field || cap == 0)
        return false;
    return field[cap - 1] == '\0';
}

int32_t validate_manifest(const kmod_manifest_t *m, bool allow_reserved_name, Err *err)
{
    if (m->magic != KMOD_MANIFEST_MAGIC) {
        err_set(err, "manifest magic", m->magic, true);
        return KMOD_ERR_FORMAT;
    }
    if (m->abi_version != KMOD_ABI_VERSION) {
        err_set(err, "manifest abi", m->abi_version, true);
        return KMOD_ERR_VERSION;
    }
    if (m->tier != KMOD_TIER_POLICY && m->tier != KMOD_TIER_IMPL) {
        err_set(err, "manifest tier (tier0 reserved)", m->tier, true);
        return KMOD_ERR_TIER;
    }
    /* 未知 flags 位拒绝（前向兼容??ABI 版本升级??*/
    const uint32_t known_flags = KMOD_MF_BOOT | KMOD_MF_HOTREPLACE |
                                 KMOD_MF_UNLOADABLE | KMOD_MF_DEBUG |
                                 KMOD_MF_NO_AUTODEP | KMOD_MF_EXPERIMENTAL;
    if (m->flags & ~known_flags) {
        err_set(err, "manifest flags", m->flags, true);
        return KMOD_ERR_FORMAT;
    }
    if (!inline_str_ok(m->name, KMOD_NAME_MAX) ||
        !inline_str_ok(m->version, KMOD_VERSION_MAX) ||
        !inline_str_ok(m->description, KMOD_DESC_MAX)) {
        err_set(err, "manifest string not terminated", 0, false);
        return KMOD_ERR_FORMAT;
    }
    int32_t rc = allow_reserved_name ? KmodBuiltinNameValidate(m->name) : KmodNameValidate(m->name);
    if (rc != KMOD_OK) {
        err_set(err, "manifest module name", 0, false);
        return rc;
    }
    rc = KmodVersionParse(m->version, nullptr, nullptr, nullptr);
    if (rc != KMOD_OK) {
        err_set(err, "manifest version", 0, false);
        return rc;
    }
    if (m->init_budget_ms > KMOD_INIT_BUDGET_MAX_MS ||
        m->quiesce_budget_ms > KMOD_QUIESCE_BUDGET_MAX_MS) {
        err_set(err, "manifest budget too large", 0, false);
        return KMOD_ERR_INVAL;
    }
    if (m->n_requires > 16 || m->n_provides > 16) {
        err_set(err, "manifest contract list too long", 0, false);
        return KMOD_ERR_FORMAT;
    }
    return KMOD_OK;
}

/* ---------------- 导出/导入段校??---------------- */
/* 判断字符串是否以 prefix 开头（要求绝对相等前缀；两串均??NUL 终止??*/
bool str_starts_with(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s != *prefix)
            return false;
        s++;
        prefix++;
    }
    return true;
}

int32_t contract_allowed(const char *contract, uint32_t kind, Err *err)
{
    if (!contract || contract[0] == '\0') {
        /* 空的私有契约：只允许 FUNC/DATA；SEAT/SYSCALL 必须有机制契约名 */
        if (kind == KMOD_SYM_FUNC || kind == KMOD_SYM_DATA)
            return KMOD_OK;
        err_set(err, "export requires contract", kind, true);
        return KMOD_ERR_CONTRACT;
    }
    if (kind == KMOD_SYM_SEAT) {
        /* 座位名固定为 kernel.seat.<name>.vN，由 Tier0 座位注册表校验存在??*/
        if (!str_starts_with(contract, "kernel.seat.")) {
            err_set(err, "seat name must be kernel.seat.*", 0, false);
            return KMOD_ERR_CONTRACT;
        }
        return KMOD_OK;
    }
    if (kind == KMOD_SYM_SYSCALL || kind == KMOD_SYM_NAMESPACE) {
        /* 固定机制契约：只允许 kernel.syscall.v1 */
        if (!str_starts_with(contract, KMOD_CONTRACT_KERNEL_SYSCALL) ||
            contract[sizeof(KMOD_CONTRACT_KERNEL_SYSCALL) - 1] != '\0') {
            err_set(err, "syscall contract must be kernel.syscall.v1", 0, false);
            return KMOD_ERR_CONTRACT;
        }
        return KMOD_OK;
    }
    /* 私有业务契约：不得占??kernel./core. 保留前缀 */
    if (str_starts_with(contract, "kernel.") || str_starts_with(contract, "core.")) {
        err_set(err, "module contract uses reserved kernel./core. prefix", 0, false);
        return KMOD_ERR_CONTRACT;
    }
    int32_t rc = KmodContractValidate(contract);
    if (rc != KMOD_OK)
        err_set(err, "export contract namespace invalid", 0, false);
    return rc;
}

int32_t validate_exports(const kmod_elf_image_t *img, Err *err)
{
    kmod_export_t *ex = img->exports;
    for (uint32_t i = 0; i < img->n_exports; i++) {
        kmod_export_t *e = &ex[i];
        uint64_t len = 0;
        const char *sym = checked_str(img->mem, img->size, e->symbol,
                                      KMOD_SYMBOL_MAX, &len);
        if (!sym || len == 0) {
            err_set(err, "export symbol string bad", i, true);
            return KMOD_ERR_FORMAT;
        }
        uint64_t clen = 0;
        const char *contract = checked_str(img->mem, img->size, e->contract,
                                           KMOD_CONTRACT_MAX, &clen);
        (void)clen;
        if (!contract) {
            err_set(err, "export contract string bad", i, true);
            return KMOD_ERR_FORMAT;
        }
        if (e->kind < KMOD_SYM_FUNC || e->kind > KMOD_SYM_NAMESPACE) {
            err_set(err, "export kind", e->kind, true);
            return KMOD_ERR_FORMAT;
        }
        if (e->flags & ~(KMOD_EXF_HOTREPLACEABLE | KMOD_EXF_OPTIONAL)) {
            err_set(err, "export flags", e->flags, true);
            return KMOD_ERR_FORMAT;
        }
        int32_t rc = contract_allowed(contract, e->kind, err);
        if (rc != KMOD_OK)
            return rc;
        if (e->kind == KMOD_SYM_SEAT) {
            if (e->size == 0 || e->size > 4096) {
                err_set(err, "seat ops size", e->size, true);
                return KMOD_ERR_CONTRACT;
            }
            /* 座位导出??symbol 必须??contract 同名（便于按名查座位??*/
            {
                const char *a = sym;
                const char *b = contract;
                while (*a && *a == *b) { a++; b++; }
                if (*a != '\0' || *b != '\0') {
                    err_set(err, "seat symbol must equal contract", 0, false);
                    return KMOD_ERR_CONTRACT;
                }
            }
        }
        if (e->kind == KMOD_SYM_CONTRACT && (e->size == 0 || e->size > 4096)) {
            err_set(err, "contract ops size", e->size, true);
            return KMOD_ERR_CONTRACT;
        }
        if (e->kind == KMOD_SYM_SYSCALL) {
            if (e->aux >= KMOD_SYSCALL_MAX) {
                err_set(err, "syscall slot", e->aux, true);
                return KMOD_ERR_INVAL;
            }
        }
        if (e->kind != KMOD_SYM_NAMESPACE) {
            uint64_t base = (uint64_t)(uintptr_t)img->mem;
            uint64_t addr = (uint64_t)(uintptr_t)e->addr;
            if (addr < base || addr >= base + img->size) {
                err_set(err, "export addr outside image", i, true);
                return KMOD_ERR_FORMAT;
            }
        }
        /* 同模块导出符号唯一 */
        for (uint32_t j = 0; j < i; j++) {
            const char *other = checked_str(img->mem, img->size, ex[j].symbol,
                                            KMOD_SYMBOL_MAX, nullptr);
            if (!other)
                continue;
            const char *a = other;
            const char *b = sym;
            while (*a && *a == *b) { a++; b++; }
            if (*a == '\0' && *b == '\0') {
                err_set(err, "duplicate export symbol", i, true);
                return KMOD_ERR_FORMAT;
            }
        }
    }
    return KMOD_OK;
}

int32_t validate_imports(const kmod_elf_image_t *img, Err *err)
{
    kmod_import_t *im = img->imports;
    for (uint32_t i = 0; i < img->n_imports; i++) {
        kmod_import_t *e = &im[i];
        uint64_t len = 0;
        const char *provider = checked_str(img->mem, img->size, e->provider,
                                           KMOD_NAME_MAX, &len);
        if (!provider || len == 0) {
            err_set(err, "import provider string bad", i, true);
            return KMOD_ERR_FORMAT;
        }
        const char *symbol = checked_str(img->mem, img->size, e->symbol,
                                         KMOD_SYMBOL_MAX, nullptr);
        const char *contract = checked_str(img->mem, img->size, e->contract,
                                           KMOD_CONTRACT_MAX, nullptr);
        if (!symbol || !contract) {
            err_set(err, "import symbol/contract string bad", i, true);
            return KMOD_ERR_FORMAT;
        }
        bool dep_only = (e->flags & KMOD_IMP_DEP_EDGE) != 0;
        if (!dep_only) {
            if (symbol[0] == '\0') {
                err_set(err, "import symbol empty", i, true);
                return KMOD_ERR_FORMAT;
            }
            if (!e->slot) {
                err_set(err, "import slot null", i, true);
                return KMOD_ERR_FORMAT;
            }
            uint64_t base = (uint64_t)(uintptr_t)img->mem;
            uint64_t addr = (uint64_t)(uintptr_t)e->slot;
            if (addr < base || addr >= base + img->size || (addr & 0x7u)) {
                err_set(err, "import slot outside image/unaligned", i, true);
                return KMOD_ERR_FORMAT;
            }
        }
        if (e->flags & ~(KMOD_IMP_OPTIONAL | KMOD_IMP_WEAK | KMOD_IMP_DEP_EDGE)) {
            err_set(err, "import flags", e->flags, true);
            return KMOD_ERR_FORMAT;
        }
        if (e->abi_max != 0 && e->abi_min != 0 && e->abi_min > e->abi_max) {
            err_set(err, "import abi range inverted", i, true);
            return KMOD_ERR_VERSION;
        }
        /* provider ??"kernel" 时契约必须落??kernel.* 命名空间（机制契约） */
        bool is_kernel = provider[0] == 'k' && provider[1] == 'e' &&
                         provider[2] == 'r' && provider[3] == 'n' &&
                         provider[4] == 'e' && provider[5] == 'l' &&
                         provider[6] == '\0';
        if (is_kernel) {
            if (contract[0] == '\0') {
                err_set(err, "kernel import requires contract", i, true);
                return KMOD_ERR_CONTRACT;
            }
        } else {
            /* provider 可以是 Tier0 内建组件（kernel.* 保留前缀，允许被 .ko 导入，
             * 但 .ko 自身不得使用该前缀命名）；其余走 .ko 命名空间规则。 */
            bool provider_reserved = (provider[0] == 'k' && provider[1] == 'e' &&
                                      provider[2] == 'r' && provider[3] == 'n' &&
                                      provider[4] == 'e' && provider[5] == 'l' &&
                                      provider[6] == '.') ||
                                     (provider[0] == 'c' && provider[1] == 'o' &&
                                      provider[2] == 'r' && provider[3] == 'e' &&
                                      provider[4] == '.');
            int32_t rc = provider_reserved ? KmodBuiltinNameValidate(provider)
                                           : KmodNameValidate(provider);
            if (rc != KMOD_OK) {
                err_set(err, "import provider name", i, true);
                return rc;
            }
            /* 私有契约：允许 kernel./core. 机制契约（内建组件导出）；业务契约走
             * 常规命名空间规则（保留前缀禁止被模块业务契约占用）。 */
            if (contract[0] != '\0' &&
                (str_starts_with(contract, "kernel.") ||
                 str_starts_with(contract, "core."))) {
                rc = KmodContractValidate(contract);
                if (rc != KMOD_OK) {
                    err_set(err, "import contract namespace invalid", i, true);
                    return rc;
                }
            } else {
                rc = contract_allowed(contract, KMOD_SYM_FUNC, err);
                if (rc != KMOD_OK)
                    return rc;
            }
        }
    }
    return KMOD_OK;
}

/* 解析 init/fini 数组??KmodElfLoadImage 主流程内完成（见下方 SHT_INIT_ARRAY 段）??*/

} /* namespace */

/* ==================================================================== */
/* 对外接口                                                              */
/* ==================================================================== */

extern "C" int32_t KmodElfValidateManifest(const kmod_manifest_t *m, char *errbuf,
                                           uint64_t errcap)
{
    Err e{errbuf, errcap};
    err_clear(&e);
    return validate_manifest(m, false, &e);
}

/* 内建组件清单校验：允许 kernel./core. 保留前缀（Tier0 构建产物），其余规则与
 * .ko 完全一致。仅由 builtin.cpp 的注册路径调用。 */
extern "C" int32_t KmodElfValidateBuiltinManifest(const kmod_manifest_t *m,
                                                  char *errbuf, uint64_t errcap)
{
    Err e{errbuf, errcap};
    err_clear(&e);
    return validate_manifest(m, true, &e);
}

extern "C" int32_t KmodElfInspect(const uint8_t *data, uint64_t size,
                                  kmod_manifest_t *out_manifest,
                                  char *errbuf, uint64_t errcap)
{
    Err e{errbuf, errcap};
    err_clear(&e);
    if (!out_manifest) {
        err_set(&e, "inspect: null out", 0, false);
        return KMOD_ERR_INVAL;
    }
    ParseCtx *ctx = (ParseCtx *)kmod_alloc(sizeof(ParseCtx));
    if (!ctx) {
        err_set(&e, "inspect: no memory", 0, false);
        return KMOD_ERR_NOMEM;
    }
    int32_t rc = parse_header(ctx, data, size, &e);
    if (rc == KMOD_OK) {
        uint32_t midx = 0;
        int32_t f = find_section(ctx, KMOD_SEC_MANIFEST, &midx);
        if (f == -2) {
            err_set(&e, "inspect: duplicate manifest section", 0, false);
            rc = KMOD_ERR_FORMAT;
        } else if (f < 0) {
            err_set(&e, "inspect: no manifest section", 0, false);
            rc = KMOD_ERR_FORMAT;
        } else {
            Shdr s = shdr_at(ctx, midx);
            if (s.type != SHT_PROGBITS || !sec_is_alloc(s) ||
                s.size != sizeof(kmod_manifest_t)) {
                err_set(&e, "inspect: bad manifest section", s.size, true);
                rc = KMOD_ERR_FORMAT;
            } else if (!read_bytes(ctx->file, ctx->fsize, s.offset,
                                   out_manifest, sizeof(*out_manifest))) {
                err_set(&e, "inspect: manifest read", 0, false);
                rc = KMOD_ERR_FORMAT;
            } else {
                rc = validate_manifest(out_manifest, false, &e);
            }
        }
    }
    kmod_free(ctx, sizeof(ParseCtx));
    return rc;
}

extern "C" void KmodElfFreeImage(kmod_elf_image_t *img)
{
    if (!img)
        return;
    if (img->mem) {
        kmod_free(img->mem, img->size);
        img->mem = nullptr;
    }
    img->size = 0;
    img->manifest = nullptr;
    img->exports = nullptr;
    img->n_exports = 0;
    img->imports = nullptr;
    img->n_imports = 0;
    img->init_array = nullptr;
    img->n_init_array = 0;
    img->fini_array = nullptr;
    img->n_fini_array = 0;
    img->reloc_count = 0;
    img->section_count = 0;
}

/* 应用一个重定位??*/
static int32_t apply_rela_section(const ParseCtx *ctx, uint8_t *mem,
                                  const Shdr &rela_sh, uint64_t *reloc_count, Err *err)
{
    if (rela_sh.type != SHT_RELA) {
        err_set(err, "reloc section not RELA", rela_sh.type, true);
        return KMOD_ERR_RELOC;
    }
    if (rela_sh.info == 0 || rela_sh.info >= ctx->shnum) {
        err_set(err, "reloc target index", rela_sh.info, true);
        return KMOD_ERR_RELOC;
    }
    Shdr target = shdr_at(ctx, rela_sh.info);
    if (!sec_is_alloc(target)) {
        /* 调试/非分配段的重定位直接忽略（不进入镜像??*/
        return KMOD_OK;
    }
    if (target.type == SHT_NOBITS) {
        /* 重定位写目标??NOBITS 意味着模块布局错误??bss 不能有重定位??*/
        err_set(err, "reloc targets NOBITS section", rela_sh.info, true);
        return KMOD_ERR_RELOC;
    }
    if (rela_sh.link == 0 || rela_sh.link >= ctx->shnum) {
        err_set(err, "reloc symtab index", rela_sh.link, true);
        return KMOD_ERR_RELOC;
    }
    Shdr symtab = shdr_at(ctx, rela_sh.link);
    if (symtab.type != SHT_SYMTAB || symtab.entsize != sizeof(Sym) ||
        symtab.size % sizeof(Sym) != 0) {
        err_set(err, "bad symtab", symtab.size, true);
        return KMOD_ERR_RELOC;
    }
    (void)symtab;
    if (rela_sh.size % sizeof(Rela) != 0 || rela_sh.entsize != sizeof(Rela)) {
        err_set(err, "bad rela entsize", rela_sh.entsize, true);
        return KMOD_ERR_RELOC;
    }
    if (rela_sh.link == 0 || symtab.link == 0 || symtab.link >= ctx->shnum) {
        err_set(err, "bad symtab strtab link", 0, false);
        return KMOD_ERR_RELOC;
    }
    Shdr strtab = shdr_at(ctx, symtab.link);
    if (strtab.type != SHT_STRTAB || !range_ok(strtab.offset, strtab.size, ctx->fsize)) {
        err_set(err, "bad symtab strtab", 0, false);
        return KMOD_ERR_RELOC;
    }
    const char *strbase = (const char *)(ctx->file + strtab.offset);

    uint64_t target_base = ctx->sec_off[rela_sh.info];
    uint64_t target_size = target.size;
    uint64_t count = rela_sh.size / sizeof(Rela);
    for (uint64_t k = 0; k < count; k++) {
        Rela r;
        if (!read_bytes(ctx->file, ctx->fsize,
                        rela_sh.offset + k * sizeof(Rela), &r, sizeof(r))) {
            err_set(err, "rela out of range", k, true);
            return KMOD_ERR_RELOC;
        }
        uint32_t type = (uint32_t)(r.info & 0xffffffffu);
        uint64_t symidx = r.info >> 32;
        if (type == R_X86_64_NONE)
            continue;
        /* 写位置必须在目标段内，且写宽度不越界 */
        uint64_t need = (type == R_X86_64_64) ? 8u : 4u;
        if (!range_ok(r.offset, need, target_size)) {
            err_set(err, "reloc offset outside section", r.offset, true);
            return KMOD_ERR_RELOC;
        }
        /* 符号解析 */
        uint64_t nsyms = symtab.size / sizeof(Sym);
        if (symidx >= nsyms) {
            err_set(err, "reloc symbol index", symidx, true);
            return KMOD_ERR_RELOC;
        }
        Sym sym;
        if (!read_bytes(ctx->file, ctx->fsize,
                        symtab.offset + symidx * sizeof(Sym), &sym, sizeof(sym))) {
            err_set(err, "reloc symbol read", symidx, true);
            return KMOD_ERR_RELOC;
        }
        uint64_t S = 0;
        bool resolved = false;
        if (sym.shndx == SHN_UNDEF) {
            if (sym.name >= strtab.size) {
                err_set(err, "undefined symbol name", sym.name, true);
                return KMOD_ERR_RELOC;
            }
            const char *sname = strbase + sym.name;
            uint64_t remain = strtab.size - sym.name;
            bool nul = false;
            for (uint64_t i = 0; i < remain; i++) {
                if (sname[i] == '\0') { nul = true; break; }
            }
            if (!nul || sname[0] == '\0') {
                err_set(err, "undefined symbol empty/bad name", 0, false);
                return KMOD_ERR_RELOC;
            }
            for (uint32_t x = 0; x < ctx->nkexp; x++) {
                const char *a = ctx->kexp[x].name;
                const char *b = sname;
                if (!a)
                    continue;
                while (*a && *a == *b) { a++; b++; }
                if (*a == '\0' && *b == '\0') {
                    S = (uint64_t)(uintptr_t)ctx->kexp[x].addr;
                    resolved = true;
                    break;
                }
            }
            if (!resolved) {
                uint8_t bind = (uint8_t)(sym.info >> 4);
                if (bind == STB_WEAK) {
                    S = 0;
                    resolved = true;
                }
            }
            if (!resolved) {
                /* 无法解析的符号直接拒绝（关键错误：E9 打印模块名由上层补充??*/
                err_set(err, "unresolved kernel symbol", symidx, true);
                return KMOD_ERR_UNRESOLVED;
            }
        } else if (sym.shndx == SHN_ABS) {
            S = sym.value;
            resolved = true;
        } else if (sym.shndx == SHN_COMMON) {
            err_set(err, "COMMON symbol unsupported", symidx, true);
            return KMOD_ERR_RELOC;
        } else {
            if (sym.shndx >= ctx->shnum) {
                err_set(err, "symbol section index", sym.shndx, true);
                return KMOD_ERR_RELOC;
            }
            Shdr ss = shdr_at(ctx, sym.shndx);
            if (!sec_is_alloc(ss)) {
                err_set(err, "symbol in non-alloc section", sym.shndx, true);
                return KMOD_ERR_RELOC;
            }
            if (sym.value > ss.size) {
                err_set(err, "symbol value outside section", sym.value, true);
                return KMOD_ERR_RELOC;
            }
            S = (uint64_t)(uintptr_t)(mem + ctx->sec_off[sym.shndx]) + sym.value;
            resolved = true;
        }
        if (!resolved) {
            err_set(err, "reloc unresolved", 0, false);
            return KMOD_ERR_UNRESOLVED;
        }
        uint64_t P = (uint64_t)(uintptr_t)(mem + target_base) + r.offset;
        uint8_t *loc = mem + target_base + r.offset;
        switch (type) {
        case R_X86_64_64: {
            uint64_t v = S + (uint64_t)r.addend;
            for (uint32_t b = 0; b < 8; b++)
                loc[b] = (uint8_t)(v >> (b * 8));
            break;
        }
        case R_X86_64_PC32:
        case R_X86_64_PLT32: {
            int64_t v = (int64_t)S + r.addend - (int64_t)P;
            if (v < INT32_MIN || v > INT32_MAX) {
                err_set(err, "PC32 reloc overflow", (uint64_t)v, true);
                return KMOD_ERR_RELOC;
            }
            int32_t sv = (int32_t)v;
            for (uint32_t b = 0; b < 4; b++)
                loc[b] = (uint8_t)((uint32_t)sv >> (b * 8));
            break;
        }
        case R_X86_64_32: {
            uint64_t v = S + (uint64_t)r.addend;
            if (v > 0xffffffffull) {
                err_set(err, "R_X86_64_32 overflow", v, true);
                return KMOD_ERR_RELOC;
            }
            uint32_t sv = (uint32_t)v;
            for (uint32_t b = 0; b < 4; b++)
                loc[b] = (uint8_t)(sv >> (b * 8));
            break;
        }
        case R_X86_64_32S: {
            int64_t v = (int64_t)S + r.addend;
            int32_t sv = (int32_t)v;
            if ((int64_t)sv != v) {
                err_set(err, "R_X86_64_32S overflow", (uint64_t)v, true);
                return KMOD_ERR_RELOC;
            }
            for (uint32_t b = 0; b < 4; b++)
                loc[b] = (uint8_t)((uint32_t)sv >> (b * 8));
            break;
        }
        default:
            err_set(err, "unsupported reloc type", type, true);
            return KMOD_ERR_RELOC;
        }
        if (reloc_count)
            (*reloc_count)++;
    }
    return KMOD_OK;
}

extern "C" int32_t KmodElfLoadImage(const uint8_t *data, uint64_t size,
                                    const kmod_kernel_export_t *kexp, uint32_t nkexp,
                                    kmod_elf_image_t *out,
                                    char *errbuf, uint64_t errcap)
{
    Err e{errbuf, errcap};
    err_clear(&e);
    if (!out) {
        return KMOD_ERR_INVAL;
    }
    for (uint64_t i = 0; i < sizeof(*out); i++)
        ((uint8_t *)out)[i] = 0;

    ParseCtx *ctx = (ParseCtx *)kmod_alloc(sizeof(ParseCtx));
    if (!ctx) {
        err_set(&e, "load: no memory", 0, false);
        return KMOD_ERR_NOMEM;
    }
    int32_t rc = parse_header(ctx, data, size, &e);
    if (rc != KMOD_OK) {
        kmod_free(ctx, sizeof(ParseCtx));
        return rc;
    }
    ctx->kexp = kexp;
    ctx->nkexp = nkexp;

    uint32_t manifest_idx = 0, out_idx = 0, in_idx = 0;
    int32_t f_man = find_section(ctx, KMOD_SEC_MANIFEST, &manifest_idx);
    int32_t f_out = find_section(ctx, KMOD_SEC_OUT, &out_idx);
    int32_t f_in = find_section(ctx, KMOD_SEC_IN, &in_idx);
    if (f_man == -2 || f_out == -2 || f_in == -2) {
        err_set(&e, "duplicate contract section", 0, false);
        kmod_free(ctx, sizeof(ParseCtx));
        return KMOD_ERR_FORMAT;
    }
    if (f_man < 0) {
        err_set(&e, "missing manifest section", 0, false);
        kmod_free(ctx, sizeof(ParseCtx));
        return KMOD_ERR_FORMAT;
    }

    /* 分配并清零镜??*/
    uint8_t *mem = (uint8_t *)kmod_alloc(ctx->image_size);
    if (!mem) {
        err_set(&e, "load: image alloc failed", ctx->image_size, true);
        kmod_free(ctx, sizeof(ParseCtx));
        return KMOD_ERR_NOMEM;
    }
    for (uint64_t i = 0; i < ctx->image_size; i++)
        mem[i] = 0;

    /* 拷贝 SHF_ALLOC 段数??*/
    for (uint32_t oi = 0; oi < ctx->n_order; oi++) {
        uint32_t si = ctx->order[oi];
        Shdr s = shdr_at(ctx, si);
        if (s.type == SHT_NOBITS)
            continue;
        if (!read_bytes(ctx->file, ctx->fsize, s.offset, mem + ctx->sec_off[si], s.size)) {
            err_set(&e, "section copy out of range", si, true);
            rc = KMOD_ERR_FORMAT;
            goto fail_image;
        }
        ctx->sec_loaded[si] = true;
    }

    /* 应用重定位（按文件顺序，所??RELA 段） */
    {
        uint64_t reloc_count = 0;
        for (uint32_t i = 1; i < ctx->shnum; i++) {
            Shdr s = shdr_at(ctx, i);
            if (s.type == SHT_RELA) {
                rc = apply_rela_section(ctx, mem, s, &reloc_count, &e);
                if (rc != KMOD_OK)
                    goto fail_image;
            } else if (s.type == SHT_REL) {
                /* x86_64 ABI 不应出现 SHT_REL；若目标??alloc 段则拒绝 */
                if (s.info != 0 && s.info < ctx->shnum) {
                    Shdr t = shdr_at(ctx, s.info);
                    if (sec_is_alloc(t)) {
                        err_set(&e, "SHT_REL unsupported", i, true);
                        rc = KMOD_ERR_RELOC;
                        goto fail_image;
                    }
                }
            }
        }
        out->reloc_count = (uint32_t)(reloc_count > 0xffffffffull ? 0xffffffffull : reloc_count);
    }

    /* 契约段解??*/
    {
        Shdr ms = shdr_at(ctx, manifest_idx);
        if (ms.type != SHT_PROGBITS || !sec_is_alloc(ms) ||
            ms.size != sizeof(kmod_manifest_t)) {
            err_set(&e, "bad manifest section", ms.size, true);
            rc = KMOD_ERR_FORMAT;
            goto fail_image;
        }
        out->manifest = (kmod_manifest_t *)(mem + ctx->sec_off[manifest_idx]);
        rc = validate_manifest(out->manifest, false, &e);
        if (rc != KMOD_OK)
            goto fail_image;

        if (f_out >= 0) {
            Shdr os = shdr_at(ctx, out_idx);
            if (os.type != SHT_PROGBITS || !sec_is_alloc(os) ||
                os.size % sizeof(kmod_export_t) != 0 ||
                os.size / sizeof(kmod_export_t) > KMOD_MAX_EXPORTS) {
                err_set(&e, "bad .out section", os.size, true);
                rc = KMOD_ERR_FORMAT;
                goto fail_image;
            }
            out->exports = (kmod_export_t *)(mem + ctx->sec_off[out_idx]);
            out->n_exports = (uint32_t)(os.size / sizeof(kmod_export_t));
        }
        if (f_in >= 0) {
            Shdr is = shdr_at(ctx, in_idx);
            if (is.type != SHT_PROGBITS || !sec_is_alloc(is) ||
                is.size % sizeof(kmod_import_t) != 0 ||
                is.size / sizeof(kmod_import_t) > KMOD_MAX_IMPORTS) {
                err_set(&e, "bad .in section", is.size, true);
                rc = KMOD_ERR_FORMAT;
                goto fail_image;
            }
            out->imports = (kmod_import_t *)(mem + ctx->sec_off[in_idx]);
            out->n_imports = (uint32_t)(is.size / sizeof(kmod_import_t));
        }
    }

    /* 镜像内字符串/结构语义校验 */
    {
        kmod_elf_image_t tmp;
        for (uint64_t i = 0; i < sizeof(tmp); i++)
            ((uint8_t *)&tmp)[i] = 0;
        tmp.mem = mem;
        tmp.size = ctx->image_size;
        tmp.exports = out->exports;
        tmp.n_exports = out->n_exports;
        tmp.imports = out->imports;
        tmp.n_imports = out->n_imports;
        rc = validate_exports(&tmp, &e);
        if (rc != KMOD_OK)
            goto fail_image;
        rc = validate_imports(&tmp, &e);
        if (rc != KMOD_OK)
            goto fail_image;
    }

    /* init/fini 数组（模块全局构??析构；加载器只收集，调用由事务层负责??*/
    {
        for (uint32_t i = 1; i < ctx->shnum; i++) {
            Shdr s = shdr_at(ctx, i);
            if (!sec_is_alloc(s))
                continue;
            if (s.type != SHT_INIT_ARRAY && s.type != SHT_FINI_ARRAY)
                continue;
            if (s.size % 8 != 0) {
                err_set(&e, "bad init/fini array size", s.size, true);
                rc = KMOD_ERR_FORMAT;
                goto fail_image;
            }
            uint32_t n = (uint32_t)(s.size / 8);
            void (**arr)(void) = (void (**)(void))(mem + ctx->sec_off[i]);
            for (uint32_t k = 0; k < n; k++) {
                uint64_t fp = (uint64_t)(uintptr_t)arr[k];
                if (fp == 0) {
                    err_set(&e, "null init/fini hook", i, true);
                    rc = KMOD_ERR_FORMAT;
                    goto fail_image;
                }
                uint64_t base = (uint64_t)(uintptr_t)mem;
                if (fp < base || fp >= base + ctx->image_size) {
                    err_set(&e, "init/fini hook outside image", i, true);
                    rc = KMOD_ERR_FORMAT;
                    goto fail_image;
                }
            }
            if (s.type == SHT_INIT_ARRAY) {
                out->init_array = arr;
                out->n_init_array = n;
            } else {
                out->fini_array = arr;
                out->n_fini_array = n;
            }
        }
    }

    out->mem = mem;
    out->size = ctx->image_size;
    out->section_count = ctx->n_order;
    kmod_free(ctx, sizeof(ParseCtx));
    return KMOD_OK;

fail_image:
    for (uint64_t i = 0; i < ctx->image_size; i++)
        mem[i] = 0;
    kmod_free(mem, ctx->image_size);
    kmod_free(ctx, sizeof(ParseCtx));
    return rc;
}

extern "C" const char *KmodElfErrorString(int32_t rc)
{
    switch (rc) {
    case KMOD_OK: return "ok";
    case KMOD_ERR_INVAL: return "invalid argument";
    case KMOD_ERR_NOMEM: return "out of memory";
    case KMOD_ERR_FORMAT: return "bad elf format";
    case KMOD_ERR_UNRESOLVED: return "unresolved symbol";
    case KMOD_ERR_VERSION: return "version mismatch";
    case KMOD_ERR_CYCLE: return "dependency cycle";
    case KMOD_ERR_CAP: return "capability denied";
    case KMOD_ERR_TIER: return "tier violation";
    case KMOD_ERR_STATE: return "bad state";
    case KMOD_ERR_EXISTS: return "already exists";
    case KMOD_ERR_NOTFOUND: return "not found";
    case KMOD_ERR_BUSY: return "busy (refcount/epoch)";
    case KMOD_ERR_BUDGET: return "watchdog budget exceeded";
    case KMOD_ERR_ROLLBACK: return "rollback applied";
    case KMOD_ERR_CONTRACT: return "contract violation";
    case KMOD_ERR_RELOC: return "unsupported relocation";
    case KMOD_ERR_NAME: return "bad name";
    case KMOD_ERR_IMAGE: return "bad image";
    case KMOD_ERR_POLICY: return "rejected by policy";
    case KMOD_ERR_DEPTH: return "dependency depth exceeded";
    default: return "unknown";
    }
}

extern "C" const char *KmodStateName(uint16_t state)
{
    switch (state) {
    case KMOD_STATE_EMPTY: return "EMPTY";
    case KMOD_STATE_LOADING: return "LOADING";
    case KMOD_STATE_RESOLVING: return "RESOLVING";
    case KMOD_STATE_READY: return "READY";
    case KMOD_STATE_QUIESCING: return "QUIESCING";
    case KMOD_STATE_REPLACING: return "REPLACING";
    case KMOD_STATE_UNLOADING: return "UNLOADING";
    case KMOD_STATE_FAILED: return "FAILED";
    case KMOD_STATE_REMOVED: return "REMOVED";
    default: return "?";
    }
}
