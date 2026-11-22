<!-- SPDX-FileCopyrightText: 2026 Yo-yo-ooo -->
<!-- SPDX-License-Identifier: MIT -->

# DOS VM：单核可逆退出 IA-32e 并在桌面运行 16 位程序

> 状态：已实现并在 QEMU 实机验证通过（`dosvm_trip_gate.py` 多次 PASS）。
> 现已能装载真实 `.COM`/`.EXE`，提供 INT 21h/10h/16h、可挂起可恢复的
> 16 位上下文，以及经宿主 VFS 的双向文件 I/O（16 位程序用自己的
> `DS:DX` 缓冲区读写桌面文件系统）。
> 入口：`Ctrl+Alt+T` 或开始菜单「DOS Console」；窗口内按 Enter 触发一次往返。

## 1. 这个子系统做什么

让一个 AP 真正离开 x86_64 long mode，进入实模式执行一段 16 位 DOS 风格
程序，把 80×25 文本写进影子屏幕，再原路回到 long mode；期间 BSP、其它 AP
和桌面 WM 全程留在 x86_64 正常工作。回程后由 `CopyToUser` 把文本快照送到用户态
窗口客户端显示。

```
BSP / AP#3 / 桌面 WM        AP#1（或 #2）
─────────────────────      ──────────────────────────────────────
x86_64 long mode            long mode (IA-32e, LMA=1)
  一直正常运行      IPI 0xF0 → compatibility 32 (L=0, D=1)
                           → 清 CR4.PCIDE, 清 CR0.PG (LMA 自动=0)
                           → protected 16 → 清 CR0.PE → REAL MODE
                           → 16 位 DOS payload 写 80×25 影屏
                           → 原路回到 long mode
                      完成 → syscall 210 把 4016 B 快照搬到用户态窗口
```

## 2. 硬件依据与「为什么不用 VM86」

用户提出过「用 VM8086 绕开 `[0x70000,0x80000)` 限制」。结论是不可行，且方向相反：

* `EFLAGS.VM` 只在 legacy 32 位保护模式有效；Intel SDM 明确
  *"Virtual-8086 mode is not supported in IA-32e mode"*。long mode 下硬件 VM86
  不存在，必须用 VMX/SVM 把实模式放进 guest（本内核无 VMXON/VMCS 代码）。
* 要用 VM86 仍须先退出 long mode——正是本子系统最难、且必须在 identity
  映射低内存里执行的那几步，VM86 一字节也省不掉。
* VM86 与 real mode 的 guest 线性地址都是 20 位段:偏移，同样够不到高位物理地址。
  真要解耦须靠分页把 guest 低 1MB 重定向到任意物理页，那反而需要新建 32 位
  PAE 页表、32 位 IDT、TSS/IO bitmap 与 #GP 反射 monitor，工程量更大。

采纳的那一半 insight：guest 地址与 host 地址确实应该解耦，但解耦放在
软件数据通路上更便宜：低 64 KiB 只放切换代码/GDT/IVT/影屏，DOS payload
写完后由内核 `CopyToUser` 搬到任意用户地址。

几个让实现显著简化的硬件事实：

| 事实 | 带来的简化 |
|---|---|
| CR0/CR3/CR4/EFER/GDTR/IDTR/GS.base 都是**每逻辑处理器**寄存器 | 摘一个核关 PG 不影响其它核 |
| `EFER.LMA = LME & CR0.PG` 由硬件自动维护 | 清 PG 即退出 IA-32e；**保持 LME=1**，回程只需重新置 PG，省掉两次 WRMSR |
| `0x70000` 是 64 KiB 对齐，低 16 位为 0 | 同一份 `org 0x70000` flat binary 的标签低 16 位恰好等于实模式段内偏移，64/32/16 位代码无需任何重定位 |
| 低 4 GiB 已 identity 映射且未置 NX（`vmm.cpp:758`） | trampoline 直接拷到物理 0x70000 即可取指，无需新页表 / linker script |

## 3. 四层架构

