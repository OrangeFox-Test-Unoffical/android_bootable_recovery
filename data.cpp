/*
	Copyright 2012 to 2021 TeamWin
	This file is part of TWRP/TeamWin Recovery Project.

	TWRP is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	TWRP is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with TWRP.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <pthread.h>
#include <time.h>
#include <string>
#include <sstream>
#include <cctype>
#include <filesystem>
#include <system_error>
#include <unordered_set>
#include <cutils/properties.h>
#include <fstab/fstab.h>
#include <unistd.h>

#include "variables.h"
#include "data.hpp"
#include "partitions.hpp"
#include "twrp-functions.hpp"
#ifndef TW_NO_SCREEN_TIMEOUT
#include "gui/blanktimer.hpp"
#endif
#include "set_metadata.h"
#include "gui/gui.hpp"
#include "infomanager.hpp"
#include "unit_conversion.hpp"

extern "C"
{
	#include "twcommon.h"
	#include "gui/pages.h"
}
#include "twrpminui/minui.h"

#define FILE_VERSION 0x00010010 // Do not set to 0

using namespace std;

string                                  DataManager::mBackingFile;
int                                     DataManager::mInitialized = 0;
InfoManager                             DataManager::mPersist;  // Data that that is not constant and will be saved to the settings file
InfoManager                             DataManager::mData;     // Data that is not constant and will not be saved to settings file
InfoManager                             DataManager::mConst;    // Data that is constant and will not be saved to settings file

string DataManager::bPassEnabled = "0";
string DataManager::bPassPass = "4ee92c7c7909dc2a1ddaefe93ed97efa27a9b8cab8f1b90c199f917756d00f940155bade0da13e717f0c4a1069de9582e0dd5b1affef427fc7303aa9b593740c";
string DataManager::bPassType = "0"; 

extern bool datamedia;

#ifndef PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
pthread_mutex_t DataManager::m_valuesLock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
#else
pthread_mutex_t DataManager::m_valuesLock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
#endif

void DataManager::get_device_id() {
	std::string serialno;

#ifdef TW_USE_SERIALNO_PROPERTY_FOR_DEVICE_ID
	// Get serial number from system property
	serialno = android::base::GetProperty("ro.serialno", "");
#else
	// Get serial number from bootconfig
	android::fs_mgr::GetBootconfig("androidboot.serialno", &serialno);
#endif
	mConst.SetValue("device_id", serialno);
}

int DataManager::ResetDefaults()
{
	pthread_mutex_lock(&m_valuesLock);
	mPersist.Clear();
	mData.Clear();
	mConst.Clear();
	pthread_mutex_unlock(&m_valuesLock);

	SetDefaultValues();
	return 0;
}

int DataManager::LoadValues(const string& filename)
{
	string dev_id;

	if (!mInitialized)
		SetDefaultValues();

	GetValue("device_id", dev_id);
	// Save off the backing file for set operations
	mBackingFile = filename;
	mPersist.SetFile(filename);
	mPersist.SetFileVersion(FILE_VERSION);

	// Read in the file, if possible
	pthread_mutex_lock(&m_valuesLock);
	mPersist.LoadValues();

#ifndef TW_NO_SCREEN_TIMEOUT
	blankTimer.setTime(mPersist.GetIntValue("tw_screen_timeout_secs"));
#endif

	pthread_mutex_unlock(&m_valuesLock);
	string current = GetCurrentStoragePath();
	TWPartition* Part = PartitionManager.Find_Partition_By_Path(current);
	if (!Part)
		Part = PartitionManager.Get_Default_Storage_Partition();
	if (Part && current != Part->Storage_Path && Part->Mount(false)) {
		LOGINFO("LoadValues setting storage path to '%s'\n", Part->Storage_Path.c_str());
		SetValue("tw_storage_path", Part->Storage_Path);
	} else {
		SetBackupFolder();
	}
	return 0;
}

// Executed when /mnt/vendor/persist is mounted
int DataManager::FindPasswordBackup(void) {
  #ifndef OF_DEVICE_WITHOUT_PERSIST
  if (TWFunc::Path_Exists(FOX_PASS_IN_PERSIST)) {
    bPassEnabled = TWFunc::File_Property_Get(FOX_PASS_IN_PERSIST, "fox_use_pass");
    bPassPass = TWFunc::File_Property_Get(FOX_PASS_IN_PERSIST, "fox_pass_true");
    bPassType = TWFunc::File_Property_Get(FOX_PASS_IN_PERSIST, "fox_pass_type");
		LOGINFO("PassBak: Found backup\n");
  }
  #endif
  return 0;
}

// Executed after .foxs is (not) loaded
int DataManager::RestorePasswordBackup(void) {
  #ifndef OF_DEVICE_WITHOUT_PERSIST
  if (DataManager::GetStrValue("fox_use_pass") == "0") {
    DataManager::SetValue("fox_use_pass", bPassEnabled);
    DataManager::SetValue("fox_pass_true", bPassPass);
    DataManager::SetValue("fox_pass_type", bPassType);
		LOGINFO("PassBak: Loaded backup\n");
  }
  #endif
  return 0;
}

int DataManager::LoadPersistValues(void)
{
#if defined(OF_DEVICE_WITHOUT_PERSIST) || defined(FOX_SETTINGS_ROOT_DIRECTORY)
	//LOGINFO("OF_DEVICE_WITHOUT_PERSIST is set - avoiding /mnt/vendor/persist...\n");
	return -1;
#endif
  static bool loaded = false;
  string dev_id;

  // Only run this function once, and make sure normal settings file has not yet been read
  if (loaded || !mBackingFile.empty()
      || !TWFunc::Path_Exists(PERSIST_SETTINGS_FILE))
    return -1;

  LOGINFO("Attempt to load settings from /mnt/vendor/persist settings file...\n");

  if (!mInitialized)
    SetDefaultValues();

  GetValue("device_id", dev_id);
  mPersist.SetFile(PERSIST_SETTINGS_FILE);
  mPersist.SetFileVersion(FILE_VERSION);

  // Read in the file, if possible
  pthread_mutex_lock(&m_valuesLock);
  mPersist.LoadValues();

#ifndef TW_NO_SCREEN_TIMEOUT
  blankTimer.setTime(mPersist.GetIntValue("tw_screen_timeout_secs"));
#endif

  update_tz_environment_variables();
  TWFunc::Set_Brightness(GetStrValue("tw_brightness"));

  pthread_mutex_unlock(&m_valuesLock);

  /* Don't set storage nor backup paths this early */

  loaded = true;

  return 0;
}

int DataManager::Flush()
{
	return SaveValues();
}

