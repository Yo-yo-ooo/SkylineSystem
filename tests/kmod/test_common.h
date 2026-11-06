// tests/kmod/test_common.h — kmod 宿主测试的公共小工具
// 仅测试侧使用：断言宏、固定种子 RNG、fixture 读取、最小 ELF64 解析/改字节工具。
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cstring>
#include <string>
#include <vector>

// ============================================================================
// 断言：失败打印文件/行/条件后 return 1（每个 test_<name>_run 都返回 int32_t）
// ============================================================================
#define KT_FAIL(fmt, ...)                                                     \
    do {                                                                      \
        std::printf("  FAIL %s:%d: " fmt "\n", __FILE__, __LINE__,            \
                    ##__VA_ARGS__);                                           \
    } while (0)

#define KT_INFO(fmt, ...)                                                     \
    do {                                                                      \
        std::printf("  [info] " fmt "\n", ##__VA_ARGS__);                     \
    } while (0)

#define KCHECK(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            KT_FAIL("%s", #cond);                                             \
            return 1;                                                         \
        }                                                                     \
    } while (0)

#define KCHECK_MSG(cond, fmt, ...)                                            \
    do {                                                                      \
        if (!(cond)) {                                                        \
            KT_FAIL(fmt " :: (%s)", ##__VA_ARGS__, #cond);                    \
            return 1;                                                         \
        }                                                                     \
    } while (0)

#define KCHECK_EQ(a, b)                                                       \
    do {                                                                      \
        auto _ka = (a);                                                       \
        auto _kb = (b);                                                       \
        if (!(_ka == _kb)) {                                                  \
            KT_FAIL("%s != %s (left=%lld right=%lld)", #a, #b,                \
                    (long long)_ka, (long long)_kb);                          \
            return 1;                                                         \
        }                                                                     \
    } while (0)

#define KCHECK_EQ_U64(a, b)                                                   \
    do {                                                                      \
        uint64_t _ka = (uint64_t)(a);                                         \
        uint64_t _kb = (uint64_t)(b);                                         \
        if (_ka != _kb) {                                                     \
            KT_FAIL("%s != %s (left=%llu right=%llu)", #a, #b,                \
                    (unsigned long long)_ka, (unsigned long long)_kb);        \
            return 1;                                                         \
        }                                                                     \
    } while (0)

// 负向用例通用断言：rc 必须是错误码（<0），且不崩溃
#define KCHECK_ERR(rc)                                                        \
    do {                                                                      \
        int32_t _rc = (int32_t)(rc);                                          \
        if (_rc >= 0) {                                                       \
            KT_FAIL("expected negative rc for %s, got %d", #rc, (int)_rc);    \
            return 1;                                                         \
        }                                                                     \
    } while (0)

// ============================================================================
// 固定种子 RNG（xorshift64*，论文口径；失败可重放）
// ============================================================================
struct KRng {
    uint64_t s;
    explicit KRng(uint64_t seed) : s(seed ? seed : 0x9e3779b97f4a7c15ull) {}
    uint64_t next()
    {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 2685821657736338717ull;
    }
    uint32_t u32() { return static_cast<uint32_t>(next() >> 32); }
    uint32_t below(uint32_t n) { return n ? (u32() % n) : 0u; }
    uint64_t next_u64() { return next(); }
};

// 测试随机种子（与 tests/README.md 口径一致：硬编码、可重放）
#define KMOD_SEED_TOPO 0x6b6d6f64ull // "kmod"
#define KMOD_SEED_FUZZ 0xdeadc0deull

// ============================================================================
// fixture .o 读取
// ============================================================================
inline const char *kmod_fixture_dir(void)
{
    // 优先使用 Makefile 注入的绝对路径；同时容忍从 tests/ 或仓库根启动
#ifdef KMOD_FIXTURE_DIR
    static const char *candidates[] = {
        KMOD_FIXTURE_DIR,
        "bin/kmod_fixtures",
        "../bin/kmod_fixtures",
        "../../tests/bin/kmod_fixtures",
        "tests/bin/kmod_fixtures",
    };
#else
    static const char *candidates[] = {
        "bin/kmod_fixtures",
        "../bin/kmod_fixtures",
        "tests/bin/kmod_fixtures",
    };
#endif
    for (const char *c : candidates) {
        std::string probe = std::string(c) + "/fixture_a.o";
        FILE *f = std::fopen(probe.c_str(), "rb");
        if (f) {
            std::fclose(f);
            return c;
        }
    }
    return candidates[0];
}

inline bool kmod_read_file(const std::string &path, std::vector<uint8_t> &out)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    if (n < 0) {
        std::fclose(f);
        return false;
    }
    std::fseek(f, 0, SEEK_SET);
    out.resize(static_cast<size_t>(n));
    size_t got = n ? std::fread(out.data(), 1, out.size(), f) : 0;
    std::fclose(f);
    return got == out.size();
}

inline bool kmod_read_fixture(const char *name, std::vector<uint8_t> &out)
{
    std::string p = std::string(kmod_fixture_dir()) + "/" + name;
    return kmod_read_file(p, out);
}

// ============================================================================
// 最小 ELF64 解析（只为负向用例定位并篡改字段；不做加载）
// ============================================================================
struct KElfEhdr {
    uint8_t e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct KElfShdr {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
};

static_assert(sizeof(KElfEhdr) == 64, "ELF64 Ehdr layout");
static_assert(sizeof(KElfShdr) == 64, "ELF64 Shdr layout");

inline bool kelf_ehdr(const std::vector<uint8_t> &d, KElfEhdr *out)
{
    if (d.size() < sizeof(KElfEhdr))
        return false;
    std::memcpy(out, d.data(), sizeof(KElfEhdr));
    return true;
}

inline void kelf_write_ehdr(std::vector<uint8_t> &d, const KElfEhdr &e)
{
    if (d.size() < sizeof(KElfEhdr))
        d.resize(sizeof(KElfEhdr));
    std::memcpy(d.data(), &e, sizeof(KElfEhdr));
}

struct KElfSection {
    uint32_t index = 0;
    std::string name;
    uint32_t type = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
    uint64_t entsize = 0;
    uint32_t link = 0;
    uint32_t info = 0;
};

// 解析节表（恶意构造时任何一处越界即返回 false，不崩溃）
inline bool kelf_sections(const std::vector<uint8_t> &d,
                          std::vector<KElfSection> &out)
{
    out.clear();
    KElfEhdr e;
    if (!kelf_ehdr(d, &e))
        return false;
    if (e.e_shentsize != sizeof(KElfShdr))
        return false;
    if (e.e_shnum == 0 || e.e_shnum > 4096)
        return false;
    uint64_t shoff = e.e_shoff;
    uint64_t need = shoff + (uint64_t)e.e_shnum * sizeof(KElfShdr);
    if (shoff > d.size() || need > d.size())
        return false;
    std::vector<KElfShdr> shdrs(e.e_shnum);
    std::memcpy(shdrs.data(), d.data() + shoff,
                (size_t)e.e_shnum * sizeof(KElfShdr));
    const KElfShdr *shstr = nullptr;
    if (e.e_shstrndx < e.e_shnum)
        shstr = &shdrs[e.e_shstrndx];
    for (uint32_t i = 0; i < e.e_shnum; i++) {
        const KElfShdr &sh = shdrs[i];
        KElfSection s;
        s.index = i;
        s.type = sh.sh_type;
        s.offset = sh.sh_offset;
        s.size = sh.sh_size;
        s.entsize = sh.sh_entsize;
        s.link = sh.sh_link;
        s.info = sh.sh_info;
        if (shstr && shstr->sh_offset <= d.size() &&
            sh.sh_name < shstr->sh_size &&
            shstr->sh_offset + shstr->sh_size <= d.size()) {
            const char *p = reinterpret_cast<const char *>(
                d.data() + shstr->sh_offset + sh.sh_name);
            size_t maxlen = (size_t)(shstr->sh_size - sh.sh_name);
            size_t len = 0;
            while (len < maxlen && p[len] != '\0')
                len++;
            s.name.assign(p, len);
        }
        if (s.type != 8 /*SHT_NOBITS*/ && s.size != 0 &&
            (s.offset > d.size() || s.offset + s.size > d.size())) {
            return false;
        }
        out.push_back(std::move(s));
    }
    return true;
}

inline const KElfSection *kelf_find(const std::vector<KElfSection> &secs,
                                    const char *name)
{
    for (const auto &s : secs)
        if (s.name == name)
            return &s;
    return nullptr;
}

inline const KElfSection *kelf_find_type(const std::vector<KElfSection> &secs,
                                         uint32_t type)
{
    for (const auto &s : secs)
        if (s.type == type)
            return &s;
    return nullptr;
}

// 往 ELF 数据里按字节偏移改字段（小端；测试只在 x86_64 宿主跑）
inline void kelf_patch_u16(std::vector<uint8_t> &d, size_t off, uint16_t v)
{
    std::memcpy(d.data() + off, &v, sizeof(v));
}
inline void kelf_patch_u32(std::vector<uint8_t> &d, size_t off, uint32_t v)
{
    std::memcpy(d.data() + off, &v, sizeof(v));
}
inline void kelf_patch_u64(std::vector<uint8_t> &d, size_t off, uint64_t v)
{
    std::memcpy(d.data() + off, &v, sizeof(v));
}
inline uint64_t kelf_read_u64(const std::vector<uint8_t> &d, size_t off)
{
    uint64_t v = 0;
    std::memcpy(&v, d.data() + off, sizeof(v));
    return v;
}
inline uint32_t kelf_read_u32(const std::vector<uint8_t> &d, size_t off)
{
    uint32_t v = 0;
    std::memcpy(&v, d.data() + off, sizeof(v));
    return v;
}

// Ehdr 关键字段的字节偏移（ELF64 固定布局）
enum : size_t {
    KELF_OFF_E_TYPE = 16,
    KELF_OFF_E_MACHINE = 18,
    KELF_OFF_E_SHOFF = 40,
    KELF_OFF_E_SHENTSIZE = 58,
    KELF_OFF_E_SHNUM = 60,
    KELF_OFF_E_SHSTRNDX = 62,
    KELF_OFF_EI_CLASS = 4,
    KELF_OFF_EI_DATA = 5,
    KELF_OFF_EI_VERSION = 6,
};