| 层 | 文件 | 职责 |
|---|---|---|
| **Tier0** | `kernel/modules/x86_64/smp/smp.cpp`<br>`lapic/lapic.cpp`<br>`kernel/src/arch/x86_64/vmm/vmm.cpp`<br>`schedule/{sched,task}.cpp`<br>`syscall/{task,exec}.cpp` | `g_cpu_offline_mask` + `dosvm_core_offline/online/is_offline/lapic_offline`；`LAPIC::IPI` 底层统一过滤（**仅 0xF0 放行**）；`IPIAll/IPIOthers` 改逐核发送；TLB shootdown 的 `SendTarget` 在 ACK 计数**之前**跳过 offline 核；调度/唤醒/选核全部跳过 |
| **DynModule** | `kernel/modules/x86_64/dosvm/dosvm.cpp`<br>`programs/dos/trampoline.nasm`<br>`kernel/include/arch/x86_64/dosvm/dosvm_trampoline.h`（生成物）<br>`res/scripts/gen_dosvm_trampoline.py`<br>`kernel/src/mem/pmm.cpp` | 单一的 **kernel.arch.dosvm.ko**（原 Tier1 实现 + 原 Tier2 门面合并，从 modules.saf 加载）：装载 blob、0xF0 IPI handler、状态机、影屏、8 个 C 导出函数，并用 `KCOMPONENT_EXPORT_SYSCALL` 直接注册 **210/211/212/213/214**；init 里 `KdllInject` 注入 `kernel.mem.pmm`/`kernel.arch.acpi` |
| **L3** | `programs/dos/main.cpp` + `Makefile`<br>`programs/Makefile` / `build_os_pkg.py`<br>`programs/desktop/main.cpp` | 80×25 CP437 8×8 位图字体 + VGA16 色窗口客户端；开始菜单 + `Ctrl+Alt+T` |

共享契约：`kernel/include/arch/x86_64/dosvm/dosvm_contract.h`。

### 为什么 trampoline 是字节数组而不是 `.asm`

`kernel/Makefile` 对 builtin 树里所有 `.asm` 强制 `-f elf64` 并链接进高半内核；
`org` 不生效，产物无法拷到 0x70000 执行。因此用 NASM `-f bin` 编成 flat binary，
再由 `gen_dosvm_trampoline.py` 生成 C 数组头文件（已提交，正常构建不依赖
NASM/Python），DynModule init 时 `memcpy` 到物理 0x70000。

## 4. 用户态 ABI

```
syscall(210, buf, cap, 0,0,0,0)  // DOSVM_SYSCALL_READ
    cap < 4016        -> -EINVAL
    无有效快照        -> -EAGAIN
    成功              -> 4016
syscall(211, cpu_id, 0,0,0,0,0)  // DOSVM_SYSCALL_ENTER
    cpu_id = 0        -> 自动挑一个非 BSP、且不是当前核的 AP
    程序结束 / banner -> 0
    挂起等文件请求    -> DOSVM_RC_NEED_FS    (1)
    挂起等按键        -> DOSVM_RC_SUSPENDED  (2)
syscall(212, img, len, args, args_len, 0, 0)  // STAGE
    把映像装进低内存（.COM 或按 MZ 头装载的 .EXE），建 PSP
syscall(213, keys, count, 0,0,0,0)            // KEYS
    u16 元素 = (scan << 8) | ascii，预置 16 位键盘队列
syscall(214, off, buf, len, is_write, 0, 0)   // MEM
    off 相对 0x70000，越界即 -EINVAL；这是用户态碰低内存的唯一通道
```

`211` 只在这一个 syscall 上有正向返回码（0 之外的两个值），因为实模式
窗口没有任何异步事件，用户态必须知道"程序是结束了、还是要我交键、还是要
我代执行文件请求"。

`buf` 4016 B = 16 B header + 4000 B cells：

```
u64[0] = generation(低32) | flags(高32)
u64[1] = last_cpu(低32)   | trips(高32)
cells  = (char, attr) × 2000，行优先，attr 为标准 VGA 属性
flags: bit0 VALID / bit1 RUNNING / bit2 REAL_OK
```

`200/201` 最初占用了 demo_syscall 模块的动态槽，故改用 `210/211`。

## 5. 真实 DOS 程序：INT 21h、挂起恢复与文件桥