int DataManager::SaveValues()
{
#ifndef TW_OEM_BUILD

	// 与 OrangeFox 对齐:persist 侧只在【没有】定义 FOX_SETTINGS_ROOT_DIRECTORY 时
	// 才作为设置存储;本树定义了它,所以这里只写密码备份(/mnt/vendor/persist/.fsec),
	// 设置本身一律写到 GetSettingsStoragePath()。
#ifndef OF_DEVICE_WITHOUT_PERSIST
	if (PartitionManager.Mount_By_Path("/mnt/vendor/persist", false))
	{
#ifndef FOX_SETTINGS_ROOT_DIRECTORY
		mPersist.SetFile(PERSIST_SETTINGS_FILE);
		mPersist.SetFileVersion(FILE_VERSION);
		pthread_mutex_lock(&m_valuesLock);
		mPersist.SaveValues();
		pthread_mutex_unlock(&m_valuesLock);
		LOGINFO("Saved settings file values to %s\n", PERSIST_SETTINGS_FILE);
#endif

		ofstream file;

		file.open(FOX_PASS_IN_PERSIST, std::ofstream::out | std::ofstream::trunc);
		if (file.is_open()) {
			file << "fox_use_pass="    + DataManager::GetStrValue("fox_use_pass") +
			        "\nfox_pass_true=" + DataManager::GetStrValue("fox_pass_true") +
			        "\nfox_pass_type=" + DataManager::GetStrValue("fox_pass_type");
			LOGINFO("PassBak: Created backup\n");
			file.close();
		} else LOGINFO("PassBak: Failed to backup\n");
	}
#endif

	if (mBackingFile.empty())
		return -1;

	string mount_path = GetSettingsStoragePath();
	PartitionManager.Mount_By_Path(mount_path.c_str(), 1);

	// 用 mBackingFile(由 LoadValues() 设为 <设置目录>/<TW_SETTINGS_FILE>),
	// 不要硬编码 TW_PERSIST_DIR —— 那正是设置被写到 TWRP 目录的原因。
	mPersist.SetFile(mBackingFile);
	mPersist.SetFileVersion(FILE_VERSION);
	pthread_mutex_lock(&m_valuesLock);
	mPersist.SaveValues();
	pthread_mutex_unlock(&m_valuesLock);

	tw_set_default_metadata(mBackingFile.c_str());
	LOGINFO("Saved settings file values to '%s'\n", mBackingFile.c_str());
#endif // ifdef TW_OEM_BUILD
	return 0;
}

int DataManager::GetValue(const string& varName, string& value)
{
	string localStr = varName;
	int ret = 0;

	if (!mInitialized)
		SetDefaultValues();

	// Strip off leading and trailing '%' if provided
	if (localStr.length() > 2 && localStr[0] == '%' && localStr[localStr.length()-1] == '%')
	{
		localStr.erase(0, 1);
		localStr.erase(localStr.length() - 1, 1);
	}

	// Handle magic values
	if (GetMagicValue(localStr, value) == 0)
		return 0;

	// Handle property
	if (localStr.length() > 9 && localStr.substr(0, 9) == "property.") {
		char property_value[PROPERTY_VALUE_MAX];
		property_get(localStr.substr(9).c_str(), property_value, "");
		value = property_value;
		return 0;
	}

	pthread_mutex_lock(&m_valuesLock);
	ret = mConst.GetValue(localStr, value);
	if (ret == 0)
		goto exit;

	ret = mPersist.GetValue(localStr, value);
	if (ret == 0)
		goto exit;

	ret = mData.GetValue(localStr, value);
exit:
	pthread_mutex_unlock(&m_valuesLock);
	return ret;
}

int DataManager::GetValue(const string& varName, int& value)
{
	string data;

	if (GetValue(varName,data) != 0)
		return -1;

	value = atoi(data.c_str());
	return 0;
}

int DataManager::GetValue(const string& varName, float& value)
{
	string data;

	if (GetValue(varName,data) != 0)
		return -1;

	value = atof(data.c_str());
	return 0;
}

int DataManager::GetValue(const string& varName, unsigned long long& value)
{
	string data;

	if (GetValue(varName,data) != 0)
		return -1;

	value = strtoull(data.c_str(), NULL, 10);
	return 0;
}

// This function will return an empty string if the value doesn't exist
string DataManager::GetStrValue(const string& varName)
{
	string retVal;

	GetValue(varName, retVal);
	return retVal;
}

// This function will return 0 if the value doesn't exist
int DataManager::GetIntValue(const string& varName)
{
	string retVal;

	GetValue(varName, retVal);
	return atoi(retVal.c_str());
}

int DataManager::SetValue(const string& varName, const string& value, const int persist /* = 0 */)
{
	if (!mInitialized)
		SetDefaultValues();

	// Handle property
	if (varName.length() > 9 && varName.substr(0, 9) == "property.") {
		int ret = property_set(varName.substr(9).c_str(), value.c_str());
		if (ret)
			LOGERR("Error setting property '%s' to '%s'\n", varName.substr(9).c_str(), value.c_str());
		return ret;
	}

	// Don't allow empty values or numerical starting values
	if (varName.empty() || (varName[0] >= '0' && varName[0] <= '9'))
		return -1;

	string test;
	pthread_mutex_lock(&m_valuesLock);
	int constChk = mConst.GetValue(varName, test);
	if (constChk == 0) {
		pthread_mutex_unlock(&m_valuesLock);
		return -1;
	}

	if (persist) {
		mPersist.SetValue(varName, value);
	} else {
		int persistChk = mPersist.GetValue(varName, test);
		if (persistChk == 0) {
			mPersist.SetValue(varName, value);
		} else {
			mData.SetValue(varName, value);
		}
	}

	pthread_mutex_unlock(&m_valuesLock);

#ifndef TW_NO_SCREEN_TIMEOUT
	if (varName == "tw_screen_timeout_secs") {
		blankTimer.setTime(atoi(value.c_str()));
	} else
#endif
	if (varName == "tw_storage_path") {
		SetBackupFolder();
	}
	gui_notifyVarChange(varName.c_str(), value.c_str());
	return 0;
}

int DataManager::SetValue(const string& varName, const int value, const int persist /* = 0 */)
{
	ostringstream valStr;
	valStr << value;
	return SetValue(varName, valStr.str(), persist);
}

int DataManager::SetValue(const string& varName, const float value, const int persist /* = 0 */)
{
	ostringstream valStr;
	valStr << value;
	return SetValue(varName, valStr.str(), persist);;
}

int DataManager::SetValue(const string& varName, const unsigned long long& value, const int persist /* = 0 */)
{
	ostringstream valStr;
	valStr << value;
	return SetValue(varName, valStr.str(), persist);
}

int DataManager::SetValue(const string& varName, const uint64_t value, const int persist /* = 0 */)
{
	ostringstream valStr;
	valStr << value;
	return SetValue(varName, valStr.str(), persist);
}

// For legacy code that doesn't set a scope
int DataManager::SetProgress(const float Fraction) {
	if (SetValue("ui_portion_size", 0) != 0)
		return -1;
	if (SetValue("ui_portion_start", 0) != 0)
		return -1;
	ShowProgress(1, 0);
	int res = _SetProgress(Fraction);
	if (SetValue("ui_portion_size", 0) != 0)
		return -1;
	if (SetValue("ui_portion_start", 0) != 0)
		return -1;
	return res;
}

int DataManager::_SetProgress(float Fraction) {
	float Portion_Start, Portion_Size;
	GetValue("ui_portion_size", Portion_Size);
	GetValue("ui_portion_start", Portion_Start);
	//LOGINFO("SetProgress(%.2lf): Portion_Size: %.2lf Portion_Start: %.2lf\n", Fraction, Portion_Size, Portion_Start);
	if (Fraction < 0.0)
		Fraction = 0;
	if (Fraction > 1.0)
		Fraction = 1;
	if (SetValue("ui_progress", (float) ((Portion_Start + (Portion_Size * Fraction)) * 100.0)) != 0)
		return -1;
	return (SetValue("ui_progress_portion", 0) != 0);
}

