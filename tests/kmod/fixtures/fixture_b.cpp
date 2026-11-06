// tests/kmod/fixtures/fixture_b.cpp — 宿主 ELF 加载器测试 fixture（consumer 模块）
// ----------------------------------------------------------------------------
//   - manifest: vendor.test.b / 1.0.0 / Tier IMPL / 不需要能力
//   - 导入: vendor.test.a 的 fixture_a_hello（模块间导入，验证加载器回填/绑定路径）
//   - 导出: fixture_b_hello_plus_one (FUNC) —— 调用导入槽 +1
// 说明：加载器本身只拿到内核导出表；模块间导入由 governance 的绑定逻辑（或测试
// 手工）把 A 的导出地址写进 B 的 .in 槽。本 fixture 让两条路径都能被验证。
// ----------------------------------------------------------------------------
#include <kmod/kmod_module.h>

// ---- 导入（.in）：provider 模块 vendor.test.a 的导出函数 ----
typedef int (*fixture_hello_fn_t)(int x);
KMOD_IMPORT_FN(hello_fn, int, (int), "vendor.test.a", "fixture_a_hello",
               "vendor.test.a.v1");

// ---- 导出函数（.out）----
extern "C" int fixture_b_hello_plus_one(int x)
{
    // 未绑定时 hello_fn 为 NULL —— 调用即崩，正好是回填断言的反向证明。
    return hello_fn(x) + 1;
}

extern "C" int fixture_b_import_bound(void)
{
    return hello_fn != 0;
}

static int32_t fixture_b_init(void)
{
    return KMOD_OK;
}

static void fixture_b_exit(void)
{
}

KMOD_EXPORT_FN(fixture_b_hello_plus_one, "vendor.test.b.v1", 1);
KMOD_EXPORT_FN(fixture_b_import_bound, "vendor.test.b.v1", 1);

KMOD_MANIFEST("vendor.test.b", "1.0.0", KMOD_TIER_IMPL, 0, 0, fixture_b_init,
              fixture_b_exit);
