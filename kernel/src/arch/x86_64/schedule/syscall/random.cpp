#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/syscall.h>
#include <klib/errno.h>
#include <elf/elf.h>
#include <mem/pmm.h>
#include <klib/algorithm/queue.h>
#include <klib/rnd.h>
#include <klib/kio.h>
#include <arch/x86_64/pit/pit.h>

#define GRND_NONBLOCK	0x0001
#define GRND_RANDOM	0x0002
#define GRND_INSECURE	0x0004

uint64_t sys_getrandom(uint64_t buf, uint64_t size, uint64_t flags,
    uint64_t IGN_0,uint64_t IGN_1,uint64_t IGN_3)
{
    IGNORE_VALUE(IGN_0);IGNORE_VALUE(IGN_1);IGNORE_VALUE(IGN_3);
	//struct iov_iter iter;
	int ret;

	if (flags & ~(GRND_NONBLOCK | GRND_RANDOM | GRND_INSECURE))
		return -EINVAL;

	/*
	 * Requesting insecure and blocking randomness at the same time makes
	 * no sense.
	 */
	if ((flags & (GRND_INSECURE | GRND_RANDOM)) == (GRND_INSECURE | GRND_RANDOM))
		return -EINVAL;

	uint8_t *ret_buf = (uint8_t *) buf;
    uint64_t length = size;
    while (length >= 8) {
        uint64_t value = (uint64_t) ((RND::rand(1337, 0, 0x8FFFFFFF) % 65535)
                                     * (rdtsc() % 65535));
        *((uint64_t *) (ret_buf)) = value;
        ret_buf += 8;
        length -= 8;
    }

    if (length > 0) {
        uint64_t value = (uint64_t) ((RND::rand(1337, 0, 0x8FFFFFFF) % 65535)
                                     * (rdtsc() % 65535));
        __memcpy(ret_buf, &value, length);
    }
    return 0;
}