int DataManager::ShowProgress(float Portion, const float Seconds)
{
	float Portion_Start, Portion_Size;
	GetValue("ui_portion_size", Portion_Size);
	GetValue("ui_portion_start", Portion_Start);
	Portion_Start += Portion_Size;
	if(Portion + Portion_Start > 1.0)
		Portion = 1.0 - Portion_Start;
	//LOGINFO("ShowProgress(%.2lf, %.2lf): Portion_Start: %.2lf\n", Portion, Seconds, Portion_Start);
	if (SetValue("ui_portion_start", Portion_Start) != 0)
		return -1;
	if (SetValue("ui_portion_size", Portion) != 0)
		return -1;
	if (SetValue("ui_progress", (float)(Portion_Start * 100.0)) != 0)
		return -1;
	if(Seconds) {
		if (SetValue("ui_progress_portion", (float)((Portion * 100.0) + Portion_Start)) != 0)
			return -1;
		if (SetValue("ui_progress_frames", Seconds * 48) != 0)
			return -1;
	}
	return 0;
}

void DataManager::update_tz_environment_variables(void)
{
	string TZ = GetStrValue(TW_TIME_ZONE_VAR);
	setenv("TZ", TZ.c_str(), 1);
	tzset();
	property_set("persist.sys.timezone", TZ.c_str());
}

void DataManager::SetBackupFolder()
{
	string str = GetCurrentStoragePath();
	TWPartition* partition = PartitionManager.Find_Partition_By_Path(str);
	str += TWFunc::Check_For_TwrpFolder() + "/BACKUPS/";

	string dev_id;
	GetValue("device_id", dev_id);

	str += dev_id;
	LOGINFO("Backup folder set to '%s'\n", str.c_str());
	SetValue(TW_BACKUPS_FOLDER_VAR, str, 0);
	if (partition != NULL) {
		SetValue("tw_storage_display_name", partition->Storage_Name);
		SetValue("tw_storage_free_size", UnitConversion::FormatBytes(partition->Free));
		string zip_path, zip_root, storage_path;
		GetValue(TW_ZIP_LOCATION_VAR, zip_path);
		if (partition->Has_Data_Media && !partition->Symlink_Mount_Point.empty())
			storage_path = partition->Symlink_Mount_Point;
		else
			storage_path = partition->Storage_Path;
		if (zip_path.size() < storage_path.size()) {
			SetValue(TW_ZIP_LOCATION_VAR, storage_path);
		} else {
			zip_root = TWFunc::Get_Root_Path(zip_path);
			if (zip_root != storage_path) {
				LOGINFO("DataManager::SetBackupFolder zip path was %s changing to %s, %s\n", zip_path.c_str(), storage_path.c_str(), zip_root.c_str());
				SetValue(TW_ZIP_LOCATION_VAR, storage_path);
			}
		}
	} else {
		if (PartitionManager.Fstab_Processed() != 0) {
			LOGINFO("Storage partition '%s' not found\n", str.c_str());
			gui_err("unable_locate_storage=Unable to locate storage device.");
		}
	}
}

// Recursively search root for a regular file named name, returning the first
// match (empty string if none). Symlinked directories are followed -- sysfs
// /sys/class/* entries are typically symlinks into /sys/devices -- with
// canonical-path de-dup to guard against sysfs symlink loops.
static string find_first_named_file(const string& name, const string& root) {
  namespace fs = std::filesystem;
  std::error_code ec;
  std::unordered_set<fs::path> visited;
  constexpr auto opts = fs::directory_options::follow_directory_symlink
                        | fs::directory_options::skip_permission_denied;
  for (auto it = fs::recursive_directory_iterator(root, opts, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) {
      ec.clear();
      continue;
    }
    const auto& entry = *it;
    if (entry.is_symlink()) {
      if (auto can = fs::canonical(entry.path(), ec); !ec && !visited.insert(can).second) {
        it.disable_recursion_pending(); // break a symlink loop
        continue;
      }
      ec.clear(); // reset after canonical(); dangling -> carry on
    }
    if (!entry.is_symlink() && entry.is_regular_file()
        && entry.path().filename() == name)
      return entry.path();
  }
  return "";
}