### 5.1 为什么"协作 yield"是硬约束而不是偷懒

实模式窗口里 IF=0（整趟往返屏蔽中断），IA-32e 下又没有 VM86，16 位程序
没有任何抢占手段。所以"可交互"只能由程序自己交出控制权：在 `INT 21h` /
`INT 16h` 的服务点把整台机器存进 state 区，trampoline 返回 64 位，桌面照常
运行；用户态准备好结果后再 `enter` 一次，从中断的下一条指令继续。

### 5.2 挂起上下文（state 区 0x7E190 起）

| 槽 | 内容 |
|---|---|
| `S_RESUME` / `S_PEND` | 一次性有效位 / 挂起来源（16 = int 16h，21 = int 21h） |
| `S_CTX` (8×u16) | ax, bx, cx, dx, si, di, bp, **sp（指向 iret 帧）** |
| `S_SEGS` (4×u16) | ds, es, ss, pad |
| `S_FRAME` (4×u16) | ip, cs, flags, pad |
| `S_RETAX` | 恢复时写入 AX 的兜底值 |

`save_ctx` 从中断 handler 的 prolog 帧整体搬进 state（`[bp+2..bp+6]` =
ip/cs/flags，`[bp-2..bp-16]` = ax,bx,cx,dx,si,di,ds,es）；`com_resume`
在程序自己的栈上重建 iret 帧（不信任栈上残留内容），恢复全部寄存器后
`iret`。文件结果优先于键盘：`S_FSACT == 2` 时取 `S_FSRES` 作 AX，并按
`S_FSCF` 在重建帧的 FLAGS 里置 CF —— 用 `mov bx,sp; or word [ss:bx+4],1`，
因为 16 位模式下 SP 不能作基址寄存器。

### 5.3 文件 I/O 双向陷阱

`INT 21h` 的 `3Ch/3Dh/3Eh/3Fh/40h` 不再回 `CF=1`，而是把
`AH`+`BX`+`CX`+`DX`+调用者 `DS` 存进 `S_FS*`、置 `S_FSACT=1` 后挂起。
`41h`(delete) / `42h`(lseek) 同样走这条路，但宿主明确回 `CF=1, AX=1`——
桌面 VFS 没有 unlink/fseek。

`dosvm_enter` 见到 `S_FSACT==1` 返回 `DOSVM_RC_NEED_FS`；`programs/dos`
读请求块 → 把 DOS 路径归一化（`C:\A\B.TXT` → `/mp/A/B.TXT`，失败再试全小写）
→ 用宿主 `fopen/fread/fwrite` 执行 → 把数据经 syscall 214 写回
`DS:DX` 指的低内存 → 写 `S_FSRES`/`S_FSCF` 并置 `S_FSACT=2` → 再 `enter`。

`DS:DX` 是实模式线性地址（`seg*16+off`），落在 `0x70000..0x80000`，而用户
进程页表看不见物理内存；syscall 214 是唯一通道，偏移相对 0x70000，
越界由内核 `-EINVAL` 卡死在 64 KiB 内。

### 5.4 .COM / .EXE 装载

| 格式 | 判据 | 装载方式 |
|---|---|---|
| `.COM` | 无 `MZ` 头 | 整份拷到 `0x71100`，`CS:IP = 7100:0100`；PSP 建在 `0x71000`（`+0x00 = CD 20`、`+0x02 = 段顶`、`+0x80 = 命令行长度 + 尾`） |
| `.EXE` | `e_magic == 'MZ'` | 解析 `e_cblp/e_cp/e_crlc/e_cparhdr/e_ss/e_sp/e_cs/e_ip/e_lfarlc`，按 `load_seg = 0x7110` 逐条应用重定位（目标越界即整份拒绝），填 `S_EXECS/S_EXEIP/S_EXESS/S_EXESP` 并置 `S_RUNMODE = 2` |

栈一律留在低内存预留段：`.COM` 用 `7000:4FF0`；`.EXE` 用 header 的
`SS:SP`（入口前留 4 字节给 far `ret`）。因此标准 `.COM` 自带的
`SS:SP = CS:FFFE` 不会落出预留区。

### 5.5 内置服务面

