// SPDX-License-Identifier: MIT
// File: allocator.c
// Copyright (C) 2026 Yo-yo-ooo
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
.intel_syntax noprefix
.global _start
.extern main
.extern _init_runtime_and_global_variables
.type _start,@function
.section .text
_start:
    # ----------------------------------------------------
    # 1. 阶段一：从内核摆好的 RSP 中提取 argc 和 argv
    # ----------------------------------------------------
   # mov rdi, [rsp]        # RDI = argc
   # lea rsi, [rsp + 8]    # RSI = argv

    # 16字节栈对齐
   # and rsp, 0xFFFFFFFFFFFFFFF0

    # 把参数暂时保存到安全的地方（比如压入对齐后的栈），防止接下来的初始化冲爆寄存器
    #push rsi
    #push rdi

    # ----------------------------------------------------
    # 2. 阶段二：由 init.S 负责 Lib 库和全局变量的初始化
    # ----------------------------------------------------
    # 调用一个 C 语言实现的运行时初始化函数（见下文）
    call _init_runtime_and_global_variables

    # ----------------------------------------------------
    # 3. 阶段三：执行用户的 main 函数
    # ----------------------------------------------------
    pop rdi               # 恢复之前保存的 argc
    pop rsi               # 恢复之前保存的 argv
    call main             # 正式进入用户的业务逻辑

    # ----------------------------------------------------
    # 4. 阶段四：安全退出
    # ----------------------------------------------------
    mov rdi, rax          # RAX 里是 main 的返回值
    mov rax, 9            
    syscall                 # 陷入内核，安乐死