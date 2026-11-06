# tests/kmod/Makefile — kmod 宿主测试门禁（由 tests/Makefile 的 `kmod` 目标委托）
# ============================================================================
# 产物:
#    ../bin/kmod_test                普通构建（-O2），目标 `kmod`
#    ../bin/kmod_test_asan         ASAN+UBSAN 构建（elf_loader 负向/fuzz 用），目标 `kmod-asan`
#    ../bin/kmod_fixtures/*.o     模块 fixture（-fno-pic -fno-pie -mcmodel=large -> ET_REL x86-64）
#
# 可移植性：kernel/src/arch/x86_64/kmod/ 下的实现文件可能陆续落地，这里用
#    存在性过滤把它们纳入/排除，并打印缺失清单（方便 Lead 看进度），不阻断构建。
#    依赖关系也做了过滤（例如 elf_loader.cpp 需要 topology.cpp 的命名/版本校验符号），
#    避免中间态出现链接错误；对应测试文件同样按依赖存在性过滤。
#
# 唯一 main = test_main.cpp；每个 *_test.cpp 提供 extern "C" int32_t test_<name>_run()。
# ============================================================================

# CXX 选择与 tests/Makefile 同策略: 取第一个能编 <mutex> 的编译器。
# （父 make 通过 `CXX='$(CXX)'` 传入时以传入值为准）
ifeq ($(origin CXX),file)
CXX := $(shell for c in g++ g++-14 g++-13 clang++-18 clang++; do \
	command -v $$c >/dev/null 2>&1 || continue; \
	printf '#include <mutex>\n#include <thread>\nint main(){}\n' > /tmp/sky_cxx_probe_kmod.cpp; \
	if $$c -std=gnu++17 -pthread /tmp/sky_cxx_probe_kmod.cpp -o /tmp/sky_cxx_probe_kmod.out >/dev/null 2>&1; then echo $$c; break; fi; \
done)
endif
ifeq ($(strip $(CXX)),)
CXX := g++
endif
$(info [kmod] CXX = $(CXX))

# ----------------------------------------------------------------------------
# 源码清单（存在性过滤 + 依赖闭包过滤）
# ----------------------------------------------------------------------------
KMOD_DIR      := ../../kernel/src/arch/x86_64/kmod
HAVE_TOPO     := $(wildcard $(KMOD_DIR)/topology.cpp)
HAVE_CAP      := $(wildcard $(KMOD_DIR)/capability.cpp)
HAVE_ELF      := $(if $(HAVE_TOPO),$(wildcard $(KMOD_DIR)/elf_loader.cpp))
HAVE_POOL     := $(if $(HAVE_ELF),$(wildcard $(KMOD_DIR)/kmod_pool.cpp))
HAVE_SEAT     := $(wildcard $(KMOD_DIR)/seat.cpp)
HAVE_SYSCALL := $(wildcard $(KMOD_DIR)/syscall_registry.cpp)
HAVE_KEXP     := $(wildcard $(KMOD_DIR)/kernel_exports.cpp)
HAVE_SERVICES := $(wildcard $(KMOD_DIR)/kernel_services.cpp)
# governance 依赖 elf_loader/pool/capability/seat/syscall_registry 全部就绪
HAVE_GOV      := $(if $(and $(HAVE_ELF),$(HAVE_POOL),$(HAVE_CAP),$(HAVE_SEAT),$(HAVE_SYSCALL)),$(wildcard $(KMOD_DIR)/governance.cpp))
# 内建组件管理器依赖 governance + elf_loader + topology
HAVE_BUILTIN := $(if $(and $(HAVE_GOV),$(HAVE_ELF),$(HAVE_TOPO)),$(wildcard $(KMOD_DIR)/builtin.cpp))

CORE_FOUND    := $(HAVE_ELF) $(HAVE_TOPO) $(HAVE_CAP) $(HAVE_GOV) $(HAVE_SYSCALL) \
                     $(HAVE_SEAT) $(HAVE_KEXP) $(HAVE_SERVICES) $(HAVE_POOL) $(HAVE_BUILTIN)
ALL_CORE_NAMES := elf_loader.cpp topology.cpp capability.cpp governance.cpp \
                        syscall_registry.cpp seat.cpp kernel_exports.cpp kmod_pool.cpp \
                        builtin.cpp kernel_services.cpp
