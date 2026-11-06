//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * kmod_pool.cpp —— 模块镜像池（Tier0）
 * ----------------------------------------------------------------------------
 * 镜像池只登记"可加载的 .ko 原始 blob"及其清单身份（不重定位、不执行）：
 *   - 引导期由 Limine module response 拿到的 boot():/boot/modules 下的 .ko 文件；
 *   - 运行期热替换用的新版本镜像。
 * 事务层（governance）按名/版本从池里选镜像做加载规划；同一 name 允许多版本
 * 共存（热替换/回滚需要），但同一 (name,version) 只允许登记一次。
 * ==========================================================================*/
#include <kmod/kmod_internal.h>

kmod_image_t g_kmod_images[KMOD_MAX_IMAGES];
uint32_t g_kmod_image_count = 0;

namespace {

int32_t pool_find_exact(const char *name, const char *version)
{
    for (uint32_t i = 0; i < g_kmod_image_count; i++) {
        kmod_image_t *img = &g_kmod_images[i];
        if (img->parse_ret != KMOD_OK)
            continue;
        const char *a = img->name;
        const char *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a != '\0' || *b != '\0')
            continue;
        const char *va = img->version;
        const char *vb = version;
        while (*va && *va == *vb) { va++; vb++; }
        if (*va == '\0' && *vb == '\0')
            return (int32_t)i;
    }
    return KMOD_ERR_NOTFOUND;
}

} /* namespace */

extern "C" int32_t KmodPoolAdd(const uint8_t *data, uint64_t size, bool owned)
{
    if (!data || size < sizeof(kmod_manifest_t)) {
        kmod_log("[kmod] pool: image too small (%llu bytes)\n",
                 (unsigned long long)size);
        return KMOD_ERR_FORMAT;
    }
    if (g_kmod_image_count >= KMOD_MAX_IMAGES) {
        kmod_log("[kmod] pool: full (%u entries)\n", g_kmod_image_count);
        return KMOD_ERR_NOMEM;
    }
    kmod_manifest_t man;
    char err[96];
    int32_t rc = KmodElfInspect(data, size, &man, err, sizeof(err));
    if (rc != KMOD_OK) {
        kmod_log("[kmod] pool: inspect rejected image (%s)\n", err);
        return rc;
    }
    /* 同 (name,version) 去重 */
    int32_t dup = pool_find_exact(man.name, man.version);
    if (dup >= 0) {
        kmod_log("[kmod] pool: duplicate image %s@%s (ignored)\n",
                 man.name, man.version);
        return KMOD_ERR_EXISTS;
    }
    kmod_image_t *img = &g_kmod_images[g_kmod_image_count];
    for (uint64_t i = 0; i < sizeof(*img); i++)
        ((uint8_t *)img)[i] = 0;
    /* 字段逐个拷（避免任何越界：inspect 已保证 NUL 在界内） */
    {
        uint32_t i = 0;
        while (i + 1 < KMOD_NAME_MAX && man.name[i]) { img->name[i] = man.name[i]; i++; }
        img->name[i] = '\0';
        i = 0;
        while (i + 1 < KMOD_VERSION_MAX && man.version[i]) { img->version[i] = man.version[i]; i++; }
        img->version[i] = '\0';
    }
    img->data = data;
    img->size = size;
    img->tier = man.tier;
    img->flags = man.flags;
    img->caps_required = man.required_caps;
    img->caps_optional = man.optional_caps;
    img->init_budget_ms = man.init_budget_ms;
    img->quiesce_budget_ms = man.quiesce_budget_ms;
    img->parse_ret = KMOD_OK;
    img->inspected = true;
    img->owned = owned;
    g_kmod_image_count++;
    kmod_log("[kmod] pool: + %s@%s (%llu bytes, tier=%u, flags=0x%x, caps=0x%llx)\n",
             img->name, img->version, (unsigned long long)size,
             (unsigned)img->tier, (unsigned)img->flags,
             (unsigned long long)img->caps_required);
    return (int32_t)(g_kmod_image_count - 1);
}

extern "C" int32_t KmodPoolFind(const char *name, const char *version)
{
    if (!name || !name[0])
        return KMOD_ERR_INVAL;
    if (version && version[0])
        return pool_find_exact(name, version);
    /* 未指定版本：同名镜像里挑最高版本 */
    int32_t best = KMOD_ERR_NOTFOUND;
    for (uint32_t i = 0; i < g_kmod_image_count; i++) {
        kmod_image_t *img = &g_kmod_images[i];
        if (img->parse_ret != KMOD_OK)
            continue;
        const char *a = img->name;
        const char *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a != '\0' || *b != '\0')
            continue;
        if (best < 0) {
            best = (int32_t)i;
        } else {
            kmod_image_t *bimg = &g_kmod_images[best];
            if (KmodVersionCompare(img->version, bimg->version) > 0)
                best = (int32_t)i;
        }
    }
    return best;
}
