//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * elf_image.cpp —— 可靠读取 + 结构校验一份 ELF 镜像 (见 elf_image.h)
 * ----------------------------------------------------------------------------
 * 根因说明 (2026-11 偶发启动卡死):
 *   三个加载器 (sys_load / Schedule::NewThread / elf_dyn 的 DT_NEEDED 读取)
 *   都调用 MP->FSOPS->read() 后**直接使用缓冲区**。
 *   ext4_fread 的语义是"尽力读": 一旦某个块 I/O 返回错误 (AHCI 端口在并发
 *   命令下报 TFES 时就是这种情况), 它会 goto Finish 提前返回, 只填好前一段
 *   数据, 返回值非 0 / *rcnt 小于请求长度。调用方丢掉这两个信号之后,
 *   kmalloc 出来**未初始化**的剩余部分就被当成 ELF 头/程序头解析 ——
 *   `[dynlink] libsky.so: bad ELF header` 就是这么来的 (libsky.so 1,032,200 B,
 *   要读 2017 个扇区, 是启动期最大的单次读, 命中概率最高)。
 * ==========================================================================*/
#include <elf/elf_image.h>
#include <elf/elf.h>

#include <fs/fd.h>
#include <klib/errno.h>
#include <klib/kprintf.h>
#include <mem/heap.h>
#include <arch/x86_64/schedule/sched.h>

using Elf64::Elf64_Ehdr;
using Elf64::Elf64_Phdr;

/* 整文件重读次数: 瞬时块设备错误由 AHCI 驱动恢复端口后即可重试成功,
   4 次足以覆盖"连续两次撞上同一个坏时刻"的概率, 又不至于在硬盘真坏时
   把启动拖到几十秒。 */
#define KELF_READ_ATTEMPTS 4

/* 读失败后让出一次 CPU: 端口恢复需要一点时间, 而且当前调用者可能持有
   文件系统挂载锁 —— 直接忙等会在慢速/模拟核上白白烧掉一个时间片。
   早期引导 (调度器还没起来, this_thread()==0) 时不能 Yield。 */
static void kelf_backoff(void) {
    if (Schedule::this_thread())
        Schedule::Yield();
}

int kelf_validate_image(const char *tag, const uint8_t *buf, uint64_t len) {
    const char *name = tag ? tag : "<image>";

    if (!buf) {
        kerrorln("[elfimg] %s: null image", name);
        return -EINVAL;
    }
    if (len < sizeof(Elf64_Ehdr)) {
        kerrorln("[elfimg] %s: image too small (%llu < hdr %llu)",
                 name, (unsigned long long)len,
                 (unsigned long long)sizeof(Elf64_Ehdr));
        return -EINVAL;
    }

    const Elf64_Ehdr *eh = (const Elf64_Ehdr*)buf;
    if (eh->e_ident.c[0] != 0x7f || eh->e_ident.c[1] != 'E' ||
        eh->e_ident.c[2] != 'L' || eh->e_ident.c[3] != 'F') {
        kerrorln("[elfimg] %s: bad magic %02x %02x %02x %02x", name,
                 eh->e_ident.c[0], eh->e_ident.c[1],
                 eh->e_ident.c[2], eh->e_ident.c[3]);
        return -EINVAL;
    }
    if (eh->e_ident.c[Elf64::EI_CLASS] != Elf64::ELFCLASS64) {
        kerrorln("[elfimg] %s: not ELFCLASS64 (class=%u)", name,
                 (unsigned)eh->e_ident.c[Elf64::EI_CLASS]);
        return -EINVAL;
    }
    if (eh->e_machine != 62 /* EM_X86_64 */) {
        kerrorln("[elfimg] %s: wrong machine %u", name, (unsigned)eh->e_machine);
        return -EINVAL;
    }
    if (eh->e_type != Elf64::ET_EXEC && eh->e_type != Elf64::ET_DYN) {
        kerrorln("[elfimg] %s: unsupported e_type %u", name, (unsigned)eh->e_type);
        return -EINVAL;
    }
    if (eh->e_phnum == 0 || eh->e_phnum == 0xFFFF) {
        kerrorln("[elfimg] %s: bad e_phnum %u", name, (unsigned)eh->e_phnum);
        return -EINVAL;
    }
    /* 用减法形式避免 phoff + phnum*entsize 回绕。
       e_phentsize 必须 >= sizeof(Elf64_Phdr): 小于它时按结构体跨步遍历会
       越过表尾读相邻数据 (畸形/被截断的镜像能借此把任意内核字节当程序头)。 */
    const uint64_t ph_ent = eh->e_phentsize ? (uint64_t)eh->e_phentsize
                                            : (uint64_t)sizeof(Elf64_Phdr);
    if (eh->e_phentsize != 0 && eh->e_phentsize < sizeof(Elf64_Phdr)) {
        kerrorln("[elfimg] %s: bad e_phentsize %u (< %llu)", name,
                 (unsigned)eh->e_phentsize,
                 (unsigned long long)sizeof(Elf64_Phdr));
        return -EINVAL;
    }
    if (eh->e_phoff > len || ph_ent > len - eh->e_phoff ||
        (uint64_t)eh->e_phnum > (len - eh->e_phoff) / ph_ent) {
        kerrorln("[elfimg] %s: phdr table out of bounds (phoff=%llu phnum=%u len=%llu)",
                 name, (unsigned long long)eh->e_phoff, (unsigned)eh->e_phnum,
                 (unsigned long long)len);
        return -EINVAL;
    }

    uint32_t loads = 0;
    for (uint32_t i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph =
            (const Elf64_Phdr*)(buf + eh->e_phoff + (uint64_t)i * ph_ent);
        if (ph->p_type != Elf64::PT_LOAD)
            continue;
        loads++;
        /* 段数据必须整段落在这个文件缓冲里, 否则 elf_load_impl 的 memcpy 会
           读到相邻内核对象 (或被钳位后静默少拷代码, 运行期随机 #PF)。 */
        if (ph->p_offset > len || ph->p_filesz > len - ph->p_offset) {
            kerrorln("[elfimg] %s: PT_LOAD[%u] data out of bounds "
                     "(off=%llu filesz=%llu len=%llu)",
                     name, i, (unsigned long long)ph->p_offset,
                     (unsigned long long)ph->p_filesz,
                     (unsigned long long)len);
            return -EINVAL;
        }
        if (ph->p_filesz > ph->p_memsz) {
            kerrorln("[elfimg] %s: PT_LOAD[%u] filesz>memsz (%llu>%llu)",
                     name, i, (unsigned long long)ph->p_filesz,
                     (unsigned long long)ph->p_memsz);
            return -EINVAL;
        }
        /* 映射终点必须在用户半区且不回绕 (与 exec.cpp 的 USER_ADDR_LIMIT
           一致: 4 级分页的用户地址上限 0x0000800000000000)。
           放在这里是为了让"镜像自洽性"检查单点成立, 而不是散落在加载器里。 */
        const uint64_t end = ph->p_vaddr + ph->p_memsz;
        if (end < ph->p_vaddr || end >= 0x0000800000000000ULL) {
            kerrorln("[elfimg] %s: PT_LOAD[%u] vaddr range illegal "
                     "(vaddr=0x%llx memsz=0x%llx)",
                     name, i, (unsigned long long)ph->p_vaddr,
                     (unsigned long long)ph->p_memsz);
            return -EINVAL;
        }
    }
    if (loads == 0) {
        kerrorln("[elfimg] %s: no PT_LOAD segment", name);
        return -EINVAL;
    }
    return 0;
}

