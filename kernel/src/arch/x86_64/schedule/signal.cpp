#include <arch/x86_64/allin.h>
#include <elf/elf.h>
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/interrupt/idt.h>
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/schedule/syscall.h>
#include <arch/x86_64/vmm/vmm.h>
#include <arch/x86_64/simd/simd.h>
#include <klib/algorithm/queue.h>

namespace Schedule{
    namespace Signal{
        int32_t Raise(proc_t *process, int32_t signal){
            // Check if the signal is valid
            if (signal < 1 || signal > 64) {
                return -EINVAL;
            }
            sigaction_t action = process->sig_handlers[signal];
            if(action.handler != nullptr){
                action.handler(signal);
            }else{
                DefaultHandler(signal);
            }
            return 0;
        }

        void DefaultHandler(int32_t signal){
            switch(signal){
                case LINUX_SIGTERM:
                case LINUX_SIGKILL:
                    Schedule::Exit(0);
                    break;
                case LINUX_SIGCHLD:
                    // Ignore
                    break;
                default:
                    // For other signals, just ignore for now
                    break;
            }
        }
    }

}