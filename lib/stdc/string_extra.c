//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT

/*
 * lib/stdc/string_extra.c — 内嵌 Lua 5.4 需要、str.c/memory.c 里缺失的
 * ISO C 字符串函数。
 *
 * 覆盖: memchr / strstr / strspn / strcspn / strrchr / strpbrk /
 *       strcoll (C locale 下等价于 strcmp) / strerror。
 */

#include <string.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* memchr: 在 n 字节内查找第一个等于 c 的字节                           */
/* ------------------------------------------------------------------ */
void *memchr(const void *str, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)str;
    unsigned char uc = (unsigned char)c;
    size_t i;

    if (!p) return NULL;
    for (i = 0; i < n; i++) {
        if (p[i] == uc) return (void *)(p + i);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* strstr: 子串查找 (空 needle 返回 haystack)                            */
/* ------------------------------------------------------------------ */
char *strstr(const char *haystack, const char *needle) {
    if (!haystack || !needle) return NULL;
    if (needle[0] == '\0') return (char *)haystack;

    for (; *haystack != '\0'; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h != '\0' && *n != '\0' && *h == *n) {
            h++;
            n++;
        }
        if (*n == '\0') return (char *)haystack;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* strspn / strcspn                                                     */
/* ------------------------------------------------------------------ */
size_t strspn(const char *str, const char *accept) {
    size_t n = 0;
    if (!str || !accept) return 0;
    while (str[n] != '\0' && strchr(accept, (int)(unsigned char)str[n]) != NULL) n++;
    return n;
}

size_t strcspn(const char *str, const char *reject) {
    size_t n = 0;
    if (!str || !reject) return 0;
    while (str[n] != '\0' && strchr(reject, (int)(unsigned char)str[n]) == NULL) n++;
    return n;
}

/* ------------------------------------------------------------------ */
/* strrchr: 最后一次出现的字符 (含 '\0', 标准语义)                       */
/* ------------------------------------------------------------------ */
char *strrchr(const char *str, int c) {
    const char *last = NULL;
    unsigned char uc = (unsigned char)c;

    if (!str) return NULL;
    do {
        if ((unsigned char)*str == uc) last = str;
    } while (*str++ != '\0');
    return (char *)last;
}

/* ------------------------------------------------------------------ */
/* strpbrk: 第一个出现在 accept 中的字符                                 */
/* ------------------------------------------------------------------ */
char *strpbrk(const char *str, const char *accept) {
    if (!str || !accept) return NULL;
    for (; *str != '\0'; str++) {
        if (strchr(accept, (int)(unsigned char)*str) != NULL) return (char *)str;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* strcoll: 目前只有 C locale, 排序等价于逐字节 strcmp                    */
/* ------------------------------------------------------------------ */
int strcoll(const char *str1, const char *str2) {
    return strcmp(str1, str2);
}

/* ------------------------------------------------------------------ */
/* strerror: errno -> 静态只读描述 (ISO C 要求返回 char*)                */
/* ------------------------------------------------------------------ */
static const char *const sky_err_msgs[] = {
    [0]                    = "Success",
    [EPERM]                = "Operation not permitted",
    [ENOENT]               = "No such file or directory",
    [ESRCH]                = "No such process",
    [EINTR]                = "Interrupted system call",
    [EIO]                  = "Input/output error",
    [ENXIO]                = "No such device or address",
    [E2BIG]                = "Argument list too long",
    [ENOEXEC]              = "Exec format error",
    [EBADF]                = "Bad file descriptor",
    [ECHILD]               = "No child processes",
    [EAGAIN]               = "Resource temporarily unavailable",
    [ENOMEM]               = "Cannot allocate memory",
    [EACCES]               = "Permission denied",
    [EFAULT]               = "Bad address",
    [ENOTBLK]              = "Block device required",
    [EBUSY]                = "Device or resource busy",
    [EEXIST]               = "File exists",
    [EXDEV]                = "Invalid cross-device link",
    [ENODEV]               = "No such device",
    [ENOTDIR]              = "Not a directory",
    [EISDIR]               = "Is a directory",
    [EINVAL]               = "Invalid argument",
    [ENFILE]               = "Too many open files in system",
    [EMFILE]               = "Too many open files",
    [ENOTTY]               = "Inappropriate ioctl for device",
    [ETXTBSY]              = "Text file busy",
    [EFBIG]                = "File too large",
    [ENOSPC]               = "No space left on device",
    [ESPIPE]               = "Illegal seek",
    [EROFS]                = "Read-only file system",
    [EMLINK]               = "Too many links",
    [EPIPE]                = "Broken pipe",
    [EDOM]                 = "Numerical argument out of domain",
    [ERANGE]               = "Numerical result out of range",
    [EDEADLK]              = "Resource deadlock avoided",
    [ENAMETOOLONG]         = "File name too long",
    [ENOLCK]               = "No locks available",
    [ENOSYS]               = "Function not implemented",
    [ENOTEMPTY]            = "Directory not empty",
    [ELOOP]                = "Too many levels of symbolic links",
    [ENOMSG]               = "No message of desired type",
    [EIDRM]                = "Identifier removed",
    [ECHRNG]               = "Channel number out of range",
    [EL2NSYNC]             = "Level 2 not synchronized",
    [EL3HLT]               = "Level 3 halted",
    [EL3RST]               = "Level 3 reset",
    [ELNRNG]               = "Link number out of range",
    [EUNATCH]              = "Protocol driver not attached",
    [ENOCSI]               = "No CSI structure available",
    [EL2HLT]               = "Level 2 halted",
    [EBADE]                = "Invalid exchange",
    [EBADR]                = "Invalid request descriptor",
    [EXFULL]               = "Exchange full",
    [ENOANO]               = "No anode",
    [EBADRQC]              = "Invalid request code",
    [EBADSLT]              = "Invalid slot",
    [EBFONT]               = "Bad font file format",
    [ENOSTR]               = "Device not a stream",
    [ENODATA]              = "No data available",
    [ETIME]                = "Timer expired",
    [ENOSR]                = "Out of streams resources",
    [ENONET]               = "Machine is not on the network",
    [ENOPKG]               = "Package not installed",
    [EREMOTE]              = "Object is remote",
    [ENOLINK]              = "Link has been severed",
    [EADV]                 = "Advertise error",
    [ESRMNT]               = "Srmount error",
    [ECOMM]                = "Communication error on send",
    [EPROTO]               = "Protocol error",
    [EMULTIHOP]            = "Multihop attempted",
    [EDOTDOT]              = "RFS specific error",
    [EBADMSG]              = "Bad message",
    [EOVERFLOW]            = "Value too large for defined data type",
    [ENOTUNIQ]             = "Name not unique on network",
    [EBADFD]               = "File descriptor in bad state",
    [EREMCHG]              = "Remote address changed",
    [ELIBACC]              = "Can not access a needed shared library",
    [ELIBBAD]              = "Accessing a corrupted shared library",
    [ELIBSCN]              = ".lib section in a.out corrupted",
    [ELIBMAX]              = "Attempting to link in too many shared libraries",
    [ELIBEXEC]             = "Cannot exec a shared library directly",
    [EILSEQ]               = "Invalid or incomplete multibyte or wide character",
    [ERESTART]             = "Interrupted system call should be restarted",
    [ESTRPIPE]             = "Streams pipe error",
    [EUSERS]               = "Too many users",
    [ENOTSOCK]             = "Socket operation on non-socket",
    [EDESTADDRREQ]         = "Destination address required",
    [EMSGSIZE]             = "Message too long",
    [EPROTOTYPE]           = "Protocol wrong type for socket",
    [ENOPROTOOPT]          = "Protocol not available",
    [EPROTONOSUPPORT]      = "Protocol not supported",
    [ESOCKTNOSUPPORT]      = "Socket type not supported",
    [EOPNOTSUPP]           = "Operation not supported",
    [EPFNOSUPPORT]         = "Protocol family not supported",
    [EAFNOSUPPORT]         = "Address family not supported by protocol",
    [EADDRINUSE]           = "Address already in use",
    [EADDRNOTAVAIL]        = "Cannot assign requested address",
    [ENETDOWN]             = "Network is down",
    [ENETUNREACH]          = "Network is unreachable",
    [ENETRESET]            = "Network dropped connection on reset",
    [ECONNABORTED]         = "Software caused connection abort",
    [ECONNRESET]           = "Connection reset by peer",
    [ENOBUFS]              = "No buffer space available",
    [EISCONN]              = "Transport endpoint is already connected",
    [ENOTCONN]             = "Transport endpoint is not connected",
    [ESHUTDOWN]            = "Cannot send after transport endpoint shutdown",
    [ETOOMANYREFS]         = "Too many references: cannot splice",
    [ETIMEDOUT]            = "Connection timed out",
    [ECONNREFUSED]         = "Connection refused",
    [EHOSTDOWN]            = "Host is down",
    [EHOSTUNREACH]         = "No route to host",
    [EALREADY]             = "Operation already in progress",
    [EINPROGRESS]          = "Operation now in progress",
    [ESTALE]               = "Stale file handle",
    [EUCLEAN]              = "Structure needs cleaning",
    [ENOTNAM]              = "Not a XENIX named type file",
    [ENAVAIL]              = "No XENIX semaphores available",
    [EISNAM]               = "Is a named type file",
    [EREMOTEIO]            = "Remote I/O error",
    [EDQUOT]               = "Disk quota exceeded",
    [ENOMEDIUM]            = "No medium found",
    [EMEDIUMTYPE]          = "Wrong medium type",
    [ECANCELED]            = "Operation canceled",
    [ENOKEY]               = "Required key not available",
    [EKEYEXPIRED]          = "Key has expired",
    [EKEYREVOKED]          = "Key has been revoked",
    [EKEYREJECTED]         = "Key was rejected by service",
    [EOWNERDEAD]           = "Owner died",
    [ENOTRECOVERABLE]      = "State not recoverable",
    [ERFKILL]              = "Operation not possible due to RF-kill",
    [EHWPOISON]            = "Memory page has hardware error",
};

#define SKY_ERR_MSG_COUNT ((int)(sizeof(sky_err_msgs) / sizeof(sky_err_msgs[0])))

char *strerror(int errnum) {
    if (errnum >= 0 && errnum < SKY_ERR_MSG_COUNT && sky_err_msgs[errnum] != NULL)
        return (char *)sky_err_msgs[errnum];
    return (char *)"Unknown error";
}