PRESENT_CORE  := $(wildcard $(addprefix $(KMOD_DIR)/,$(ALL_CORE_NAMES)))
CORE_MISSING  := $(notdir $(filter-out $(PRESENT_CORE),$(addprefix $(KMOD_DIR)/,$(ALL_CORE_NAMES))))
CORE_EXCLUDED := $(notdir $(filter-out $(CORE_FOUND),$(PRESENT_CORE)))

HOST_SRC     := host_platform.cpp
BIN_DIR      := ../bin
FIX_DIR      := $(BIN_DIR)/kmod_fixtures
FIX_SRCS     := $(wildcard fixtures/*.cpp)
FIX_OBJS     := $(patsubst fixtures/%.cpp,$(FIX_DIR)/%.o,$(FIX_SRCS))

# 测试文件按"依赖的实现文件是否存在"过滤；gov/syscall 的测试由其他任务交付，
# 缺失时 test_main 打印 SKIP（不算失败）。
KNOWN_TESTS := topology_test.cpp capability_test.cpp elf_loader_test.cpp \
                    gov_test.cpp syscall_test.cpp component_test.cpp
TEST_SRCS    := test_main.cpp
TEST_SKIP    :=

ifneq ($(strip $(HAVE_TOPO)),)
ifneq ($(wildcard topology_test.cpp),)
TEST_SRCS += topology_test.cpp
else
TEST_SKIP += topology_test.cpp(file missing)
endif
else
TEST_SKIP += topology_test.cpp(topology.cpp)
endif

ifneq ($(strip $(HAVE_CAP)),)
ifneq ($(wildcard capability_test.cpp),)
TEST_SRCS += capability_test.cpp
else
TEST_SKIP += capability_test.cpp(file missing)
endif
else
TEST_SKIP += capability_test.cpp(capability.cpp)
endif

ifneq ($(strip $(HAVE_ELF)),)
ifneq ($(wildcard elf_loader_test.cpp),)
TEST_SRCS += elf_loader_test.cpp
else
TEST_SKIP += elf_loader_test.cpp(file missing)
endif
else
TEST_SKIP += elf_loader_test.cpp(elf_loader.cpp/topology.cpp)
endif

ifneq ($(strip $(HAVE_GOV)),)
ifneq ($(wildcard gov_test.cpp),)
TEST_SRCS += gov_test.cpp
else
TEST_SKIP += gov_test.cpp(not delivered)
endif
else
TEST_SKIP += gov_test.cpp(governance.cpp deps)
endif

ifneq ($(strip $(HAVE_SYSCALL)),)
ifneq ($(strip $(HAVE_SEAT)),)
ifneq ($(wildcard syscall_test.cpp),)
TEST_SRCS += syscall_test.cpp
else
TEST_SKIP += syscall_test.cpp(not delivered)
endif
else
TEST_SKIP += syscall_test.cpp(seat.cpp)
endif
else
TEST_SKIP += syscall_test.cpp(syscall_registry.cpp)
endif

ifneq ($(strip $(HAVE_BUILTIN)),)
ifneq ($(wildcard component_test.cpp),)
TEST_SRCS += component_test.cpp
else
TEST_SKIP += component_test.cpp(file missing)
endif
else
TEST_SKIP += component_test.cpp(builtin.cpp deps)
endif

# 其他任务/后续新增的 *_test.cpp 自动纳入（不写 main，提供 test_<name>_run）
EXTRA_TESTS := $(filter-out test_main.cpp $(KNOWN_TESTS),$(wildcard *_test.cpp))
TEST_SRCS    += $(EXTRA_TESTS)

$(info [kmod] core sources compiled: $(words $(CORE_FOUND))/10 ($(notdir $(CORE_FOUND))))
ifneq ($(strip $(CORE_EXCLUDED)),)
$(info [kmod] present but waiting deps (not compiled): $(CORE_EXCLUDED))
endif
ifneq ($(strip $(CORE_MISSING)),)
$(info [kmod] files not landed yet: $(CORE_MISSING))
endif
$(info [kmod] tests: $(TEST_SRCS))
ifneq ($(strip $(TEST_SKIP)),)
$(info [kmod] SKIP tests: $(TEST_SKIP))
endif

# ----------------------------------------------------------------------------
# flags
# ----------------------------------------------------------------------------
CXXFLAGS_COMMON := -std=gnu++17 -O2 -g -Wall -Wextra -Wno-unused-parameter \
                         -Wno-unused-function -pthread -D__KERNEL_TEST_HOST__ \
                         -I ../common -I ../../kernel/include -I . \
                         -DKMOD_FIXTURE_DIR='"$(abspath $(FIX_DIR))"'
CXXFLAGS_ASAN    := -std=gnu++17 -O1 -g -Wall -Wextra -Wno-unused-parameter \
                         -Wno-unused-function -pthread -D__KERNEL_TEST_HOST__ \
                         -fsanitize=address,undefined -fno-omit-frame-pointer \
                         -I ../common -I ../../kernel/include -I . \
                         -DKMOD_FIXTURE_DIR='"$(abspath $(FIX_DIR))"'
LDLIBS             := -pthread

# fixture 编译：模块作者视角（只依赖 kmod_module.h/kmod_abi.h），
# -fno-pic -fno-pie 产出普通 ET_REL；-mcmodel=large 让外部符号引用用
# R_X86_64_64 绝对重定位（宿主 malloc/mmap 地址 >4GiB，小模型会生成 32 位
# 重定位被加载器按设计拒绝）。-fno-stack-protector 避免 __stack_chk_fail。
# 这里刻意**不加** -Wa,-mx86-used-note=no：Ubuntu g++/as 默认会生成 SHF_ALLOC
# 的 .note.gnu.property（CET/IBT 属性），加载器已显式跳过 SHT_NOTE 段，
# 正好用真实工具链输出覆盖该路径（见 elf_loader.cpp）。
FIX_CXXFLAGS := -std=gnu++17 -O2 -g -mcmodel=large -fno-pic -fno-pie \
                     -fno-stack-protector -fno-exceptions -fno-rtti \
                     -fno-asynchronous-unwind-tables -I ../../kernel/include

# ----------------------------------------------------------------------------
# 目标
# ----------------------------------------------------------------------------
.PHONY: kmod kmod-asan kmod-status clean FORCE

kmod: kmod-status $(BIN_DIR)/kmod_test $(FIX_OBJS)

kmod-asan: kmod-status $(BIN_DIR)/kmod_test_asan $(FIX_OBJS)

# 每次构建都打印源码就绪情况（-B 下也可见）
kmod-status: FORCE
	@echo "[kmod] ===== source status ====="
	@echo "[kmod] kernel core $(words $(CORE_FOUND))/10: $(notdir $(CORE_FOUND))"
	@if [ -n "$(strip $(CORE_EXCLUDED))" ]; then \
	    echo "[kmod] present but waiting deps (not compiled): $(CORE_EXCLUDED)"; fi
	@if [ -n "$(strip $(CORE_MISSING))" ]; then \
	    echo "[kmod] files not landed yet: $(CORE_MISSING)"; fi
	@echo "[kmod] tests: $(TEST_SRCS)"
	@if [ -n "$(strip $(TEST_SKIP))" ]; then \
	    echo "[kmod] SKIP tests: $(TEST_SKIP)"; fi

FORCE:

$(BIN_DIR)/kmod_test: $(TEST_SRCS) $(CORE_FOUND) $(HOST_SRC) host_platform.h test_common.h
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS_COMMON) $(TEST_SRCS) $(CORE_FOUND) $(HOST_SRC) -o $@ $(LDLIBS)
	@echo "[kmod] built $@"
	@echo "[kmod] run: cd tests && ./bin/kmod_test"

$(BIN_DIR)/kmod_test_asan: $(TEST_SRCS) $(CORE_FOUND) $(HOST_SRC) host_platform.h test_common.h
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS_ASAN) $(TEST_SRCS) $(CORE_FOUND) $(HOST_SRC) -o $@ $(LDLIBS)
	@echo "[kmod] built $@ (ASAN+UBSAN)"
	@echo "[kmod] run: cd tests && ./bin/kmod_test_asan"

$(FIX_DIR)/%.o: fixtures/%.cpp ../../kernel/include/kmod/kmod_module.h \
                     ../../kernel/include/kmod/kmod_abi.h
	@mkdir -p $(FIX_DIR)
	$(CXX) $(FIX_CXXFLAGS) -c $< -o $@
	@echo "[kmod] fixture $@"

clean:
	rm -rf $(FIX_DIR) $(BIN_DIR)/kmod_test $(BIN_DIR)/kmod_test_asan
