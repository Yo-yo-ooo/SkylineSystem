// tests/kmod/test_main.cpp — kmod 宿主测试的唯一 main
// ----------------------------------------------------------------------------
// 约定（所有测试文件遵守）：
//   * 每个 *_test.cpp 提供 extern "C" int32_t test_<name>_run(void)；
//     返回 0 = 该测试全通过，非 0 = 失败（自行 printf 失败断言行）。
//   * 本文件是唯一 main：遍历测试表，打印
//         KMOD-TEST <name> PASS|FAIL|SKIP
//     与汇总
//         KMOD-TEST RESULT PASS x/y
//     全部执行通过退出 0，否则 1。
//   * 用 weak 符号引用各测试入口：测试文件缺失/未被 Makefile 收集时打印 SKIP。
//     topology/capability/elf_loader 是核心门禁，缺失 → INCOMPLETE 退出 1
//     （防止内核文件没落地时门禁假绿）；gov/syscall 允许 SKIP（由其他任务交付）。
//   * 可选用命令行参数筛选：./bin/kmod_test elf_loader topology
// ----------------------------------------------------------------------------
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" {
int32_t test_topology_run(void) __attribute__((weak));
int32_t test_capability_run(void) __attribute__((weak));
int32_t test_elf_loader_run(void) __attribute__((weak));
int32_t test_gov_run(void) __attribute__((weak));
int32_t test_syscall_run(void) __attribute__((weak));
int32_t test_component_run(void) __attribute__((weak));
}

namespace {

struct TestEntry {
    const char *name;
    int32_t (*fn)(void);
    bool required; // 缺失时门禁算 INCOMPLETE
};

const TestEntry kTests[] = {
    {"topology", test_topology_run, true},
    {"capability", test_capability_run, true},
    {"elf_loader", test_elf_loader_run, true},
    {"gov", test_gov_run, false},
    {"syscall", test_syscall_run, false},
    {"component", test_component_run, true},
};

int run_all(int argc, char **argv)
{
    int ran = 0, passed = 0, failed = 0, skipped = 0;
    bool missing_required = false;
    char skipped_names[256] = {};
    size_t skip_len = 0;

    for (const TestEntry &t : kTests) {
        // 命令行筛选（无参数 = 全跑）
        bool selected = (argc <= 1);
        for (int i = 1; i < argc; i++) {
            if (std::strcmp(argv[i], t.name) == 0) {
                selected = true;
                break;
            }
        }
        if (!selected)
            continue;

        if (t.fn == nullptr) {
            std::printf("KMOD-TEST %s SKIP (symbol not linked)\n", t.name);
            skipped++;
            if (t.required)
                missing_required = true;
            if (skip_len + std::strlen(t.name) + 4 < sizeof(skipped_names)) {
                if (skip_len)
                    skipped_names[skip_len++] = ',';
                std::strcpy(skipped_names + skip_len, t.name);
                skip_len += std::strlen(t.name);
            }
            continue;
        }

        std::printf("KMOD-TEST %s RUN\n", t.name);
        int32_t rc = t.fn();
        ran++;
        if (rc == 0) {
            passed++;
            std::printf("KMOD-TEST %s PASS\n", t.name);
        } else {
            failed++;
            std::printf("KMOD-TEST %s FAIL (rc=%d)\n", t.name, (int)rc);
        }
    }

    if (failed == 0 && !missing_required) {
        std::printf("KMOD-TEST RESULT PASS %d/%d", passed, ran);
        if (skipped)
            std::printf(" (skipped: %s)", skipped_names);
        std::printf("\n");
        return 0;
    }
    if (missing_required) {
        std::printf("KMOD-TEST RESULT INCOMPLETE %d/%d", passed, ran);
        if (skipped)
            std::printf(" (skipped: %s)", skipped_names);
        std::printf("\n");
        return 1;
    }
    std::printf("KMOD-TEST RESULT FAIL %d/%d\n", passed, ran);
    return 1;
}

} // namespace

int main(int argc, char **argv)
{
    // 立即输出：ASAN 崩溃时日志也不能丢
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("===== kmod host test gate =====\n");
    int rc = run_all(argc, argv);
    std::printf("===== kmod host test gate done (rc=%d) =====\n", rc);
    return rc;
}
