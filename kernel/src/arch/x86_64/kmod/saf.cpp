//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * saf.cpp —— LIMINE 模块归档 (.saf) 解析 / 内核模块池注入
 * ----------------------------------------------------------------------------
 * 目标：Limine 只声明一个
 *      module_path: boot():/boot/modules.saf
 * 该 SAF 里放内核启动依赖的全部 .ko（38 个 resident 组件壳 + demo/扩展）。
 * kernel 解析 SAF 的 root folder，把所有 .ko 文件按 manifest 名称登记进镜像池：
 *   - manifest 名为 kernel.* / core.* 的镜像标记 trusted（内核构建产物），
 *     允许保留名、固定 syscall 槽、能力按清单授予；
 *   - vendor.* 演示模块仍走 STRICT 能力策略；
 *   - 之后 KmodBootLoad() 按 KMOD_MF_BOOT 加载，依赖由 governance 拓扑拉起。
 *
 * SAF 布局见 res/saf/SPECS.md：
 *   folder: { hdr, num_children, u64 children[] }
 *   file  : { hdr, size, u64 addr }
 * 所有 offset/addr 都是相对 archive 起点的字节偏移。
 * ==========================================================================*/
#include <kmod/kmod_internal.h>

#include <stdint.h>
#include <stddef.h>

namespace {

constexpr uint64_t SAF_MAGIC = 0x766863726c706d73ull;
constexpr uint64_t SAF_FLAG_FOLDER = 1ull << 0;
constexpr uint32_t SAF_MAX_DEPTH = 8u;
constexpr uint32_t SAF_MAX_CHILDREN = 4096u;

#pragma pack(push, 1)
struct saf_hdr {
    uint64_t magic;
    uint64_t len;
    char name[256];
    uint64_t flags;
};
struct saf_file {
    saf_hdr hdr;
    uint64_t size;
    uint64_t addr;
};
struct saf_folder {
    saf_hdr hdr;
    uint64_t num_children;
    /* uint64_t children[] follows */
};
#pragma pack(pop)

bool span_ok(uint64_t off, uint64_t len, uint64_t total)
{
    return off <= total && len <= total - off;
}

bool ends_with_ko(const char *name)
{
    if (!name)
        return false;
    uint64_t n = 0;
    while (n < 256u && name[n])
        n++;
    return n >= 3u && name[n - 3u] == '.' && name[n - 2u] == 'k' &&
           name[n - 1u] == 'o';
}

bool header_ok(const saf_hdr *h, uint64_t total)
{
    if (h->magic != SAF_MAGIC)
        return false;
    if (!span_ok(0u, h->len, total))
        return false;
    /* name 必须 NUL 终止（saf-make 会 memcpy 整个 256B 字段） */
    bool nul = false;
    for (uint32_t i = 0; i < 256u; i++) {
        if (h->name[i] == '\0') {
            nul = true;
            break;
        }
    }
    return nul;
}

int32_t ingest_file(const uint8_t *data, uint64_t total, uint64_t off,
                    const char *name, uint32_t *accepted, uint32_t *rejected)
{
    if (!span_ok(off, sizeof(saf_file), total))
        return KMOD_ERR_FORMAT;
    const saf_file *f = (const saf_file *)(data + off);
    if (!header_ok(&f->hdr, total))
        return KMOD_ERR_FORMAT;
    if (!span_ok(f->addr, f->size, total))
        return KMOD_ERR_FORMAT;
    if (!ends_with_ko(name))
        return KMOD_OK;

    const uint8_t *blob = data + f->addr;
    kmod_manifest_t man;
    char err[96];
    err[0] = '\0';
    /* SAF 是内核构建产物：先允许保留名读出 manifest，再按 kernel./core.
     * 前缀决定 trusted；vendor.* 仍按普通 .ko 处理。 */
    int32_t irc = KmodElfInspectEx(blob, f->size, &man, err, sizeof(err), true);
    bool trusted = false;
    if (irc == KMOD_OK)
        trusted = KmodNameIsReserved(man.name);

    int32_t rc = KmodPoolAddEx(blob, f->size, false, trusted);
    if (rc >= 0 || rc == KMOD_ERR_EXISTS) {
        (*accepted)++;
        return KMOD_OK;
    }
    (*rejected)++;
    kmod_log("[kmod] saf: %s rejected rc=%d (%s)%s%s\n", name, (int)rc,
             KmodElfErrorString(rc),
             irc == KMOD_OK ? "" : " inspect=", irc == KMOD_OK ? "" : err);
    return KMOD_OK; /* 坏文件不应阻断其它模块；boot gate 会因缺失失败 */
}

int32_t walk(const uint8_t *data, uint64_t total, uint64_t off,
             uint32_t depth, uint32_t *accepted, uint32_t *rejected)
{
    if (depth > SAF_MAX_DEPTH)
        return KMOD_ERR_DEPTH;
    if (!span_ok(off, sizeof(saf_folder), total))
        return KMOD_ERR_FORMAT;
    const saf_folder *f = (const saf_folder *)(data + off);
    if (!header_ok(&f->hdr, total))
        return KMOD_ERR_FORMAT;
    if ((f->hdr.flags & SAF_FLAG_FOLDER) == 0u)
        return KMOD_ERR_FORMAT;
    if (f->num_children > SAF_MAX_CHILDREN)
        return KMOD_ERR_FORMAT;

    const uint64_t children_off = off + sizeof(saf_folder);
    uint64_t children_bytes = f->num_children * sizeof(uint64_t);
    if (!span_ok(children_off, children_bytes, total))
        return KMOD_ERR_FORMAT;

    for (uint64_t i = 0; i < f->num_children; i++) {
        uint64_t child = 0;
        const uint8_t *p = data + children_off + i * sizeof(uint64_t);
        for (uint32_t b = 0; b < 8u; b++)
            ((uint8_t *)&child)[b] = p[b];
        if (!span_ok(child, sizeof(saf_hdr), total))
            return KMOD_ERR_FORMAT;
        const saf_hdr *h = (const saf_hdr *)(data + child);
        if (!header_ok(h, total))
            return KMOD_ERR_FORMAT;
        char cname[257];
        for (uint32_t k = 0; k < 256u; k++) {
            cname[k] = h->name[k];
            if (cname[k] == '\0')
                break;
            if (k == 255u)
                cname[256] = '\0';
        }
        int32_t rc = 0;
        if (h->flags & SAF_FLAG_FOLDER)
            rc = walk(data, total, child, depth + 1u, accepted, rejected);
        else
            rc = ingest_file(data, total, child, cname, accepted, rejected);
        if (rc != KMOD_OK)
            return rc;
    }
    return KMOD_OK;
}

} /* namespace */

extern "C" int32_t KmodSafIngest(const uint8_t *data, uint64_t size)
{
    if (!data || size < sizeof(saf_folder))
        return KMOD_ERR_FORMAT;

    uint32_t accepted = 0;
    uint32_t rejected = 0;
    int32_t rc = walk(data, size, 0u, 0u, &accepted, &rejected);
    if (rc != KMOD_OK) {
        kmod_log("[kmod] saf: malformed archive rc=%d (%s)\n", (int)rc,
                 KmodElfErrorString(rc));
        return rc;
    }
    kmod_log("[kmod] saf: %u .ko accepted, %u rejected (size=%llu)\n", accepted,
             rejected, (unsigned long long)size);
    return KMOD_OK;
}
