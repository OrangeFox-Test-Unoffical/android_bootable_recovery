#ifndef TWRP_KERNEL_MODULE_LOADER_HPP
#define TWRP_KERNEL_MODULE_LOADER_HPP

#include <filesystem>
#include <string>
#include <vector>

// Base paths probed for kernel modules by TWRP.

// vendor modules (mounted /vendor)
inline std::filesystem::path VENDOR_MODULE_DIR = "/vendor/lib/modules";

// vendor_boot ramdisk GKI modules
inline std::filesystem::path VENDOR_BOOT_MODULE_DIR = "/lib/modules";

// vendor_dlkm placed modules
inline std::filesystem::path VENDOR_DLKM_MODULE_DIR = "/vendor_dlkm/lib/modules";

enum class BootMode {
    RecoveryFastboot = 0,
    RecoveryInBoot,
    Fastbootd,
};

class KernelModuleLoader {
public:
    // Load maintainer-defined kernel modules in TWRP
    static bool Load_Vendor_Modules();

private:
    // Use libmodprobe to attempt loading kernel modules
    static bool Try_And_Load_Modules(std::string module_dir, bool vendor_is_mounted);

    // Write list of modules to load from TW_LOAD_VENDOR_MODULES
    static bool Write_Module_List(const std::string& module_dir);

    // Copy modules to ramdisk for loading
    static bool Copy_Modules_To_Tmpfs(const std::string& module_dir);

    // Modules already loaded by init that we must not reload
    static std::vector<std::string> Skip_Loaded_Kernel_Modules();

    // Query the current boot mode
    static BootMode Get_Boot_Mode();
};

#endif  // TWRP_KERNEL_MODULE_LOADER_HPP