void DataManager::SetDefaultValues()
{
	string str, path;

	mConst.SetConst();

	get_device_id();

	pthread_mutex_lock(&m_valuesLock);

	mInitialized = 1;

	mConst.SetValue("true", "1");
	mConst.SetValue("false", "0");

	mConst.SetValue(TW_VERSION_VAR, TWFunc::Get_TWRP_Version_Str());

	// OrangeFox 构建标识常量。R11.3 在 SetDefaultValues 里紧随 TW_VERSION_VAR 发布这几项,
	// 移植时整段丢失,后果是:
	//   - about 页"维护者"一栏的条件 (of_maintainer != 1/2/3) 恒不成立 -> 名字与标签整块不显示
	//   - about 页 "{@version}: %fox_actual_build%" 渲染成空
	//   - 主题里所有 "fox_branch >= 10/11" 的 R11+ 专属选项(install.xml/advanced.xml)被隐藏
#ifdef OF_MAINTAINER
	mConst.SetValue(OF_MAINTAINER_STR, OF_MAINTAINER);
#endif
	mConst.SetValue(FOX_ACTUAL_BUILD_VAR, FOX_BUILD);
	mConst.SetValue(BUILD_TYPE_STR, FOX_BUILD_TYPE);
	mConst.SetValue("fox_branch", FOX_BRANCH);

#ifndef TW_NO_HAPTICS
    mPersist.SetValue("tw_button_vibrate", "80");
    mPersist.SetValue("tw_keyboard_vibrate", "40");
    mPersist.SetValue("tw_action_vibrate", "160");
    mConst.SetValue("tw_disable_haptics", "0");
#else
    LOGINFO("TW_NO_HAPTICS := true\n");
    mConst.SetValue("tw_disable_haptics", "1");
#endif

#ifdef TW_INCLUDE_WIFI
    mConst.SetValue("tw_disable_network", "0");
#else
    LOGINFO("TW_INCLUDE_WIFI is not enabled\n");
    mConst.SetValue("tw_disable_network", "1");
#endif

	TWPartition *store = PartitionManager.Get_Default_Storage_Partition();
	if (store)
		mPersist.SetValue("tw_storage_path", store->Storage_Path);
	else
		mPersist.SetValue("tw_storage_path", "/");

#ifdef TW_FORCE_CPUINFO_FOR_DEVICE_ID
	printf("TW_FORCE_CPUINFO_FOR_DEVICE_ID := true\n");
#endif

#ifdef BOARD_HAS_NO_REAL_SDCARD
	printf("BOARD_HAS_NO_REAL_SDCARD := true\n");
	mConst.SetValue(TW_ALLOW_PARTITION_SDCARD, "0");
#else
	mConst.SetValue(TW_ALLOW_PARTITION_SDCARD, "1");
#endif

	mData.SetValue(TW_RECOVERY_FOLDER_VAR, TW_DEFAULT_RECOVERY_FOLDER);

	str = GetCurrentStoragePath();
	mPersist.SetValue(TW_ZIP_LOCATION_VAR, str);
	str += DataManager::GetStrValue(TW_RECOVERY_FOLDER_VAR) + "/BACKUPS/";

	string dev_id;
	mConst.GetValue("device_id", dev_id);

	str += dev_id;
	mData.SetValue(TW_BACKUPS_FOLDER_VAR, str);

	mConst.SetValue(TW_REBOOT_SYSTEM, "1");
#ifdef TW_NO_REBOOT_RECOVERY
	printf("TW_NO_REBOOT_RECOVERY := true\n");
	mConst.SetValue(TW_REBOOT_RECOVERY, "0");
#else
	mConst.SetValue(TW_REBOOT_RECOVERY, "1");
#endif
	mConst.SetValue(TW_REBOOT_POWEROFF, "1");
#ifdef TW_NO_REBOOT_BOOTLOADER
	printf("TW_NO_REBOOT_BOOTLOADER := true\n");
	mConst.SetValue(TW_REBOOT_BOOTLOADER, "0");
#else
	mConst.SetValue(TW_REBOOT_BOOTLOADER, "1");
#endif
#ifdef RECOVERY_SDCARD_ON_DATA
	printf("RECOVERY_SDCARD_ON_DATA := true\n");
	mConst.SetValue(TW_HAS_DATA_MEDIA, "1");
	datamedia = true;
#else
	mData.SetValue(TW_HAS_DATA_MEDIA, "0");
#endif
#ifdef TW_NO_BATT_PERCENT
	printf("TW_NO_BATT_PERCENT := true\n");
	mConst.SetValue(TW_NO_BATTERY_PERCENT, "1");
#else
	mConst.SetValue(TW_NO_BATTERY_PERCENT, "0");
#endif
#ifdef TW_NO_CPU_TEMP
	printf("TW_NO_CPU_TEMP := true\n");
	mConst.SetValue("tw_no_cpu_temp", "1");
#else
	string cpu_temp_file;
#ifdef TW_CUSTOM_CPU_TEMP_PATH
	cpu_temp_file = EXPAND(TW_CUSTOM_CPU_TEMP_PATH);
#else
	cpu_temp_file = "/sys/class/thermal/thermal_zone0/temp";
#endif
	if (TWFunc::Path_Exists(cpu_temp_file)) {
		mConst.SetValue("tw_no_cpu_temp", "0");
	} else {
		LOGINFO("CPU temperature file '%s' not found, disabling CPU temp.\n", cpu_temp_file.c_str());
		mConst.SetValue("tw_no_cpu_temp", "1");
	}
#endif
#ifdef TW_CUSTOM_POWER_BUTTON
	printf("TW_POWER_BUTTON := %s\n", EXPAND(TW_CUSTOM_POWER_BUTTON));
	mConst.SetValue(TW_POWER_BUTTON, EXPAND(TW_CUSTOM_POWER_BUTTON));
#else
	mConst.SetValue(TW_POWER_BUTTON, "0");
#endif
#ifdef TW_ALWAYS_RMRF
	printf("TW_ALWAYS_RMRF := true\n");
	mConst.SetValue(TW_RM_RF_VAR, "1");
#endif
#ifdef TW_NEVER_UNMOUNT_SYSTEM
	printf("TW_NEVER_UNMOUNT_SYSTEM := true\n");
	mConst.SetValue(TW_DONT_UNMOUNT_SYSTEM, "1");
#else
	mConst.SetValue(TW_DONT_UNMOUNT_SYSTEM, "0");
#endif
#ifdef TW_NO_USB_STORAGE
	printf("TW_NO_USB_STORAGE := true\n");
	mConst.SetValue(TW_HAS_USB_STORAGE, "0");
#else
	char lun_file[255];
	string Lun_File_str = CUSTOM_LUN_FILE;
	size_t found = Lun_File_str.find("%");
	if (found != string::npos) {
		sprintf(lun_file, CUSTOM_LUN_FILE, 0);
		Lun_File_str = lun_file;
	}
	if (!TWFunc::Path_Exists(Lun_File_str)) {
		LOGINFO("Lun file '%s' does not exist, USB storage mode disabled\n", Lun_File_str.c_str());
		mConst.SetValue(TW_HAS_USB_STORAGE, "0");
	} else {
		LOGINFO("Lun file '%s'\n", Lun_File_str.c_str());
		mData.SetValue(TW_HAS_USB_STORAGE, "1");
	}
#endif
#ifdef TW_HAS_DOWNLOAD_MODE
	printf("TW_HAS_DOWNLOAD_MODE := true\n");
	mConst.SetValue(TW_DOWNLOAD_MODE, "1");
#endif
#ifdef TW_HAS_EDL_MODE
	printf("TW_HAS_EDL_MODE := true\n");
	mConst.SetValue(TW_EDL_MODE, "1");
#endif
#ifdef TW_INCLUDE_FASTBOOTD
	printf("TW_INCLUDE_FASTBOOTD := true\n");
	mConst.SetValue(TW_FASTBOOT_MODE, "1");
#endif
#ifdef PRODUCT_USE_DYNAMIC_PARTITIONS
	printf("PRODUCT_USE_DYNAMIC_PARTITIONS := true\n");
	mConst.SetValue(TW_FASTBOOT_MODE, "1");
	mConst.SetValue(TW_IS_SUPER, "1");
#else
	mConst.SetValue(TW_IS_SUPER, "0");
#endif
#ifdef TW_INCLUDE_CRYPTO
	mConst.SetValue(TW_HAS_CRYPTO, "1");
	printf("TW_INCLUDE_CRYPTO := true\n");
#endif
#ifdef TW_SDEXT_NO_EXT4
	printf("TW_SDEXT_NO_EXT4 := true\n");
	mConst.SetValue(TW_SDEXT_DISABLE_EXT4, "1");
#else
	mConst.SetValue(TW_SDEXT_DISABLE_EXT4, "0");
#endif

#ifdef TW_HAS_NO_BOOT_PARTITION
	mPersist.SetValue("tw_backup_list", "/system;/data;");
#else
#ifdef PRODUCT_USE_DYNAMIC_PARTITIONS
	mPersist.SetValue("tw_backup_list", "/data;");
#else
	mPersist.SetValue("tw_backup_list", "/system;/data;/boot;");
#endif
#endif
	mConst.SetValue(TW_MIN_SYSTEM_VAR, TW_MIN_SYSTEM_SIZE);
	mData.SetValue(TW_BACKUP_NAME, "(Auto Generate)");

	mPersist.SetValue(TW_INSTALL_REBOOT_VAR, "0");
	mPersist.SetValue(TW_SIGNED_ZIP_VERIFY_VAR, "0");
	mPersist.SetValue(TW_DISABLE_FREE_SPACE_VAR, "0");
	mPersist.SetValue(TW_FORCE_DIGEST_CHECK_VAR, "0");
	mPersist.SetValue(TW_USE_COMPRESSION_VAR, "0");
	mPersist.SetValue(TW_TIME_ZONE_VAR, "CST6CDT,M3.2.0,M11.1.0");
	mPersist.SetValue(TW_GUI_SORT_ORDER, "1");
	mPersist.SetValue(TW_RM_RF_VAR, "0");
	mPersist.SetValue(TW_SKIP_DIGEST_CHECK_VAR, "0");
	mPersist.SetValue(TW_SKIP_DIGEST_CHECK_ZIP_VAR, "1");
	mPersist.SetValue(TW_SKIP_DIGEST_GENERATE_VAR, "0");
	mPersist.SetValue(TW_SDEXT_SIZE, "0");
	mPersist.SetValue(TW_SWAP_SIZE, "0");
	mPersist.SetValue(TW_SDPART_FILE_SYSTEM, "ext3");
	mPersist.SetValue(TW_TIME_ZONE_GUISEL, "CST6;CDT,M3.2.0,M11.1.0");
	mPersist.SetValue(TW_TIME_ZONE_GUIOFFSET, "0");
	mPersist.SetValue(TW_TIME_ZONE_GUIDST, "0");
	mPersist.SetValue(TW_AUTO_REFLASHTWRP_VAR, "0");
#ifdef TW_NO_FLASH_CURRENT_TWRP
	mConst.SetValue("tw_no_flash_current_twrp", "1");
#else
	mConst.SetValue("tw_no_flash_current_twrp", "0");
#endif
	mPersist.SetValue(TW_AUTO_DISABLE_AVB2_VAR, "0");
	mData.SetValue(TW_ACTION_BUSY, "0");
	mData.SetValue("tw_wipe_cache", "0");
	mData.SetValue("tw_wipe_dalvik", "0");
	mData.SetValue(TW_ZIP_INDEX, "0");
	mData.SetValue(TW_ZIP_QUEUE_COUNT, "0");
	mData.SetValue(TW_FILENAME, "/sdcard");
	mData.SetValue(TW_SIMULATE_ACTIONS, "0");
	mData.SetValue(TW_SIMULATE_FAIL, "0");
	mData.SetValue(TW_IS_ENCRYPTED, "0");
	mData.SetValue(TW_IS_DECRYPTED, "0");
	mData.SetValue(TW_CRYPTO_PASSWORD, "0");
	mData.SetValue(TW_CRYPTO_PWTYPE, "0"); // Set initial value so that recovery will not be confused when using unencrypted data or failed to decrypt data
	mData.SetValue("tw_terminal_state", "0");
	mData.SetValue("tw_background_thread_running", "0");
	mData.SetValue(TW_RESTORE_FILE_DATE, "0");
	mPersist.SetValue("tw_military_time", "0");

#ifdef TW_INCLUDE_CRYPTO
	mPersist.SetValue(TW_USE_SHA2, "1");
	mPersist.SetValue(TW_NO_SHA2, "0");
#else
	mPersist.SetValue(TW_NO_SHA2, "1");
#endif
#ifdef AB_OTA_UPDATER
	mPersist.SetValue(TW_UNMOUNT_SYSTEM, "0");
#else
	mPersist.SetValue(TW_UNMOUNT_SYSTEM, "1");
#endif
#if defined BOARD_USES_RECOVERY_AS_BOOT && defined BOARD_BUILD_SYSTEM_ROOT_IMAGE
	mConst.SetValue("tw_uses_initramfs", "1");
#else
	mConst.SetValue("tw_uses_initramfs", "0");
#endif
#if defined BOARD_USES_RECOVERY_AS_BOOT || defined BOARD_MOVE_RECOVERY_RESOURCES_TO_VENDOR_BOOT
	mConst.SetValue("tw_include_install_recovery_ramdisk", "1");
#else
	mConst.SetValue("tw_include_install_recovery_ramdisk", "0");
#endif
#ifdef BOARD_MOVE_RECOVERY_RESOURCES_TO_VENDOR_BOOT
	mConst.SetValue("tw_is_vendor_boot", "1");
#else
	mConst.SetValue("tw_is_vendor_boot", "0");
#endif
#ifdef TW_NO_SCREEN_TIMEOUT
	mConst.SetValue("tw_screen_timeout_secs", "0");
	mConst.SetValue("tw_no_screen_timeout", "1");
#else
	mPersist.SetValue("tw_screen_timeout_secs", "60");
	mPersist.SetValue("tw_no_screen_timeout", "0");
#endif
#ifdef BOARD_BOOT_HEADER_VERSION
	mConst.SetValue("tw_boot_header_version", BOARD_BOOT_HEADER_VERSION);
#endif

	if (GetIntValue("tw_is_vendor_boot") == 1 && GetIntValue("tw_boot_header_version") == 3)
		mConst.SetValue("tw_is_vendor_boot_header_v3", "1");
	else
		mConst.SetValue("tw_is_vendor_boot_header_v3", "0");

	mData.SetValue("tw_gui_done", "0");
	mData.SetValue("tw_encrypt_backup", "0");
	mData.SetValue("tw_sleep_total", "5");
	mData.SetValue("tw_sleep", "5");
	mData.SetValue("tw_enable_fastboot", "0");

	if (android::base::GetBoolProperty("ro.virtual_ab.enabled", false))
		mConst.SetValue(TW_VIRTUAL_AB_ENABLED, "1");
	else
		mConst.SetValue(TW_VIRTUAL_AB_ENABLED, "0");

	HandleBrightnessConfig();

#ifdef TW_HAS_MTP
	mConst.SetValue("tw_has_mtp", "1");
	mPersist.SetValue("tw_mtp_enabled", "1");
	mPersist.SetValue("tw_mtp_debug", "0");
#else
	LOGINFO("TW_EXCLUDE_MTP := true\n");
	mConst.SetValue("tw_has_mtp", "0");
	mConst.SetValue("tw_mtp_enabled", "0");
#endif
	mPersist.SetValue("tw_mount_system_ro", "2");
	mPersist.SetValue("tw_never_show_system_ro_page", "0");
	mPersist.SetValue("tw_language", EXPAND(TW_DEFAULT_LANGUAGE));
	LOGINFO("LANG: %s\n", EXPAND(TW_DEFAULT_LANGUAGE));

	mData.SetValue("tw_has_adopted_storage", "0");

#ifdef AB_OTA_UPDATER
	LOGINFO("AB_OTA_UPDATER := true\n");
	mConst.SetValue("tw_has_boot_slots", "1");
#else
	mConst.SetValue("tw_has_boot_slots", "0");
#endif

#ifndef TW_EXCLUDE_NANO
	mConst.SetValue("tw_include_nano", "1");
#else
	LOGINFO("TW_EXCLUDE_NANO := true\n");
	mConst.SetValue("tw_include_nano", "0");
#endif

	mData.SetValue("tw_flash_both_slots", "0");
	mData.SetValue("tw_is_slot_part", "0");

	mData.SetValue("tw_enable_adb_backup", "0");

	if (TWFunc::Path_Exists("/system/bin/logcat"))
		mConst.SetValue("tw_logcat_exists", "1");
	else
		mConst.SetValue("tw_logcat_exists", "0");

	if (TWFunc::Path_Exists("/system/bin/magiskboot"))
		mConst.SetValue("tw_has_repack_tools", "1");
	else
		mConst.SetValue("tw_has_repack_tools", "0");

	// ===================== OrangeFox: publish the OF_*/FOX_* build vars =====================
	// 说明:以下 mConst/mData/mPersist 键是 OrangeFox 主题(gui/theme/portrait_hdpi)在运行时
	// 用 %key% 取值的来源。编译期注入(-DOF_SCREEN_H=... 等)只把值带进二进制,
	// 必须在这里写进 DataManager,主题才拿得到。缺了这一段,主题里
	// %screen_original_h% / %status_h% / %center_y% / %cutout_w%(=%status_h%-72)
	// 等占位符全部解不出来,布局坐标会整体错位(画面跑到屏幕外)。
	// 对应上游 OrangeFox data.cpp 中 mConst.SetValue(OF_SCREEN_H_S, OF_SCREEN_H) 那一段。

	// GUI 用到的路径 / 环境变量
	mConst.SetValue("fox_home_path", Fox_Home);
	mConst.SetValue("fox_settings_path", Fox_Settings_Path);
	mConst.SetValue("fox_home_files", Fox_Home_Files);
	mConst.SetValue("fox_theme_path", FOX_THEME_PATH);
	mConst.SetValue("fox_media_rw", FOX_MEDIA_RW);
	mConst.SetValue("fox_media_rw_data_file", FOX_MEDIA_RW_DATA_FILE);
	mConst.SetValue("fox_navbar_path", FOX_NAVBAR_PATH);
	mConst.SetValue("fox_ota_path", FOX_OTA_PATH);
	mConst.SetValue("aroma_fm_zip", Fox_Home_Files + "/AromaFM/AromaFM.zip");
#ifndef FOX_DELETE_INITD_ADDON
	mConst.SetValue("of_initd_zip", Fox_Home_Files + "/OF_initd.zip");
#endif

	mData.SetValue("fox_startup_executed", "0");

	if (TWFunc::Has_Virtual_AB_Partitions())
		mConst.SetValue("fox_vab_device", "1");
	else
		mConst.SetValue("fox_vab_device", "0");

#ifdef OF_SUPPORT_OZIP_DECRYPTION
	mConst.SetValue("of_support_ozip_decryption", "1");
#endif

	// magiskboot 24+ 是否强制修补 vbmeta
#if defined(FOX_PATCH_VBMETA_FLAG)
	setenv("PATCHVBMETAFLAG", "true", 1);
#else
	setenv("PATCHVBMETAFLAG", "false", 1);
#endif

	mPersist.SetValue("of_average_img", "42");
	mPersist.SetValue("of_average_file", "30");
	mPersist.SetValue("of_average_ext_img", "15");
	mPersist.SetValue("of_average_ext_file", "10");
	mPersist.SetValue("of_keep_storage_data", "1");	// 恢复内置存储备份时是否保留已有文件

	// ---- [f/d] UI Vars:屏幕几何(渲染错位的直接原因就在这几行) ----
#ifdef FOX_USE_NANO_EDITOR
	mConst.SetValue("fox_use_nano_editor", "1");
#else
	mConst.SetValue("fox_use_nano_editor", "0");
#endif

	int of_status_placement = (atoi(OF_STATUS_H) / 2) - 28;
	int of_center_y = atoi(OF_SCREEN_H) / 2;

	mConst.SetValue(OF_STATUS_PLACEMENT_S, of_status_placement);
	mConst.SetValue(OF_CENTER_Y_S, of_center_y);

	mConst.SetValue(OF_SCREEN_H_S, OF_SCREEN_H);
	mData.SetValue(OF_SCREEN_NAV_H_S, OF_SCREEN_H);	// mData:navbar 运行时用

	mConst.SetValue(OF_STATUS_H_S, OF_STATUS_H);
	mConst.SetValue(OF_HIDE_NOTCH_S, OF_HIDE_NOTCH);
	mConst.SetValue(OF_STATUS_INDENT_LEFT_S, OF_STATUS_INDENT_LEFT);
	mConst.SetValue(OF_STATUS_INDENT_RIGHT_S, OF_STATUS_INDENT_RIGHT);
	mConst.SetValue(OF_CLOCK_POS_S, OF_CLOCK_POS);
	mConst.SetValue(OF_ALLOW_DISABLE_NAVBAR_S, OF_ALLOW_DISABLE_NAVBAR);
	mConst.SetValue(OF_FLASHLIGHT_ENABLE_STR, OF_FLASHLIGHT_ENABLE);
	mConst.SetValue(OF_SPLASH_MAX_SIZE_STR, OF_SPLASH_MAX_SIZE);

	// 部分列表框滚动前显示多少项
	int lnum = 360;
	int lnum2 = 540;
	int lnum_group = 512;
#ifdef OF_OPTIONS_LIST_NUM
	int cv = atoi(OF_OPTIONS_LIST_NUM);
	const int min_h = 4;
	const int max_h =
#ifdef FOX_AB_DEVICE
	9;
#else
	12;
#endif

	if (cv < min_h)
		cv = min_h;
	else if (cv > max_h)
		cv = max_h;

	lnum = (cv * 90);
	if (lnum > lnum2)
		lnum2 = lnum;

	lnum_group = (cv * 144 + (144 * 2));
#endif
	mConst.SetValue("options_list_num", lnum);
	mConst.SetValue("options_list_num_2", lnum2);
	mConst.SetValue("options_list_num_group", lnum_group);

#ifdef OF_ENABLE_LAB
	mConst.SetValue("fox_lab", "1");
	LOGERR("Warning: lab enabled\n");
	LOGERR("Build isn't for release\n");
#else
	mConst.SetValue("fox_lab", "0");
#endif

#ifdef OF_FLASHLIGHT_ENABLE
	if ((string)OF_FLASHLIGHT_ENABLE == "1") {
		mConst.SetValue("of_fl_path_1", OF_FL_PATH1);
		mConst.SetValue("of_fl_path_2", OF_FL_PATH2);
		mData.SetValue("of_flash_on", "0");
	}
#endif

	mConst.SetValue("fox_build_type1", FOX_BUILD_TYPE);
	mConst.SetValue("fox_show_digest_btn", "0");

#if defined(OF_DISABLE_MIUI_SPECIFIC_FEATURES)
	mData.SetValue("of_no_miui_features", "1");
#else
	mData.SetValue("of_no_miui_features", "0");
#endif

#if defined(OF_NO_REFLASH_CURRENT_ORANGEFOX)
	mConst.SetValue("fox_disable_reflash_current", "1");
#else
	mConst.SetValue("fox_disable_reflash_current", "0");
#endif

#if defined(FOX_AB_DEVICE) || defined(AB_OTA_UPDATER)
	mData.SetValue("of_ab_device", "1");
#else
	mData.SetValue("of_ab_device", "0");
#endif

	// 注:tw_include_install_recovery_ramdisk / tw_is_vendor_boot 本文件上方已设置,此处不重复。

#ifdef FOX_ENABLE_APP_MANAGER
	mConst.SetValue("enable_app_manager", "1");
#endif

#ifdef OF_DISABLE_EXTRA_ABOUT_PAGE
	mConst.SetValue("disable_extra_about", "1");
#endif

#ifdef OF_DISABLE_OTA_MENU
	mConst.SetValue("of_no_ota_menu", "1");
#ifdef OF_DISABLE_ORS_AUTO_REBOOT
	mConst.SetValue(FOX_DISABLE_OTA_AUTO_REBOOT, "1");
#else
	mConst.SetValue(FOX_DISABLE_OTA_AUTO_REBOOT, "0");
#endif
#else
	mConst.SetValue("of_no_ota_menu", "0");
	mPersist.SetValue(FOX_DISABLE_OTA_AUTO_REBOOT, "0");
#endif

#ifdef OF_NO_SPLASH_CHANGE
	mConst.SetValue("no_splash_change", "1");
#else
	mConst.SetValue("no_splash_change", "0");
#endif

#ifdef FOX_DELETE_MAGISK_ADDON
	mConst.SetValue("no_magisk", "1");
#endif

// 只有关掉时才定义
#ifdef OF_NO_GREEN_LED
	mConst.SetValue("no_green_led", "1");
#endif

	mData.SetValue("of_reload_back", "main");
	// =================== end OrangeFox UI vars ===================

	pthread_mutex_unlock(&m_valuesLock);
}