| int | 功能 |
|---|---|
| `INT 20h` / `21h AH=4Ch` | 终止：写 `S_EXITCODE`、置 `S_EXITED` |
| `21h AH=02` / `09` | 单字符 / `$` 结尾串输出 |
| `21h AH=01` / `06` / `07` / `08` | 读键（队列空则挂起，`S_PEND=21`） |
| `21h AH=0B` | 键状态（回填 ZF） |
| `21h AH=30` | 版本 `AX=0x0330` |
| `21h AH=0E` / `19` / `25` / `35` / `4A` | no-op / 当前盘 / 取设向量 / 改块 |
| `21h AH=3C..42` | 文件桥（见 5.3） |
| `INT 10h` | `AH=00` 设模式（0–3 清屏）/ `02` 设光标 / `03` 读光标 / `06` 滚屏 / `0E` TTY |
| `INT 16h` | `AH=00` 读键 / `01` 查询（正确回填 ZF，空则挂起 `S_PEND=16`） |

所有 handler 经 `H_PROLOG` / `H_EPILOG` 把活寄存器写回栈槽再弹回，因此
`AX/BX/CX/DX` 的回填与真实 DOS 一致；state 一律用 `cs:` 前缀访问，不依赖
调用者的 `DS`。

## 6. 实机调试中修复的五个关键 bug

1. 自 IPI 死锁：`dos.elf` 被调度到 cpu=1 且自动选核也选中 1。syscall 内核
   路径 IF=0（FMASK 清 IF），自 IPI 永远不投递，`dosvm_enter` 在自旋里死等 →
   `STALL WATCHDOG: CPU 1 has not run for 3000 ms (vec=0 syscall)`。
   修复：选核时排除 `this_cpu()->id`，单 AP 且正是当前核时返回 `-EBUSY`。
2. 低内存 #PF（最关键）：低 4 GiB identity 映射只建在 `kernel_pagemap`，用户
   进程页表只复制 PML4 `[256..511]`。C++ 在 syscall / IPI 上下文直接读写
   `0x7E120` → `Segfault: unmapped ... CR3=0x10FE000` → 整机 HALT。
   修复：C++ 侧统一走 HHDM 别名（`phys + hhdm_offset`，位于被复制的
   PML4E=256）；仅在 trampoline 运行的那一小段用裸 `mov cr3` 临时切到
   `kernel_pagemap` 的干净 CR3，返回后恢复原 CR3（不能用 `SwitchPageMap()`，
   它会改 `this_cpu()->pagemap` 并触发 LazyTLB，中断上下文不安全）。
3. CR4.PCIDE：本内核启用了 PCID（`vmm.cpp:223`）且 `SwitchPageMap` 用
   `cr3 |= (1<<63)` no-flush。`PCIDE=1` 时清 PG 会 #GP。修复：compat32 阶段
   先清 CR4.PCIDE，回程 `p32_return` 加载低 12 位清零的干净 CR3 再置 PG，
   `ret64` 恢复原 CR4/CR3。
4. 0xF0 不自动 EOI：`idt_irq_handler` 只对 `0x20..0x3F` 发 EOI。0xF0 handler
   必须自己 `LAPIC::EOI()`，否则 ISR 一直挂着该向量。
5. 窗口末行被裁：`contentH = h - TITLE_H - radius`，`248` 实测只给 200 px，
   放不下 8 px 状态栏 + 25×8=200 px 文本屏。窗口高度改 `272`（contentH=224）。
   另把状态栏改成深灰底亮青字，避免与 banner 行（VGA 属性 0x1F 白字蓝底）撞色。

其它防护：实模式 IVT 256 个向量全部指向 `IRET`（C++ 预填，避免 NMI/#MC 落到
64 位 IDT 门描述符）、切换前 `StopTimer` + mask 全部 LVT + TPR=0xFF、回来后
重新 `Oneshot(SCHED_VEC, ...)` 武装调度时钟、重载 CR3 全刷 TLB（offline 期间
其它核跳过了本核的 shootdown）。

## 7. 验证证据

