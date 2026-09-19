/*
 * OrangeFox (FOX_* / OF_*) -> Soong 兼容层,完全复用 TWRP16 自带的插件机制。
 *
 * 取值机制与 TWRP 自己的 TW_* 一致:
 *   vendor/twrp/config/BoardConfigSoong.mk 的 FOX_EXPORTED_VARS
 *     -> SOONG_CONFIG_twrpVarsPlugin_*  (与 TW_* 同一个命名空间 twrpVarsPlugin)
 *     -> soong.variables
 *     -> soong/makevars.go:getMakeVars()
 *   getMakeVars() 取不到时回落到环境变量(vendorsetup.sh 的 export),
 *   即上游 OrangeFox orangefox_common.go:foxVar() 的双通道写法。
 *
 * 默认值不在这里:按 TWRP16 惯例放在 Make(BoardConfigSoong.mk 里的
 *   FOX_INTERNAL_RELEASE ?= R12 / OF_SCREEN_H ?= 1920 ...),
 * 本文件只做"变量名 -> 宏名"的映射(对应 vendor/twrp/build/soong/Android.bp 里
 * twrp 类型的 cflags 映射表)。
 *
 * 本文件刻意不含 init()/RegisterModuleType(与上游 orangefox_common.go 同理):
 * 它会被 recovery 插件包和 gui 插件包同时编译,注册动作放在
 * fox_recovery_defaults.go 的薄壳里,否则会重复注册同一模块类型。
 */
package twrp

import (
	"android/soong/android"
	"fmt"
	"strings"
)

// foxVar 读取一个 make 变量:先走 twrpVarsPlugin(与 TW_* 同一条路),再回落到 env。
func foxVar(ctx android.BaseContext, name string) string {
	if v := getMakeVars(ctx, name); v != "" {
		return v
	}
	return strings.TrimSpace(ctx.Config().Getenv(name))
}

// -D 的注入形态,与 orangefox.mk 一一对应
const (
	modeStr  = byte('s') // -DNAME="val"  (orangefox.mk: -DNAME='"$(NAME)"')
	modePres = byte('p') // -DNAME        (orangefox.mk: -DNAME)
	modeInt  = byte('1') // -DNAME=1      (orangefox.mk: -DNAME=1)
	modeVal  = byte('v') // -DNAME=val    (orangefox.mk: -DNAME=$(NAME))
)

type foxFlag struct {
	name string
	mode byte
}