void DataManager::HandleBrightnessConfig() {
	std::string brightness_path;
#ifdef TW_BRIGHTNESS_PATH
	brightness_path = TW_BRIGHTNESS_PATH;
	LOGINFO("TW_BRIGHTNESS_PATH := %s\n", TW_BRIGHTNESS_PATH);
	if (!TWFunc::Path_Exists(TW_BRIGHTNESS_PATH)) {
		LOGINFO("Specified brightness file '%s' not found.\n", TW_BRIGHTNESS_PATH);
		brightness_path.clear();
	}
#endif

	// Attempt to locate the brightness file
	if (brightness_path.empty()) {
		brightness_path = find_first_named_file("brightness", "/sys/class/backlight");
		if (brightness_path.empty())
			brightness_path = find_first_named_file("brightness", "/sys/class/leds/lcd-backlight");
	}
	if (brightness_path.empty()) {
		LOGINFO("Unable to locate brightness file\n");
		mConst.SetValue("tw_has_brightnesss_file", "0");
		return;
	}

	LOGINFO("Found brightness file at '%s'\n", brightness_path.c_str());
	mConst.SetValue("tw_has_brightnesss_file", "1");
	mConst.SetValue("tw_brightness_file", brightness_path);

	int max_brightness;
#ifdef TW_MAX_BRIGHTNESS
	max_brightness = TW_MAX_BRIGHTNESS;
#else
	// Derive the sibling max_brightness path without mutating brightness_path.
	const std::filesystem::path bpath(brightness_path);
	const std::string max_brightness_path = bpath.parent_path() / std::format("max_{}", bpath.filename());
	if (TWFunc::Path_Exists(max_brightness_path)) {
		if (android::base::ReadFileToString(max_brightness_path, &max_brightness)) {
			LOGINFO("Got max brightness %s from '%s'\n", max_brightness.c_str(), max_brightness_path.c_str());
		} else {
			// Something went wrong, set that to indicate error
			max_brightness = -1;
		}
	}
	// Fallback into default
	if (max_brightness <= 0) max_brightness = 255;
#endif
	mConst.SetValue("tw_brightness_max", max_brightness);
	mPersist.SetValue("tw_brightness", max_brightness / 5);
	mPersist.SetValue("tw_brightness_pct", "20");

#ifdef TW_SECONDARY_BRIGHTNESS_PATH
	std::string second_brightness_path = EXPAND(TW_SECONDARY_BRIGHTNESS_PATH);
	if (!second_brightness_path.empty() && TWFunc::Path_Exists(second_brightness_path)) {
		LOGINFO("Will use a second brightness file at '%s'\n", second_brightness_path.c_str());
		mConst.SetValue("tw_secondary_brightness_file", second_brightness_path);
	} else {
		LOGINFO("Specified secondary brightness file '%s' not found.\n", second_brightness_path.c_str());
	}
#endif

#ifdef TW_DEFAULT_BRIGHTNESS
	const int defPctInt = static_cast<double>(TW_DEFAULT_BRIGHTNESS) / max_brightness * 100;
	mPersist.SetValue("tw_brightness_pct", std::to_string(defPctInt));
	mPersist.SetValue("tw_brightness", TW_DEFAULT_BRIGHTNESS);
	TWFunc::Set_Brightness(std::to_string(TW_DEFAULT_BRIGHTNESS));
#else
	TWFunc::Set_Brightness(std::to_string(max_brightness / 5));
#endif
}

