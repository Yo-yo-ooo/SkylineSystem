// tests/kmod/capability_test.cpp — kmod 能力策略宿主测试
// ----------------------------------------------------------------------------
// 覆盖 KMOD_POLICY_STRICT / OPEN / DISABLED 三种策略、可信清单交集、
// required 缺口拒绝、KMOD_CAP_RESERVED_CORE 拒绝、未定义能力位拒绝、
// KmodCapIsReserved/KmodCapName/KmodCapTrustAdd/KmodCapTrustedAt。
//
// 说明（与 capability.cpp 实现一致）：
//   * 合法能力位只有 bit0..bit18（CAP_DEFINED_MASK）；bit19..bit34 虽在
//     KMOD_CAP_ALL_KNOWN 范围内但未定义，声明即拒绝；bit63 是 Tier0 保留位。
//   * STRICT 下"可信清单之外的模块"返回 KMOD_ERR_POLICY；
//     "可信但 required 有缺口"返回 KMOD_ERR_CAP。
// ----------------------------------------------------------------------------
#include <kmod/kmod_internal.h>

#include <cstdint>
#include <cstdio>

#include "test_common.h"

namespace {

// 每个用例都从已知状态开始：Init 会把策略重置为 STRICT（可信表已初始化后不重建）
void reset_cap(void)
{
    KmodCapInit();
    KmodCapSetMode(KMOD_POLICY_STRICT);
}

// 1) 默认 STRICT + 内置可信清单存在
int32_t test_init_and_trust_table()
{
    reset_cap();
    KCHECK_EQ(KmodCapMode(), KMOD_POLICY_STRICT);
    uint32_t n0 = KmodCapTrustedCount();
    KCHECK_MSG(n0 > 0, "builtin trusted list should not be empty");
    // 内置清单里应有 demo 模块（名单来自 capability.cpp；只断言至少存在一个）
    bool found_demo = false;
    for (uint32_t i = 0; i < n0; i++) {
        const kmod_trusted_entry_t *e = KmodCapTrustedAt(i);
        KCHECK(e != nullptr);
        KCHECK(e->name != nullptr && e->name[0] != '\0');
        if (std::strncmp(e->name, "vendor.demo.", 12) == 0)
            found_demo = true;
    }
    KCHECK(found_demo);
    // 越界下标返回 nullptr
    KCHECK(KmodCapTrustedAt(n0 + 1000) == nullptr);

    // TrustAdd：唯一名字 -> count+1；重复名字更新 caps 不增计数
    int32_t rc = KmodCapTrustAdd("test.mod.a", KMOD_CAP_LOG | KMOD_CAP_MEM_ALLOC);
    KCHECK_EQ(rc, KMOD_OK);
    KCHECK_EQ(KmodCapTrustedCount(), n0 + 1);
    rc = KmodCapTrustAdd("test.mod.a", KMOD_CAP_LOG);
    KCHECK_EQ(rc, KMOD_OK);
    KCHECK_EQ(KmodCapTrustedCount(), n0 + 1); // 原地更新
    // 非法名字 / 保留位 / 未定义位 拒绝
    KCHECK_EQ(KmodCapTrustAdd("badname", KMOD_CAP_LOG), KMOD_ERR_NAME);
    KCHECK_EQ(KmodCapTrustAdd(nullptr, KMOD_CAP_LOG), KMOD_ERR_NAME);
    KCHECK_EQ(KmodCapTrustAdd("test.mod.b", KMOD_CAP_RESERVED_CORE),
              KMOD_ERR_CAP);
    KCHECK_EQ(KmodCapTrustAdd("test.mod.b", 1ull << 40), KMOD_ERR_CAP);
    return 0;
}

// 2) STRICT：未知模块拒绝（POLICY），可信模块 granted=声明∩清单
int32_t test_strict()
{
    reset_cap();
    uint64_t granted = 0xdeadbeefull, denied = 0xdeadbeefull;
    int32_t rc = KmodCapGrant("vendor.unknown.module", KMOD_CAP_LOG, 0,
                              &granted, &denied);
    KCHECK_EQ(rc, KMOD_ERR_POLICY);
    KCHECK_EQ_U64(granted, 0u);
    KCHECK_EQ_U64(denied, KMOD_CAP_LOG);

    rc = KmodCapTrustAdd("test.mod.grant", KMOD_CAP_LOG | KMOD_CAP_MEM_ALLOC);
    KCHECK_EQ(rc, KMOD_OK);

    // required 完全在清单内 -> OK；optional 未授权项进 denied
    granted = denied = 0xdeadbeefull;
    rc = KmodCapGrant("test.mod.grant", KMOD_CAP_LOG | KMOD_CAP_MEM_ALLOC,
                      KMOD_CAP_IRQ, &granted, &denied);
    KCHECK_EQ(rc, KMOD_OK);
    KCHECK_EQ_U64(granted, KMOD_CAP_LOG | KMOD_CAP_MEM_ALLOC);
    KCHECK_EQ_U64(denied, KMOD_CAP_IRQ);

    // required 有缺口 -> KMOD_ERR_CAP（granted 记录交集，denied 记录缺口）
    granted = denied = 0xdeadbeefull;
    rc = KmodCapGrant("test.mod.grant", KMOD_CAP_LOG | KMOD_CAP_IRQ,
                      KMOD_CAP_NET, &granted, &denied);
    KCHECK_EQ(rc, KMOD_ERR_CAP);
    KCHECK_EQ_U64(granted, KMOD_CAP_LOG);
    KCHECK_EQ_U64(denied, KMOD_CAP_IRQ | KMOD_CAP_NET);

    // 空 required 但 optional 全给：OK
    granted = denied = 0xdeadbeefull;
    rc = KmodCapGrant("test.mod.grant", 0, KMOD_CAP_LOG, &granted, &denied);
    KCHECK_EQ(rc, KMOD_OK);
    KCHECK_EQ_U64(granted, KMOD_CAP_LOG);
    KCHECK_EQ_U64(denied, 0u);

    // 空模块名
    KCHECK_EQ(KmodCapGrant(nullptr, KMOD_CAP_LOG, 0, nullptr, nullptr),
              KMOD_ERR_NAME);
    KCHECK_EQ(KmodCapGrant("", KMOD_CAP_LOG, 0, nullptr, nullptr),
              KMOD_ERR_NAME);
    return 0;
}

// 3) 保留位 / 未定义能力位：STRICT 与 OPEN 下都整单拒绝
int32_t test_reserved_and_undefined_bits()
{
    // 位合法性辅助
    KCHECK(KmodCapIsReserved(KMOD_CAP_RESERVED_CORE));
    KCHECK(!KmodCapIsReserved(KMOD_CAP_LOG));
    KCHECK(!KmodCapIsReserved(0u));
    KCHECK(KmodCapIsReserved(KMOD_CAP_RESERVED_CORE | KMOD_CAP_LOG));

    // 名称映射
    KCHECK(std::strcmp(KmodCapName(KMOD_CAP_MEM_ALLOC), "mem_alloc") == 0);
    KCHECK(std::strcmp(KmodCapName(KMOD_CAP_LOG), "log") == 0);
    KCHECK(std::strcmp(KmodCapName(KMOD_CAP_RESERVED_CORE), "reserved_core") == 0);
    KCHECK(std::strcmp(KmodCapName(1ull << 40), "unknown") == 0);

    KCHECK_EQ(KmodCapTrustAdd("test.mod.resv", KMOD_CAP_LOG), KMOD_OK);

    for (uint32_t mode : {KMOD_POLICY_STRICT, KMOD_POLICY_OPEN}) {
        reset_cap();
        KmodCapSetMode(mode);
        KCHECK_EQ(KmodCapMode(), mode);
        uint64_t g = 0, d = 0;
        // RESERVED_CORE 出现在 required
        KCHECK_EQ(KmodCapGrant("test.mod.resv", KMOD_CAP_RESERVED_CORE, 0, &g, &d),
                  KMOD_ERR_CAP);
        KCHECK_EQ_U64(g, 0u);
        KCHECK_EQ_U64(d, KMOD_CAP_RESERVED_CORE);
        // RESERVED_CORE 出现在 optional
        KCHECK_EQ(KmodCapGrant("test.mod.resv", 0, KMOD_CAP_RESERVED_CORE, &g, &d),
                  KMOD_ERR_CAP);
        // 未定义位 bit40 / bit62（KMOD_CAP_ALL_KNOWN 之外/之内但未定义）
        KCHECK_EQ(KmodCapGrant("test.mod.resv", 1ull << 40, 0, &g, &d),
                  KMOD_ERR_CAP);
        KCHECK_EQ(KmodCapGrant("test.mod.resv", 1ull << 62, 0, &g, &d),
                  KMOD_ERR_CAP);
        // bit19..bit34：在 ALL_KNOWN 内但未定义，同样拒绝
        KCHECK_EQ(KmodCapGrant("test.mod.resv", 1ull << 19, 0, &g, &d),
                  KMOD_ERR_CAP);
        KCHECK_EQ(KmodCapGrant("test.mod.resv", 1ull << 34, 0, &g, &d),
                  KMOD_ERR_CAP);
    }
    return 0;
}

// 4) OPEN 模式：未知模块按声明授予（仍拒绝保留/未定义位）
int32_t test_open()
{
    reset_cap();
    KmodCapSetMode(KMOD_POLICY_OPEN);
    KCHECK_EQ(KmodCapMode(), KMOD_POLICY_OPEN);
    uint64_t granted = 0, denied = 0xdeadbeefull;
    int32_t rc = KmodCapGrant("vendor.unknown.openmod",
                              KMOD_CAP_LOG | KMOD_CAP_NET, KMOD_CAP_IRQ,
                              &granted, &denied);
    KCHECK_EQ(rc, KMOD_OK);
    KCHECK_EQ_U64(granted, KMOD_CAP_LOG | KMOD_CAP_NET | KMOD_CAP_IRQ);
    KCHECK_EQ_U64(denied, 0u);

    // OPEN 下保留位仍拒绝
    KCHECK_EQ(KmodCapGrant("vendor.unknown.openmod", KMOD_CAP_RESERVED_CORE, 0,
                           &granted, &denied),
              KMOD_ERR_CAP);
    // OPEN 下未定义位仍拒绝
    KCHECK_EQ(KmodCapGrant("vendor.unknown.openmod", 0, 1ull << 50, &granted,
                           &denied),
              KMOD_ERR_CAP);
    reset_cap(); // 恢复 STRICT
    return 0;
}

// 5) DISABLED 模式：所有加载请求拒绝（POLICY）
int32_t test_disabled()
{
    reset_cap();
    KmodCapSetMode(KMOD_POLICY_DISABLED);
    KCHECK_EQ(KmodCapMode(), KMOD_POLICY_DISABLED);
    uint64_t g = 0xdeadbeefull, d = 0xdeadbeefull;
    // 即便是可信清单内模块也拒绝
    KCHECK_EQ(KmodCapGrant("vendor.demo.hello", KMOD_CAP_LOG, 0, &g, &d),
              KMOD_ERR_POLICY);
    KCHECK_EQ(KmodCapGrant("test.mod.resv", 0, 0, &g, &d), KMOD_ERR_POLICY);
    KCHECK_EQ(KmodCapGrant("vendor.unknown.disabled", KMOD_CAP_LOG, 0, &g, &d),
              KMOD_ERR_POLICY);
    // 非法 mode 被忽略（保持 DISABLED）
    KmodCapSetMode(0x12345678u);
    KCHECK_EQ(KmodCapMode(), KMOD_POLICY_DISABLED);
    reset_cap();
    return 0;
}

} // namespace

extern "C" int32_t test_capability_run(void)
{
    int32_t bad = 0;
    if (test_init_and_trust_table() != 0)
        bad++;
    if (test_strict() != 0)
        bad++;
    if (test_reserved_and_undefined_bits() != 0)
        bad++;
    if (test_open() != 0)
        bad++;
    if (test_disabled() != 0)
        bad++;
    return bad == 0 ? 0 : 1;
}