// 逐项对应 orangefox.mk 的 -D 注入清单(形态照抄,只有 FDE 项被剥除)。
// 只在此处注入:派生结果(foxForcedFlags)不再单独产出 -D,
// 避免同一个宏出现两种定义形态(clang -Wmacro-redefined + -Werror)。
var foxFlagTable = []foxFlag{
	// --- 版本 / 构建信息 ---
	{"FOX_INTERNAL_RELEASE", modeStr},
	{"FOX_MAINTAINER_PATCH_VERSION", modeStr},
	{"FOX_BUILD", modeStr},
	{"FOX_VARIANT", modeStr},
	{"FOX_BUILD_TYPE", modeStr},
	{"FOX_BUILD_DEVICE", modeStr},
	{"FOX_CURRENT_DEV_STR", modeStr},
	{"FOX_DEVICE_MODEL", modeStr},
	{"FOX_TARGET_DEVICES", modeStr},
	{"FOX_SPR", modeStr},
	{"OF_CURRENT_BRANCH", modeStr},
	{"OF_MAINTAINER", modeStr},
	{"OF_DEFAULT_TIMEZONE", modeStr},
	{"OF_DEFAULT_KEYMASTER_VERSION", modeStr},

	// --- 属性 / 存储 / 显示 ---
	{"FOX_SETTINGS_ROOT_DIRECTORY", modeStr},
	{"FOX_MISCELLANEOUS_ROOT_DIRECTORY", modeStr},
	{"OF_OPTIONS_LIST_NUM", modeStr},
	{"OF_QUICK_BACKUP_LIST", modeStr},
	{"OF_DYNAMIC_FULL_SIZE", modeStr},
	{"OF_FL_PATH1", modeStr},
	{"OF_FL_PATH2", modeStr},
	{"OF_FLASHLIGHT_ENABLE", modeStr},
	{"OF_SPLASH_MAX_SIZE", modeStr},
	{"OF_SCREEN_H", modeStr},
	{"OF_STATUS_H", modeStr},
	{"OF_HIDE_NOTCH", modeStr},
	{"OF_STATUS_INDENT_LEFT", modeStr},
	{"OF_STATUS_INDENT_RIGHT", modeStr},
	{"OF_CLOCK_POS", modeStr},
	{"OF_ALLOW_DISABLE_NAVBAR", modeStr},

	// --- A/B / virtual A/B / 设备形态 ---
	{"FOX_AB_DEVICE", modeStr},
	{"FOX_VIRTUAL_AB_DEVICE", modeStr},
	{"FOX_VANILLA_BUILD", modeStr},
	{"FOX_VENDOR_BOOT_RECOVERY", modeStr},
	{"FOX_PATCH_VBMETA_FLAG", modeStr},
	{"OF_AB_DEVICE_WITH_RECOVERY_PARTITION", modeStr},
	{"FOX_USE_DATA_RECOVERY_FOR_SETTINGS", modePres},
	{"FOX_USE_NANO_EDITOR", modeStr},

	// --- MIUI / OTA ---
	{"OF_DISABLE_MIUI_SPECIFIC_FEATURES", modeStr},
	{"OF_DISABLE_MIUI_OTA_BY_DEFAULT", modeStr},
	{"OF_SKIP_ORANGEFOX_PROCESS", modeStr},
	{"OF_DONT_PATCH_ON_FRESH_INSTALLATION", modeStr},
	{"OF_NO_MIUI_PATCH_WARNING", modeStr},
	{"OF_NO_MIUI_OTA_VENDOR_BACKUP", modeStr},
	{"OF_NO_TREBLE_COMPATIBILITY_CHECK", modeStr},
	{"OF_SUPPORT_ALL_BLOCK_OTA_UPDATES", modeStr},
	{"OF_OTA_BACKUP_STOCK_BOOT_IMAGE", modePres},
	{"OF_OTA_RES_CHECK_MICROSD", modeStr},
	{"OF_FIX_OTA_UPDATE_MANUAL_FLASH_ERROR", modeStr},
	{"OF_INCREMENTAL_OTA_BACKUP_SUPER", modeStr},
	{"OF_DISABLE_ORS_AUTO_REBOOT", modePres},

	// --- 补丁 / 校验 / 高级 ---
	{"OF_FORCE_MAGISKBOOT_BOOT_PATCH_MIUI", modeStr},
	{"OF_PATCH_AVB20", modeStr},
	{"OF_SUPPORT_VBMETA_AVB2_PATCHING", modeStr},
	{"OF_ADVANCED_SECURITY", modeStr},
	{"OF_CHECK_OVERWRITE_ATTEMPTS", modeStr},
	{"OF_NO_SPLASH_CHANGE", modeStr},
	{"OF_REPORT_HARMLESS_MOUNT_ISSUES", modeStr},
	{"OF_DONT_SUBSTITUTE_PERMISSIONS", modePres},
	{"OF_FORCE_CHECK_RAMDISK_CHECKSUM", modeStr},
	{"OF_NO_REFLASH_CURRENT_ORANGEFOX", modeStr},
	{"FOX_BUGGED_AOSP_ARB_WORKAROUND", modeStr},
	{"FOX_ALLOW_EARLY_SETTINGS_LOAD", modeStr},

	// --- 功能开关(裸存在式) ---
	{"OF_RECOVERY_AB_FULL_REFLASH_RAMDISK", modePres},
	{"OF_USE_LOCKSCREEN_BUTTON", modePres},
	{"OF_USE_LZ4_COMPRESSION", modePres},
	{"OF_NO_ADDITIONAL_MIUI_PROPS_CHECK", modePres},
	{"OF_DISABLE_OTA_MENU", modePres},
	{"OF_ENABLE_FS_COMPRESSION", modePres},
	{"OF_USE_FS_COMPRESSION", modePres},
	{"OF_LOOP_DEVICE_ERRORS_TO_LOG", modePres},
	{"OF_DISPLAY_FORMAT_FILESYSTEMS_DEBUG_INFO", modePres},
	{"OF_WIPE_METADATA_AFTER_DATAFORMAT", modePres},
	{"OF_BIND_MOUNT_SDCARD_ON_FORMAT", modePres},
	{"OF_UNMOUNT_SDCARDS_BEFORE_REBOOT", modePres},
	{"OF_FORCE_DATA_FORMAT_F2FS", modePres},
	{"OF_FORCE_DATA_FORMAT_EXT4", modePres},
	{"OF_USE_LEGACY_TIME_FIXUP", modePres},
	{"OF_FORCE_CASEFOLDING", modePres},
	{"OF_SKIP_PREBUILT_MODULES", modePres},
	{"OF_WORKAROUND_BACKUP_BUG", modePres},
	{"OF_USE_AIDL_BOOT_CONTROL", modePres},
	{"OF_ENABLE_FRP_ADDON", modePres},
	{"OF_NO_REBOOT_FASTBOOT", modePres},
	{"OF_VAB_ORS_WIPE_DATA_IS_FORMAT", modePres},
	{"OF_FBE_METADATA_MOUNT_IGNORE", modeStr},
	{"OF_SKIP_DECRYPTED_ADOPTED_STORAGE", modeStr},
	{"OF_UNBIND_SDCARD_F2FS", modeStr},
	{"OF_FIX_DECRYPTION_ON_DATA_MEDIA", modeStr},
	{"OF_SKIP_FBE_DECRYPTION", modeStr},
	{"OF_SKIP_FBE_DECRYPTION_SDKVERSION", modeStr},
	{"OF_CLASSIC_LEDS_FUNCTION", modeStr},
	{"OF_NO_GREEN_LED", modeStr},
	{"OF_NO_KEYMASTER_VER_4X", modePres},
	{"OF_REFRESH_ENCRYPTION_PROPS_BEFORE_FORMAT", modePres},
	{"OF_DEVICE_WITHOUT_PERSIST", modePres},
	{"OF_ENABLE_LAB", modeStr},
	{"OF_SUPPORT_OZIP_DECRYPTION", modeStr},
	{"OF_REDUCE_DECRYPTION_TIMEOUT", modeStr},
	{"OF_DONT_KEEP_LOG_HISTORY", modeStr},
	{"OF_DISABLE_EXTRA_ABOUT_PAGE", modeStr},
	{"OF_BLOCK_OPERATIONS_AFTER_ROM_FLASH", modeStr},
	{"OF_USE_GREEN_LED", modeStr},
	{"OF_USE_LEGACY_BATTERY_SERVICES", modePres},
	{"OF_ENABLE_WLAN", modePres},
	{"OF_FORCE_PREBUILT_KERNEL", modePres},
	{"OF_USE_DMCTL", modeStr},
	{"FOX_EXCLUDE_ZIP", modePres},
	{"FOX_USE_UPDATED_MAGISKBOOT", modePres},
	{"FOX_DELETE_INITD_ADDON", modePres},
	{"FOX_DELETE_MAGISK_ADDON", modeStr},
	{"FOX_DELETE_AROMAFM", modePres},
	{"FOX_MOVE_MAGISK_INSTALLER_TO_RAMDISK", modeStr},
	{"FOX_ENABLE_APP_MANAGER", modeStr},
	{"FOX_ENABLE_KERNELSU_SUPPORT", modePres},
	{"FOX_ENABLE_KERNELSU_NEXT_SUPPORT", modePres},
	{"FOX_ENABLE_SUKISU_SUPPORT", modePres},
	{"FOX_USE_DMSETUP", modeStr},

	// --- 数值 1 形态 ---
	{"OF_USE_MAGISKBOOT", modeInt},
	{"OF_USE_MAGISKBOOT_FOR_ALL_PATCHES", modeInt},
	{"OF_MASK_GET_FOLDER_SIZE_READ_ERRORS", modeInt},
	{"FOX_USE_MEIZU_TOUCH_MAPPING", modeInt},

	// --- 原样取值(不带引号) ---
	{"PRODUCT_PLATFORM", modeVal},
}

