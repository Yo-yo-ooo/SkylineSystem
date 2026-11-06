// tests/kmod/fixtures/fixture_a.cpp — 宿主 ELF 加载器测试 fixture（provider 模块）
// ----------------------------------------------------------------------------
// 用模块作者头 <kmod/kmod_module.h> 写成的最小 .ko：
//   - manifest: vendor.test.a / 1.0.0 / Tier IMPL / KMOD_MF_BOOT / 需要 CAP_MEM_ALLOC
//   - 导出: fixture_a_hello (FUNC, 100 + x)、fixture_a_counter_get (FUNC)、
//           fixture_a_kexp_call (FUNC, 调用直接外部符号)、g_counter (DATA)、
//           fixture_a_ops (CONTRACT, 私有契约 vendor.test.a.ops.v1)
//   - 导入: kernel 的 kprintf_（.in 槽 klog_，验证 .in 结构/槽可写，由治理层绑定）
//   - 直接外部引用 kmod_fixture_kexp_probe：编译器生成的未定义符号，加载器
//     必须用传入的内核导出表(kexp)解析；缺失即 KMOD_ERR_UNRESOLVED（负向用例）。
//     生产模块应走 KMOD_IMPORT 槽；这里刻意保留直接引用以覆盖 loader 的 kexp 路径。
// 编译：-mcmodel=large -fno-pic -fno-pie -fno-stack-protector -O2
//       （宿主 g++ 产物仍是 ET_REL EM_X86_64；large 模型产生 64 位绝对重定位）
// ----------------------------------------------------------------------------
#include <kmod/kmod_module.h>

// ---- 模块内数据 ----
static int g_counter = 7;

// ---- 导入（.in）：kernel 日志出口（槽名 klog_，导入符号名 kprintf_）----
KMOD_IMPORT_FN(klog_, int, (const char *, ...), "kernel", "kprintf_",
               "kernel.log.v1");

// ---- 编译器生成的未定义符号：由加载器的 kexp 表解析 ----
extern "C" int kmod_fixture_kexp_probe(int x);

// ---- 导出函数（.out）----
extern "C" int fixture_a_hello(int x)
{
    return x + 100;
}

extern "C" int fixture_a_counter_get(void)
{
    return g_counter;
}

extern "C" int fixture_a_kexp_call(int x)
{
    return kmod_fixture_kexp_probe(x) + 1;
}

typedef struct fixture_a_ops {
    int (*hello)(int x);
    int (*counter_get)(void);
} fixture_a_ops_t;

static fixture_a_ops_t g_ops = {fixture_a_hello, fixture_a_counter_get};

// ---- 生命周期 ----
static int32_t fixture_a_init(void)
{
    klog_("[fixture_a] init (counter=%d)\n", g_counter);
    g_counter++;
    return KMOD_OK;
}

static void fixture_a_exit(void)
{
    klog_("[fixture_a] exit\n");
}

// ---- 声明导出/清单 ----
KMOD_EXPORT_FN(fixture_a_hello, "vendor.test.a.v1", 1);
KMOD_EXPORT_FN(fixture_a_counter_get, "vendor.test.a.v1", 1);
KMOD_EXPORT_FN(fixture_a_kexp_call, "vendor.test.a.v1", 1);
KMOD_EXPORT_DATA(g_counter, "vendor.test.a.v1", 1, sizeof(g_counter));
KMOD_EXPORT_CONTRACT("vendor.test.a.ops.v1", g_ops, 1);

KMOD_MANIFEST("vendor.test.a", "1.0.0", KMOD_TIER_IMPL, KMOD_MF_BOOT,
              KMOD_CAP_MEM_ALLOC, fixture_a_init, fixture_a_exit);