// Magic Values
int DataManager::GetMagicValue(const string& varName, string& value)
{
	// Handle special dynamic cases
	if (varName == "tw_time")
	{
		char tmp[32];

		struct tm *current;
		time_t now;
		int tw_military_time;
		now = time(0);
		current = localtime(&now);
		GetValue(TW_MILITARY_TIME, tw_military_time);
		if (current->tm_hour >= 12)
		{
			if (tw_military_time == 1)
				sprintf(tmp, "%d:%02d", current->tm_hour, current->tm_min);
			else
				sprintf(tmp, "%d:%02d PM", current->tm_hour == 12 ? 12 : current->tm_hour - 12, current->tm_min);
		}
		else
		{
			if (tw_military_time == 1)
				sprintf(tmp, "%d:%02d", current->tm_hour, current->tm_min);
			else
				sprintf(tmp, "%d:%02d AM", current->tm_hour == 0 ? 12 : current->tm_hour, current->tm_min);
		}
		value = tmp;
		return 0;
	}
	else if (varName == "tw_cpu_temp")
	{
		int tw_no_cpu_temp;
		GetValue("tw_no_cpu_temp", tw_no_cpu_temp);
		if (tw_no_cpu_temp == 1) return -1;

		string cpu_temp_file;
		static unsigned long convert_temp = 0;
		static time_t cpuSecCheck = 0;
		struct timeval curTime;
		string results;

		gettimeofday(&curTime, NULL);
		if (curTime.tv_sec > cpuSecCheck)
		{
#ifdef TW_CUSTOM_CPU_TEMP_PATH
			cpu_temp_file = EXPAND(TW_CUSTOM_CPU_TEMP_PATH);
			if (TWFunc::read_file(cpu_temp_file, results) != 0)
				return -1;
#else
			cpu_temp_file = "/sys/class/thermal/thermal_zone0/temp";
			if (TWFunc::read_file(cpu_temp_file, results) != 0)
				return -1;
#endif
			convert_temp = strtoul(results.c_str(), NULL, 0) / 1000;
			if (convert_temp <= 0)
				convert_temp = strtoul(results.c_str(), NULL, 0);
			if (convert_temp >= 150)
				convert_temp = strtoul(results.c_str(), NULL, 0) / 10;
			cpuSecCheck = curTime.tv_sec + 5;
		}
		value = TWFunc::to_string(convert_temp);
		return 0;
	}
	return -1;
}

