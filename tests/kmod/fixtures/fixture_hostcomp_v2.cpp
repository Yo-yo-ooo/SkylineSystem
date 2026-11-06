// tests/kmod/fixtures/fixture_hostcomp_v2.cpp — 内建组件热替换回退用例 fixture
// ----------------------------------------------------------------------------
// 这是一个"同名 .ko"：manifest 名 vendor.host.comp / 版本 2.0.0。
// component_test 会把它加入镜像池并调用 KmodGovHotReplace("vendor.host.comp",
// "2.0.0")。它的 init 故意返回 KMOD_ERR_INVAL，因此热替换事务必须完整回滚，
// 把内建组件（1.0.0、internal_flags=BUILTIN）恢复成 READY。
// 编译参数与其它 fixture 相同（-mcmodel=large -fno-pic -fno-pie）。
// ----------------------------------------------------------------------------
#include <kmod/kmod_module.h>

extern "C" int hostcomp_v2_hello(int x)
{
    return x + 2000;
}

static int32_t hostcomp_v2_init(void)
{
    return KMOD_ERR_INVAL; // 让热替换在"新 init"阶段失败 → 触发全量回退
}

static void hostcomp_v2_exit(void)
{
}

KMOD_EXPORT_FN(hostcomp_v2_hello, "vendor.host.comp.v2", 2);

KMOD_MANIFEST_FULL("vendor.host.comp", "2.0.0", KMOD_TIER_IMPL,
                   KMOD_MF_HOTREPLACE | KMOD_MF_UNLOADABLE, KMOD_CAP_HOT_REPLACE,
                   0, "host component hotswap-rollback fixture",
                   hostcomp_v2_init, hostcomp_v2_exit, 0, 0, 100u, 50u);