// foxForcedFlags 镜像 orangefox.mk 的 ifeq -> 赋值派生链,只返回"应被置位"的
// 变量名;是否真的注入、用什么形态,统一由 foxFlagTable 决定。
func foxForcedFlags(ctx android.BaseContext) map[string]bool {
	forced := map[string]bool{}

	ab := foxVar(ctx, "FOX_AB_DEVICE")
	virtualAB := foxVar(ctx, "FOX_VIRTUAL_AB_DEVICE")
	vanilla := foxVar(ctx, "FOX_VANILLA_BUILD")
	vendorBoot := foxVar(ctx, "FOX_VENDOR_BOOT_RECOVERY")
	buildType := foxVar(ctx, "FOX_BUILD_TYPE")
	disableMiui := foxVar(ctx, "OF_DISABLE_MIUI_SPECIFIC_FEATURES")
	miuiOtaDefault := foxVar(ctx, "OF_DISABLE_MIUI_OTA_BY_DEFAULT")
	skipFoxProcess := foxVar(ctx, "OF_SKIP_ORANGEFOX_PROCESS")
	dontPatchFresh := foxVar(ctx, "OF_DONT_PATCH_ON_FRESH_INSTALLATION")
	abWithRecovery := foxVar(ctx, "OF_AB_DEVICE_WITH_RECOVERY_PARTITION")
	twrpCompat := foxVar(ctx, "OF_TWRP_COMPATIBILITY_MODE")
	abOtaUpdater := foxVar(ctx, "AB_OTA_UPDATER")

	// virtual A/B 或 AB_OTA_UPDATER -> AB 设备
	if virtualAB == "1" || abOtaUpdater == "true" {
		ab = "1"
		if virtualAB == "1" {
			vanilla = "1"
		}
	}
	// vendor_boot recovery -> AB + vanilla + 不改 splash
	if vendorBoot == "1" {
		ab = "1"
		vanilla = "1"
		forced["OF_NO_SPLASH_CHANGE"] = true
	}
	// 带独立 recovery 分区的 A/B
	if abWithRecovery == "1" {
		ab = "1"
	}
	// TWRP 兼容模式 -> 关掉 MIUI 专属功能
	if twrpCompat == "1" {
		disableMiui = "1"
	}
	// Stable 构建 -> 高级安全
	if buildType == "Stable" {
		forced["OF_ADVANCED_SECURITY"] = true
	}
	// 跳过 orangefox 流程 -> 顺便关掉 fresh install 补丁
	if skipFoxProcess == "1" {
		dontPatchFresh = "1"
	}
	// vanilla 派生组
	if vanilla == "1" {
		forced["FOX_VANILLA_BUILD"] = true
		forced["OF_SKIP_ORANGEFOX_PROCESS"] = true
		forced["OF_DISABLE_MIUI_SPECIFIC_FEATURES"] = true
		forced["OF_DISABLE_OTA_MENU"] = true
		forced["OF_NO_ADDITIONAL_MIUI_PROPS_CHECK"] = true
		forced["OF_DONT_PATCH_ON_FRESH_INSTALLATION"] = true
		forced["OF_NO_MIUI_PATCH_WARNING"] = true
	}
	if disableMiui == "1" {
		forced["OF_DISABLE_MIUI_SPECIFIC_FEATURES"] = true
	}
	if miuiOtaDefault == "1" {
		forced["OF_DISABLE_MIUI_OTA_BY_DEFAULT"] = true
	}
	if dontPatchFresh == "1" {
		forced["OF_DONT_PATCH_ON_FRESH_INSTALLATION"] = true
	}
	if ab == "1" {
		forced["FOX_AB_DEVICE"] = true
	}

	return forced
}