int kelf_read_file(const char *path, uint8_t **out_buf, uint64_t *out_len) {
    if (!path || !out_buf || !out_len)
        return -EINVAL;
    *out_buf = nullptr;
    *out_len = 0;

    __hmap_s_mp *MP = GetMount(path);
    if (!MP || !MP->FSOPS || !MP->FSOPS->read) {
        kerrorln("[elfimg] no mount for '%s'", path);
        return -ENOENT;
    }
    if (MP->FSOPS->SIZEOF_FILE_DESC == 0) {
        kerrorln("[elfimg] '%s': filesystem has no file descriptor", path);
        return -EINVAL;
    }

    for (int attempt = 1; attempt <= KELF_READ_ATTEMPTS; attempt++) {
        void *fd = kmalloc(MP->FSOPS->SIZEOF_FILE_DESC);
        if (!fd)
            return -ENOMEM;
        _memset(fd, 0, MP->FSOPS->SIZEOF_FILE_DESC);

        /* open 失败是确定性错误 (路径/权限), 重试没有意义。 */
        if (MP->FSOPS->open(fd, path, O_RDONLY) != 0) {
            kfree(fd);
            kerrorln("[elfimg] open failed: '%s'", path);
            return -EACCES;
        }

        const uint64_t want = MP->FSOPS->fsize(fd);
        if (want < sizeof(Elf64_Ehdr)) {
            MP->FSOPS->close(fd);
            kfree(fd);
            kerrorln("[elfimg] '%s' too small: %llu bytes", path,
                     (unsigned long long)want);
            return -EINVAL;
        }

        uint8_t *buf = (uint8_t*)kmalloc(want);
        if (!buf) {
            MP->FSOPS->close(fd);
            kfree(fd);
            return -ENOMEM;
        }
        /* 先清零: 驱动短读时, 未写入的区域保持确定值 (全 0), 即使上层漏检
           也不会被解析成"看起来合法"的 ELF 结构。 */
        _memset(buf, 0, want);

        size_t got = 0;
        const int32_t rc = MP->FSOPS->read(fd, buf, (size_t)want, &got);
        MP->FSOPS->close(fd);
        kfree(fd);

        if (rc == 0 && (uint64_t)got == want) {
            *out_buf = buf;
            *out_len = want;
            return 0;
        }

        /* 短读 / I/O 错误: 驱动层已经做过端口级恢复 (RecoverPort), 这里整文件
           重读一次通常就能拿到完整镜像。日志必须带全 现场, 否则下次再出现又
           只能靠猜。 */
        kerrorln("[elfimg] short read '%s' attempt %d/%d: rc=%d got=%llu want=%llu",
                 path, attempt, KELF_READ_ATTEMPTS, (int)rc,
                 (unsigned long long)got, (unsigned long long)want);
        kfree(buf);
        kelf_backoff();
    }

    kerrorln("[elfimg] '%s': giving up after %d attempts (I/O error)",
             path, KELF_READ_ATTEMPTS);
    return -EIO;
}
