/*
 * OrangeFox -> soong defaults 插件(薄壳)。
 *
 * 所有取值与 -D 生成逻辑都在 fox_common.go(无 init,可被多个 go 包共用);
 * 本文件只负责注册模块类型,供需要的模块在 defaults: 里引用:
 *   recovery(Android.bp)、orscmd(orscmd/Android.bp)、libguitwrp(gui/Android.bp)。
 *
 * 未加保护地使用 FOX_ 与 OF_ 宏的源码文件分布:
 *   recovery  : data.cpp / orangefox.cpp / twrp-functions.cpp / partitionmanager.cpp / startupArgs.cpp
 *   libguitwrp: gui/action.cpp / gui/gui.cpp / gui/fileselector.cpp / gui/pages.cpp / gui/objects.hpp
 * 新增模块若引用了这些宏,把 "fox_recovery_defaults" 加进它的 defaults 即可。
 */
package twrp

import (
	"android/soong/android"
	"android/soong/cc"
)

func foxRecoveryDefaults(ctx android.LoadHookContext) {
	type props struct {
		Cflags []string
	}
	p := &props{}
	p.Cflags = foxGlobalFlags(ctx)
	ctx.AppendProperties(p)
}

func init() {
	android.RegisterModuleType("fox_recovery_defaults", foxRecoveryDefaultsFactory)
}

func foxRecoveryDefaultsFactory() android.Module {
	module := cc.DefaultsFactory()
	android.AddLoadHook(module, foxRecoveryDefaults)
	return module
}