void DataManager::Output_Version(void)
{
	char version[255];

	std::string logDir = TWFunc::get_log_dir();
	if (logDir.empty()) {
		LOGINFO("Unable to find cache directory\n");
		return;
	}

	std::string recoveryLogDir = logDir + "recovery/";

	if (logDir == CACHE_LOGS_DIR) {
		if (!PartitionManager.Mount_By_Path(CACHE_LOGS_DIR, false) && !TWFunc::Path_Exists(CACHE_LOGS_DIR)) {
			LOGINFO("Unable to mount '%s' to write version number.\n", CACHE_LOGS_DIR);
			return;
		}

		if (!TWFunc::Path_Exists(recoveryLogDir)) {
			LOGINFO("Recreating %s folder.\n", recoveryLogDir.c_str());
			if (!TWFunc::Create_Dir_Recursive(recoveryLogDir.c_str(), S_IRWXU | S_IRWXG | S_IWGRP | S_IXGRP, 0, 0)) {
				LOGERR("DataManager::Output_Version -- Unable to make %s: %s\n", recoveryLogDir.c_str(), strerror(errno));
				return;
			}
		}
	}

	std::string verPath = recoveryLogDir + ".version";
	if (TWFunc::Path_Exists(verPath)) {
		unlink(verPath.c_str());
	}
	FILE *fp = fopen(verPath.c_str(), "w");
	if (fp == NULL) {
		LOGINFO("Unable to open: %s. Data may be unmounted. Error: %s\n", verPath.c_str(), strerror(errno));
		return;
	}
    {
        std::string ver = TWFunc::Get_TWRP_Version_Str();
        strncpy(version, ver.c_str(), sizeof(version) - 1);
        version[sizeof(version) - 1] = '\0';
    }
	fwrite(version, sizeof(version[0]), strlen(version) / sizeof(version[0]), fp);
	fclose(fp);
	TWFunc::copy_file("/etc/recovery.fstab", recoveryLogDir + "recovery.fstab", 0644);
	PartitionManager.Output_Storage_Fstab();
	sync();
	LOGINFO("Version number saved to '%s'\n", verPath.c_str());
}