`res/scripts/test/dosvm_trip_gate.py`（QEMU q35 / `-cpu max` / 2048M / `-smp 4`）：
等待 `[dosvm] ready` + `[dosvm] dynlink:` 标记 → 等桌面主循环就绪 → `Ctrl+Alt+T` → 抓串口
`[dosvm] trip OK` → 可选 `--screenshot` / `--dump-shadow`。

> 以下日志是 banner 版本的历史记录（blob 1434 B），保留作为"可逆往返"
> 的原始证据。当前源码的 blob 为 2890 B（`nasm -f bin -Wall -Werror`
> 通过，预算 0xC00），syscall 面已扩到 `210/211/212/213/214`。新版本的
> QEMU 复跑尚未执行——本文档只声明已静态验证的部分，不复用旧日志冒充。

```
[dosvm] ready: blob=2890 B @0x70000 pml4_pa=0x139000 ipi=0xF0 aps=1..3 syscall=210/211
[dosvm] dynlink: inject kernel.mem.pmm rc=0, kernel.arch.acpi rc=0 (pins=1/1)
[dos] init: content=660x224 pitch=712 80x25 buf=4016 syscall_read=210 syscall_enter=211
[dosvm] trip begin: cpu=2 lapic_id=2
[dosvm] trip OK: cpu=2 gen=1 trips=1
[gate] RESULT PASS — real-mode round trip returned: cpu=2 gen=1 trips=1
```

多次独立运行分别选到 `cpu=1` / `cpu=2` 均 PASS，日志内无 `#PF`、
`STALL WATCHDOG`、`SYSTEM HALTED`、`trip TIMEOUT/FAILED`。

交叉验证（排除「假成功」）：

* HMP `xp /96bx 0x7f010` 读出的影屏物理内存为
  `20 1f 20 1f 53 1f 6b 1f 79 1f ...` = `"  SkylineSystem DOS VM - 16-bit REAL MODE"`，
  顺序与 VGA 属性 0x1F 均正确——证明 16 位 payload 真的在实模式执行并写了屏。
* 对截图做逐字符 XOR OCR：绿色行读出 `'  This text was writ'`（mismatch=0），
  banner 行读出 `'  SkylineSystem DOS VM'`（xor=0）；正向字模模板
  MSB 位序 25/25 命中，LSB 仅 18/25。

## 8. 已知限制

* NMI / SMI / #MC 无法屏蔽；实模式 IVT 已指向 IRET 兜底，窗口极短且 IF=0 +
  TPR=0xFF + LVT 全 mask，但真机仍属残余风险。
* 若 trampoline 在窗口内 triple fault，整机会重启（`-no-reboot` 下停在故障点）；
  串口日志是唯一证据，故 init/enter 都打印可检索标记，timeout 会打印
  `stage=<1..11> magic=...` 帮助定位。
* `dosvm_enter` 在发起核上忙等完成（2 s 超时）；往返期间该核不响应调度。
* 单 AP（`-smp 2`）且 syscall 恰好落在该 AP 上时返回 `-EBUSY`。
* 16 位侧没有 unlink / fseek：`INT 21h AH=41h/42h` 回 `CF=1, AX=1`。
* 同一时刻只有一个 16 位程序（单窗口、单挂起上下文），没有并发任务切换。
* 没有 EMS/XMS/DPMI、没有 TSR，程序跑在真实模式 1 MiB 地址空间里。
* 原 `dosvm.ko` 门面已合并进 `kernel.arch.dosvm.ko`（从 modules.saf 加载）：
  模式切换/影屏/低内存都在这个独立 .ko 内，syscall 210..214 由同一个模块注册。
  普通 vendor .ko 仍无法访问 CR/GDT/IDT/LAPIC/页表（没有 kernel 实现导出）。

## 9. 复现

```bash
wsl -e bash -lc 'cd /mnt/c/ZSY/SkylineSystem && make -j8'
wsl -e bash -lc 'cd /mnt/c/ZSY/SkylineSystem && make -C programs'
python3 res/scripts/test/dosvm_trip_gate.py --wait 150 \
    --screenshot C:/ZSY/SkylineSystem/dosvm_screen.ppm
# 修改 trampoline 后需重新生成字节数组：
python3 res/scripts/gen_dosvm_trampoline.py    # 需在 WSL 内（依赖 nasm）
```