// foxGlobalFlags 产出全部 FOX_*/OF_* 的 -D。
func foxGlobalFlags(ctx android.BaseContext) []string {
	var cflags []string
	forced := foxForcedFlags(ctx)

	for _, f := range foxFlagTable {
		val := strings.TrimSpace(foxVar(ctx, f.name))
		if val == "" && forced[f.name] {
			val = "1"
		}
		if val == "" {
			continue
		}
		switch f.mode {
		case modeStr:
			cflags = append(cflags, fmt.Sprintf("-D%s=\"%s\"", f.name, val))
		case modePres:
			cflags = append(cflags, "-D"+f.name)
		case modeInt:
			cflags = append(cflags, "-D"+f.name+"=1")
		case modeVal:
			cflags = append(cflags, fmt.Sprintf("-D%s=%s", f.name, val))
		}
	}

	// orangefox.mk 对这两项始终注入空串(其 else 分支为 -DOF_FL_PATH1='""')。
	// OF_FLASHLIGHT_ENABLE 有 Make 默认值 "1",它的 #ifdef 块(data.cpp:863)
	// 会被真的编译进去并引用这两个宏,所以未设置它们的设备必须有空串定义。
	for _, name := range []string{"OF_FL_PATH1", "OF_FL_PATH2"} {
		if foxVar(ctx, name) == "" {
			cflags = append(cflags, fmt.Sprintf("-D%s=\"\"", name))
		}
	}

	// gui/action.cpp 未加保护地使用该宏,而 TWRP 自己的 twrp_recovery_defaults.go
	// 只在它非空时才定义 —— 这里在未设置时补一个空串定义,保证 libguitwrp 能编译
	// (与 TWRP 已定义的值不同时不会同时生效,故不会触发 macro redefined)。
	if foxVar(ctx, "TW_OZIP_DECRYPT_KEY") == "" {
		cflags = append(cflags, "-DTW_OZIP_DECRYPT_KEY=\"\"")
	}

	return cflags
}