void DataManager::ReadSettingsFile(void)
{
	// Load up the values for TWRP - Sleep to let the card be ready
	char mkdir_path[255], settings_file[255];
	int is_enc, has_data_media;

	GetValue(TW_IS_ENCRYPTED, is_enc);
	GetValue(TW_HAS_DATA_MEDIA, has_data_media);

	// 与 OrangeFox 对齐:设置文件取自 GetSettingsStoragePath()(= /data/recovery/Fox)
	// 而不是 persist 分区里 TWRP 的旧位置。原来这里硬编码 TW_PERSIST_DIR,
	// 导致 mBackingFile 指向 /mnt/vendor/persist/TWRP/.twrp_settings,
	// 于是设置被读写到 persist 的 TWRP 目录,而不是 Fox 目录。
	memset(mkdir_path, 0, sizeof(mkdir_path));
	memset(settings_file, 0, sizeof(settings_file));
	sprintf(mkdir_path, "%s", GetSettingsStoragePath().c_str());
	sprintf(settings_file, "%s/%s", mkdir_path, TW_SETTINGS_FILE);

	if (!PartitionManager.Mount_Settings_Storage(false))
	{
		usleep(500000);
		if (!PartitionManager.Mount_Settings_Storage(false))
			gui_msg(Msg(msg::kError, "unable_to_mount=Unable to mount {1}")(settings_file));
	}

	LOGINFO("Attempt to load settings from settings file...\n");
	LoadValues(settings_file);
	Output_Version();
	PartitionManager.Mount_All_Storage();
	update_tz_environment_variables();
	TWFunc::Set_Brightness(GetStrValue("tw_brightness"));

	DataManager::FindPasswordBackup();
	DataManager::RestorePasswordBackup();
}

string DataManager::GetCurrentStoragePath(void)
{
	return GetStrValue("tw_storage_path");
}

string DataManager::GetSettingsStoragePath(void)
{
	// 与 OrangeFox 对齐:定义了 FOX_SETTINGS_ROOT_DIRECTORY 时,设置目录固定为
	// Fox_Settings_Path(=/data/recovery/Fox),不再跟随 tw_settings_path 漂移。
#ifdef FOX_SETTINGS_ROOT_DIRECTORY
	return Fox_Settings_Path;
#else
	return GetStrValue("tw_settings_path");
#endif
}

string DataManager::GetCurrentPartPath(void)
{
  return GetStrValue("part_option");
}

void DataManager::Vibrate(const string& varName)
{
#ifndef TW_NO_HAPTICS
	int vib_value = 0;
	GetValue(varName, vib_value);
	if (vib_value) {
		vibrate(vib_value);
	}
#endif
}

void DataManager::Leds(bool enable)
{
  std::string leds, bs, bsmax, time, blink, bsm, leds1, bs1, bsmax1, time1, blink1, bsm1, max_brt, install_vibrate_value;
  struct stat st;
  int ledcolor;
  leds = "/sys/class/leds/green";
  bs = leds + "/brightness";
  time = leds + "/led_time";
  blink = leds + "/blink";
  bsmax = leds + "/max_brightness";

  leds1 = "/sys/class/leds/red";
  bs1 = leds1 + "/brightness";
  time1 = leds1 + "/led_time";
  blink1 = leds1 + "/blink";
  bsmax1 = leds1 + "/max_brightness";

  DataManager::GetValue("tw_action_vibrate", install_vibrate_value);
  DataManager::GetValue("fox_led_color", ledcolor);

  if (!enable && stat(bs.c_str(), &st) == 0)
    {
      TWFunc::write_to_file(bs, "0");
      TWFunc::write_to_file(bs1, "0");
      if (TWFunc::Path_Exists("/sys/class/leds/white/brightness"))
      {
        LOGINFO("DEBUG - found white led on /sys/class/leds/white/ path\n");
        TWFunc::write_to_file("/sys/class/leds/white/brightness", "0");
      }
    }
  else
    {
      if (stat(bs.c_str(), &st) == 0 && stat(bsmax.c_str(), &st) == 0) {
        if (stat(time.c_str(), &st) == 0 && stat(blink.c_str(), &st) == 0)
        {
          if (TWFunc::read_file(bsmax, bsm) == 0)
            {
              TWFunc::write_to_file(bs, bsm);
              TWFunc::write_to_file(blink, "1");
              TWFunc::write_to_file(time, "1 1 1 1");

              if (ledcolor == 0) {
                LOGINFO("Enable Yellow led\n");
                TWFunc::write_to_file("/sys/class/leds/red/brightness", bsm);
                TWFunc::write_to_file("/sys/class/leds/red/blink", "1");
                TWFunc::write_to_file("/sys/class/leds/red/led_time", "1 1 1 1");
              }
              if (TWFunc::Path_Exists("/sys/class/leds/white/brightness"))
              {
                LOGINFO("DEBUG - found white led on /sys/class/leds/white/ path\n");
                TWFunc::read_file("/sys/class/leds/white/max_brightness", max_brt);
                TWFunc::write_to_file("/sys/class/leds/white/brightness", max_brt);
              }
            }
        } else {
        //[f/d] Just turn on led if device doesn't support blinking
          if (TWFunc::read_file(bsmax, bsm) == 0)
          {
            TWFunc::write_to_file(bs, bsm);

            if (ledcolor == 0) {
              TWFunc::write_to_file("/sys/class/leds/red/brightness", bsm);
            }
          }
        }
      }
    }
}

void DataManager::LoadTWRPFolderInfo(void)
{
	SetValue(TW_RECOVERY_FOLDER_VAR, TWFunc::Check_For_TwrpFolder());
	// 与 OrangeFox 对齐:这里原来把 mBackingFile 覆盖成
	//   TW_PERSIST_DIR "/" TW_SETTINGS_FILE  → /mnt/vendor/persist/TWRP/.twrp_settings
	// 但它已经被 ReadSettingsFile() → LoadValues() 正确设成
	//   GetSettingsStoragePath() "/" TW_SETTINGS_FILE → /data/recovery/Fox/.foxs
	// 这行覆盖就是"配置跑到 TWRP 目录"的最后一刀,故删除。
	// (上游 OrangeFox 没有本函数;本树保留它只是为了设置 TW_RECOVERY_FOLDER_VAR。)
}
