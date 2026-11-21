//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
/* pci_name_hook.cpp —— PCI 名称表的 Tier0 跳板。
 *
 * pcidesc.cpp 已迁入 kernel.pci.core.ko；内核 pci.cpp 的日志在枚举时查名，
 * 枚举由 pci.core/pci.enumerate 组件在模块加载后触发，因此可用函数指针。 */
#include <arch/x86_64/dev/pci/pci.h>

namespace {
const char *(*g_vendor_name)(uint16_t) = nullptr;
const char *(*g_device_name)(uint16_t, uint16_t) = nullptr;
const char *(*g_class_name)(uint8_t) = nullptr;
const char *(*g_subclass_name)(uint8_t, uint8_t) = nullptr;
const char *(*g_progif_name)(uint8_t, uint8_t, uint8_t) = nullptr;
}

extern "C" void pci_name_set_hooks(const char *(*vendor)(uint16_t),
                                   const char *(*device)(uint16_t, uint16_t),
                                   const char *(*cls)(uint8_t),
                                   const char *(*subclass)(uint8_t, uint8_t),
                                   const char *(*progif)(uint8_t, uint8_t, uint8_t))
{
    __atomic_store_n(&g_vendor_name, vendor, __ATOMIC_RELEASE);
    __atomic_store_n(&g_device_name, device, __ATOMIC_RELEASE);
    __atomic_store_n(&g_class_name, cls, __ATOMIC_RELEASE);
    __atomic_store_n(&g_subclass_name, subclass, __ATOMIC_RELEASE);
    __atomic_store_n(&g_progif_name, progif, __ATOMIC_RELEASE);
}

namespace PCI {
const char *unknownString = "Unknown";
const char *GetVendorName(uint16_t vendorID)
{
    auto fn = __atomic_load_n(&g_vendor_name, __ATOMIC_ACQUIRE);
    return fn ? fn(vendorID) : "Unknown";
}
const char *GetDeviceName(uint16_t vendorID, uint16_t deviceID)
{
    auto fn = __atomic_load_n(&g_device_name, __ATOMIC_ACQUIRE);
    return fn ? fn(vendorID, deviceID) : "Unknown";
}
const char *GetClassName(uint8_t classCode)
{
    auto fn = __atomic_load_n(&g_class_name, __ATOMIC_ACQUIRE);
    return fn ? fn(classCode) : "Unknown";
}
const char *GetSubclassName(uint8_t classCode, uint8_t subclassCode)
{
    auto fn = __atomic_load_n(&g_subclass_name, __ATOMIC_ACQUIRE);
    return fn ? fn(classCode, subclassCode) : "Unknown";
}
const char *GetProgIFName(uint8_t classCode, uint8_t subclassCode, uint8_t progIFCode)
{
    auto fn = __atomic_load_n(&g_progif_name, __ATOMIC_ACQUIRE);
    return fn ? fn(classCode, subclassCode, progIFCode) : "Unknown";
}
}
