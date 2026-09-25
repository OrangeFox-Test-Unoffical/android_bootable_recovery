/*
	Copyright 2012-2020 TeamWin
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

#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <unistd.h>
#include <vector>
#include <dirent.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <fstab/fstab.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cctype>
#include <algorithm>
#include <selinux/label.h>
#include <thread>

#include <android-base/strings.h>
#include <android-base/chrono_utils.h>
#include <android-base/properties.h>

#include "twrp-functions.hpp"
#include "oaes/oaes.hpp"
#include "abx.hpp"
#include "twcommon.h"
#include "gui/gui.hpp"
#include <fs_mgr_priv.h>
#ifndef BUILD_TWRPTAR_MAIN
#include "data.hpp"
#include "partitions.hpp"
#include "variables.h"
#include "bootloader_message/include/bootloader_message/bootloader_message.h"
#include "cutils/properties.h"
#include "cutils/android_reboot.h"
#include <sys/reboot.h>
#include "gui/rapidxml.hpp"
#include "gui/pages.hpp"
#endif // ndef BUILD_TWRPTAR_MAIN
#include "set_metadata.h"

// ---- OrangeFox(OFRP)移植所需 ----
#include "orangefox.hpp"
#include "abx-functions.hpp"
#include "twinstall.h"
#include <private/android_filesystem_config.h>
#include <locale>
#include <codecvt>

#ifdef TW_INCLUDE_LIBRESETPROP
    #include <resetprop.hpp>
#endif

struct selabel_handle *selinux_handle;

std::string TWFunc::Get_TWRP_Version_Str() {
    std::string dev = android::base::GetProperty("ro.twrp.device_version", "");
    if (!dev.empty()) return std::string(TW_MAIN_VERSION_STR) + std::string("-") + dev;
    return std::string(TW_VERSION_STR);
}

/* Execute a command */
// OrangeFox 移植:fox_14.1 的 2 参版本(等价于 combine_stderr=false)
int TWFunc::Exec_Cmd(const string& cmd, string &result) {
	return TWFunc::Exec_Cmd(cmd, result, false);
}

int TWFunc::Exec_Cmd(const string& cmd, string &result, bool combine_stderr) {
	FILE* exec;
	char buffer[130];
	int ret = 0;
	std::string popen_cmd = cmd;
	if (combine_stderr)
		popen_cmd = cmd + " 2>&1";
	exec = popen(popen_cmd.c_str(), "r");
	if (exec == nullptr) {
		LOGERR("Exec_Cmd(): failed to execute command: %s\n", cmd.c_str());
		return -1;
	}

	while (fgets(buffer, sizeof(buffer), exec) != nullptr) {
		result += buffer;
	}
	ret = pclose(exec);
	return ret;
}

int TWFunc::Exec_Cmd(const string& cmd, bool Show_Errors) {
	pid_t pid;
	int status;
	switch(pid = fork())
	{
		case -1:
			LOGERR("Exec_Cmd(): vfork failed: %d!\n", errno);
			return -1;
		case 0: // child
			execl("/system/bin/sh", "sh", "-c", cmd.c_str(), NULL);
			_exit(127);
			break;
		default:
		{
			if (TWFunc::Wait_For_Child(pid, &status, cmd, Show_Errors) != 0)
				return -1;
			else
				return 0;
		}
	}
}

// Returns "file.name" from a full /path/to/file.name
string TWFunc::Get_Filename(const string& Path) {
	size_t pos = Path.find_last_of("/");
	if (pos != string::npos) {
		string Filename;
		Filename = Path.substr(pos + 1, Path.size() - pos - 1);
		return Filename;
	} else
		return Path;
}

// Returns "/path/to/" from a full /path/to/file.name
string TWFunc::Get_Path(const string& Path) {
	size_t pos = Path.find_last_of("/");
	if (pos != string::npos) {
		string Pathonly;
		Pathonly = Path.substr(0, pos + 1);
		return Pathonly;
	} else
		return Path;
}

int TWFunc::Wait_For_Child(pid_t pid, int *status, string Child_Name, bool Show_Errors) {
	pid_t rc_pid;

	rc_pid = waitpid(pid, status, 0);
	if (rc_pid > 0) {
		if (WIFSIGNALED(*status)) {
			if (Show_Errors)
				gui_msg(Msg(msg::kError, "pid_signal={1} process ended with signal: {2}")(Child_Name)(WTERMSIG(*status))); // Seg fault or some other non-graceful termination
			return -1;
		} else if (WEXITSTATUS(*status) == 0) {
			LOGINFO("%s process ended with RC=%d\n", Child_Name.c_str(), WEXITSTATUS(*status)); // Success
		} else {
			if (Show_Errors)
				gui_msg(Msg(msg::kError, "pid_error={1} process ended with ERROR: {2}")(Child_Name)(WEXITSTATUS(*status))); // Graceful exit, but there was an error
			return -1;
		}
	} else { // no PID returned
		if (errno == ECHILD)
			LOGERR("%s no child process exist\n", Child_Name.c_str());
		else {
			LOGERR("%s Unexpected error %d\n", Child_Name.c_str(), errno);
			return -1;
		}
	}
	return 0;
}

int TWFunc::Wait_For_Child_Timeout(pid_t pid, int *status, const string& Child_Name, int timeout) {
	pid_t retpid = waitpid(pid, status, WNOHANG);
	for (; retpid == 0 && timeout; --timeout) {
		sleep(1);
		retpid = waitpid(pid, status, WNOHANG);
	}
	if (retpid == 0 && timeout == 0) {
		LOGERR("%s took too long, killing process\n", Child_Name.c_str());
		kill(pid, SIGKILL);
		for (timeout = 5; retpid == 0 && timeout; --timeout) {
			sleep(1);
			retpid = waitpid(pid, status, WNOHANG);
		}
		if (retpid)
			LOGINFO("Child process killed successfully\n");
		else
			LOGINFO("Child process took too long to kill, may be a zombie process\n");
		return -1;
	} else if (retpid > 0) {
		if (WIFSIGNALED(*status)) {
			gui_msg(Msg(msg::kError, "pid_signal={1} process ended with signal: {2}")(Child_Name)(WTERMSIG(*status))); // Seg fault or some other non-graceful termination
			return -1;
		}
	} else if (retpid < 0) { // no PID returned
		if (errno == ECHILD)
			LOGERR("%s no child process exist\n", Child_Name.c_str());
		else {
			LOGERR("%s Unexpected error %d\n", Child_Name.c_str(), errno);
			return -1;
		}
	}
	return 0;
}

bool TWFunc::Path_Exists(string Path) {
	struct stat st;
	return stat(Path.c_str(), &st) == 0;
}

void TWFunc::killForUseTargetProcess(const string& target) {
	if (!target.empty()) {
		char cmdBuf[256] = {0};
		snprintf(
			cmdBuf,
			sizeof(cmdBuf),
			"lsof | awk '{print($1,$9,$2)}' | grep -v '^recovery ' | awk '{print($2,$3)}' | grep '^%s' | awk '{print $2}' | sort | uniq | xargs kill -9 > /dev/null 2>&1",
			target.c_str()
		);
		string ret;
		Exec_Cmd(cmdBuf, ret, false);
	}
}

Archive_Type TWFunc::Get_File_Type(string fn) {
	string::size_type i = 0;
	int firstbyte = 0, secondbyte = 0;
	char header[3];

	ifstream f;
	f.open(fn.c_str(), ios::in | ios::binary);
	f.get(header, 3);
	f.close();
	firstbyte = header[i] & 0xff;
	secondbyte = header[++i] & 0xff;

	if (firstbyte == 0x1f && secondbyte == 0x8b)
		return COMPRESSED;
	else if (firstbyte == 0x4f && secondbyte == 0x41)
		return ENCRYPTED;
	return UNCOMPRESSED; // default
}

int TWFunc::Try_Decrypting_File(string fn, string password) {
	return Oaes::TryDecryptingFile(fn, password);
}

void TWFunc::AES_Encrypt_Stream(const string& password) {
	Oaes::EncryptStream(password);
}

void TWFunc::AES_Decrypt_Stream(const string& password) {
	Oaes::DecryptStream(password);
}

unsigned long TWFunc::Get_File_Size(const string& Path) {
	struct stat st;

	if (stat(Path.c_str(), &st) != 0)
		return 0;
	return st.st_size;
}

std::string TWFunc::Remove_Beginning_Slash(const std::string& path) {
	std::string res;
	size_t pos = path.find_first_of("/");
	if (pos != std::string::npos) {
		res = path.substr(pos+1);
	}
	return res;
}

std::string TWFunc::Remove_Trailing_Slashes(const std::string& path, bool leaveLast)
{
	std::string res;
	size_t last_idx = 0, idx = 0;

	while (last_idx != std::string::npos)
	{
		if (last_idx != 0)
			res += '/';

		idx = path.find_first_of('/', last_idx);
		if (idx == std::string::npos) {
			res += path.substr(last_idx, idx);
			break;
		}

		res += path.substr(last_idx, idx-last_idx);
		last_idx = path.find_first_not_of('/', idx);
	}

	if (leaveLast)
		res += '/';
	return res;
}

void TWFunc::Strip_Quotes(char* &str) {
	if (strlen(str) > 0 && str[0] == '\"')
		str++;
	if (strlen(str) > 0 && str[strlen(str)-1] == '\"')
		str[strlen(str)-1] = 0;
}

vector<string> TWFunc::split_string(const string &in, char del, bool skip_empty) {
	vector<string> res;

	if (in.empty() || del == '\0')
		return res;

	string field;
	istringstream f(in);
	if (del == '\n') {
		while (getline(f, field)) {
			if (field.empty() && skip_empty)
				continue;
			res.push_back(field);
		}
	} else {
		while (getline(f, field, del)) {
			if (field.empty() && skip_empty)
				continue;
			res.push_back(field);
		}
	}
	return res;
}

timespec TWFunc::timespec_diff(timespec& start, timespec& end)
{
	timespec temp;
	if ((end.tv_nsec-start.tv_nsec)<0) {
		temp.tv_sec = end.tv_sec-start.tv_sec-1;
		temp.tv_nsec = 1000000000+end.tv_nsec-start.tv_nsec;
	} else {
		temp.tv_sec = end.tv_sec-start.tv_sec;
		temp.tv_nsec = end.tv_nsec-start.tv_nsec;
	}
	return temp;
}

int32_t TWFunc::timespec_diff_ms(timespec& start, timespec& end)
{
	return ((end.tv_sec * 1000) + end.tv_nsec/1000000) -
			((start.tv_sec * 1000) + start.tv_nsec/1000000);
}

bool TWFunc::Wait_For_File(const string& path, std::chrono::nanoseconds timeout) {
    android::base::Timer t;
    while (t.duration() < timeout) {
        struct stat sb;
        if (stat(path.c_str(), &sb) != -1) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
	return false;
}

static bool _useTmpfsCache = false;

void TWFunc::Use_Tmpfs_Cache() {
	_useTmpfsCache = true;
	remove("/cache");
	mkdir("/cache", 0777);
}

#ifndef BUILD_TWRPTAR_MAIN

// Returns "/path" from a full /path/to/file.name
string TWFunc::Get_Root_Path(const string& Path) {
	string Local_Path = Path;

	// Make sure that we have a leading slash
	if (Local_Path.substr(0, 1) != "/")
		Local_Path = "/" + Local_Path;

	// Trim the path to get the root path only
	size_t position = Local_Path.find("/", 2);
	if (position != string::npos) {
		Local_Path.resize(position);
	}
	return Local_Path;
}

int TWFunc::Recursive_Mkdir(string Path, bool ShowErr) {
	std::vector<std::string> parts = Split_String(Path, "/", true);
	std::string cur_path;
	for (size_t i = 0; i < parts.size(); ++i) {
		cur_path += "/" + parts[i];
		if (!TWFunc::Path_Exists(cur_path)) {
			if (mkdir(cur_path.c_str(), 0777)) {
				// OrangeFox 移植:fox_14.1 支持静默失败 Recursive_Mkdir(path, false)
				if (ShowErr)
					gui_msg(Msg(msg::kError, "create_folder_strerr=Can not create '{1}' folder ({2}).")(cur_path)(strerror(errno)));
				return false;
			} else {
				tw_set_default_metadata(cur_path.c_str());
			}
		}
	}
	return true;
}

void TWFunc::GUI_Operation_Text(string Read_Value, string Default_Text) {
	string Display_Text;

	DataManager::GetValue(Read_Value, Display_Text);
	if (Display_Text.empty())
		Display_Text = Default_Text;

	DataManager::SetValue("tw_operation", Display_Text);
	DataManager::SetValue("tw_partition", "");
}

void TWFunc::GUI_Operation_Text(string Read_Value, string Partition_Name, string Default_Text) {
	string Display_Text;

	DataManager::GetValue(Read_Value, Display_Text);
	if (Display_Text.empty())
		Display_Text = Default_Text;

	DataManager::SetValue("tw_operation", Display_Text);
	DataManager::SetValue("tw_partition", Partition_Name);
}

void TWFunc::Copy_Log(string Source, string Destination) {
	int logPipe[2];
	int pigz_pid;
	int destination_fd;
	std::string destLogBuffer;

	PartitionManager.Mount_By_Path(Destination, false);

	size_t extPos = Destination.find(".gz");
	std::string uncompressedLog(Destination);
	uncompressedLog.replace(extPos, Destination.length(), "");

	if (Path_Exists(Destination)) {
		Archive_Type type = Get_File_Type(Destination);
		if (type == COMPRESSED) {
			std::string destFileBuffer;
			std::string getCompressedContents = "pigz -c -d " + Destination;
			if (Exec_Cmd(getCompressedContents, destFileBuffer, false) < 0) {
				LOGINFO("Unable to get destination logfile contents.\n");
				return;
			}
			destLogBuffer.append(destFileBuffer);
		}
	} else if (Path_Exists(uncompressedLog)) {
		std::ifstream uncompressedIfs(uncompressedLog.c_str());
		std::stringstream uncompressedSS;
		uncompressedSS << uncompressedIfs.rdbuf();
		uncompressedIfs.close();
		std::string uncompressedLogBuffer(uncompressedSS.str());
		destLogBuffer.append(uncompressedLogBuffer);
		std::remove(uncompressedLog.c_str());
	}

	std::ifstream ifs(Source.c_str());
	std::stringstream ss;
	ss << ifs.rdbuf();
	std::string srcLogBuffer(ss.str());
	ifs.close();

	if (pipe(logPipe) < 0) {
		LOGINFO("Unable to open pipe to write to persistent log file: %s\n", Destination.c_str());
	}

	destination_fd = open(Destination.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);

	pigz_pid = fork();
	if (pigz_pid < 0) {
		LOGINFO("fork() failed\n");
		close(destination_fd);
		close(logPipe[0]);
		close(logPipe[1]);
	} else if (pigz_pid == 0) {
		close(logPipe[1]);
		dup2(logPipe[0], fileno(stdin));
		dup2(destination_fd, fileno(stdout));
		if (execlp("pigz", "pigz", "-", NULL) < 0) {
			close(destination_fd);
			close(logPipe[0]);
			_exit(-1);
		}
	} else {
		close(logPipe[0]);
		if (write(logPipe[1], destLogBuffer.c_str(), destLogBuffer.size()) < 0) {
			LOGINFO("Unable to append to persistent log: %s\n", Destination.c_str());
			close(logPipe[1]);
			close(destination_fd);
			return;
		}
		if (write(logPipe[1], srcLogBuffer.c_str(), srcLogBuffer.size()) < 0) {
			LOGINFO("Unable to append to persistent log: %s\n", Destination.c_str());
			close(logPipe[1]);
			close(destination_fd);
			return;
		}
		close(logPipe[1]);
	}
	close(destination_fd);
}

void TWFunc::Update_Log_File(void) {
	std::string logDir = get_log_dir();

	if (logDir == CACHE_LOGS_DIR) {
		if (!PartitionManager.Mount_By_Path(CACHE_LOGS_DIR, false)) {
			LOGINFO("Failed to mount %s for TWFunc::Update_Log_File\n", CACHE_LOGS_DIR);
		}
	}

	if (logDir == DATA_LOGS_DIR && !TWFunc::Path_Exists(DATA_LOGS_DIR)) {
		Use_Tmpfs_Cache();
		logDir = CACHE_LOGS_DIR;
	}

	std::string recoveryDir = logDir + "recovery/";

	if (!TWFunc::Path_Exists(recoveryDir)) {
		LOGINFO("Recreating %s folder.\n", recoveryDir.c_str());
		if (!Create_Dir_Recursive(recoveryDir,  S_IRWXU | S_IRWXG | S_IWGRP | S_IXGRP, 0, 0)) {
			LOGINFO("Unable to create %s folder.\n", recoveryDir.c_str());
		}
	}

	std::string logCopy = recoveryDir + "log.gz";
	std::string lastLogCopy = recoveryDir + "last_log.gz";
	copy_file(logCopy, lastLogCopy, 0600);
	Copy_Log(TMP_LOG_FILE, logCopy);
	chown(logCopy.c_str(), 1000, 1000);
	chmod(logCopy.c_str(), 0600);
	chmod(lastLogCopy.c_str(), 0640);

	if (get_log_dir() == CACHE_LOGS_DIR) {
		if (PartitionManager.Mount_By_Path("/cache", false)) {
			if (unlink("/cache/recovery/command") && errno != ENOENT) {
				LOGINFO("Can't unlink %s\n", "/cache/recovery/command");
			}
		}
	}
	sync();
}

void TWFunc::Clear_Bootloader_Message() {
	std::string err;
	if (!clear_bootloader_message(&err)) {
		LOGINFO("%s\n", err.c_str());
	}
}

void TWFunc::Update_Intent_File(string Intent) {
	if (PartitionManager.Mount_By_Path("/cache", false) && !Intent.empty()) {
		TWFunc::write_to_file("/cache/recovery/intent", Intent);
	}
}

// reboot: Reboot the system. Return -1 on error, no return on success
int TWFunc::tw_reboot(RebootCommand command)
{
	DataManager::Flush();
	Update_Log_File();

	// Always force a sync before we reboot
	sync();

	TWPartition *dataPart = PartitionManager.Find_Partition_By_Path("/data");
	if (dataPart) {
		if (dataPart->Is_Mounted()) {
			if (!dataPart->UnMount(false)) {
				killForUseTargetProcess(dataPart->Get_Mount_Point());
				dataPart->UnMount(false);
			}
		}
	}

	switch (command) {
		case rb_current:
		case rb_system:
			Update_Intent_File("s");
			sync();
			check_and_run_script("/system/bin/rebootsystem.sh", "reboot system");
#ifdef ANDROID_RB_PROPERTY
			return property_set(ANDROID_RB_PROPERTY, "reboot,");
#elif defined(ANDROID_RB_RESTART)
			return android_reboot(ANDROID_RB_RESTART, 0, 0);
#else
			return reboot(RB_AUTOBOOT);
#endif
		case rb_recovery:
			check_and_run_script("/system/bin/rebootrecovery.sh", "reboot recovery");
			return property_set(ANDROID_RB_PROPERTY, "reboot,recovery");
		case rb_bootloader:
			check_and_run_script("/system/bin/rebootbootloader.sh", "reboot bootloader");
			return property_set(ANDROID_RB_PROPERTY, "reboot,bootloader");
		case rb_poweroff:
			check_and_run_script("/system/bin/poweroff.sh", "power off");
#ifdef ANDROID_RB_PROPERTY
			return property_set(ANDROID_RB_PROPERTY, "shutdown,");
#elif defined(ANDROID_RB_POWEROFF)
			return android_reboot(ANDROID_RB_POWEROFF, 0, 0);
#else
			return reboot(RB_POWER_OFF);
#endif
		case rb_download:
			check_and_run_script("/system/bin/rebootdownload.sh", "reboot download");
			return property_set(ANDROID_RB_PROPERTY, "reboot,download");
		case rb_edl:
			check_and_run_script("/system/bin/rebootedl.sh", "reboot edl");
			return property_set(ANDROID_RB_PROPERTY, "reboot,edl");
		case rb_fastboot:
			return property_set(ANDROID_RB_PROPERTY, "reboot,fastboot");
		default:
			return -1;
	}
	return -1;
}

void TWFunc::check_and_run_script(const char* script_file, const char* display_name)
{
	// Check for and run startup script if script exists
	struct stat st;
	if (stat(script_file, &st) == 0) {
		gui_msg(Msg("run_script=Running {1} script...")(display_name));
		chmod(script_file, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
		TWFunc::Exec_Cmd(script_file);
		gui_msg("done=Done.");
	}
}

int TWFunc::removeDir(const string path, bool skipParent) {
	DIR *d = opendir(path.c_str());
	int r = 0;
	string new_path;

	if (d == NULL) {
		gui_msg(Msg(msg::kError, "error_opening_strerr=Error opening: '{1}' ({2})")(path)(strerror(errno)));
		return -1;
	}

	if (d) {
		struct dirent *p;
		while (!r && (p = readdir(d))) {
			if (!strcmp(p->d_name, ".") || !strcmp(p->d_name, ".."))
				continue;
			new_path = path + "/";
			new_path.append(p->d_name);
			if (p->d_type == DT_DIR) {
				r = removeDir(new_path, true);
				if (!r) {
					if (p->d_type == DT_DIR)
						r = rmdir(new_path.c_str());
					else
						LOGINFO("Unable to removeDir '%s': %s\n", new_path.c_str(), strerror(errno));
				}
			} else if (p->d_type == DT_REG || p->d_type == DT_LNK || p->d_type == DT_FIFO || p->d_type == DT_SOCK) {
				r = unlink(new_path.c_str());
				if (r != 0) {
					LOGINFO("Unable to unlink '%s: %s'\n", new_path.c_str(), strerror(errno));
				}
			}
		}
		closedir(d);

		if (!r) {
			if (skipParent)
				return 0;
			else
				r = rmdir(path.c_str());
		}
	}
	return r;
}

int TWFunc::copy_file(string src, string dst, int mode, bool mount_paths) {
	if (mount_paths) {
		PartitionManager.Mount_By_Path(src, false);
		PartitionManager.Mount_By_Path(dst, false);
	}
	if (!Path_Exists(src)) {
		LOGINFO("Path %s does not exist. Unable to copy file to %s\n", src.c_str(), dst.c_str());
		return -1;
	}
	std::ifstream srcfile(src.c_str(), ios::binary);
	std::ofstream dstfile(dst.c_str(), ios::binary);
	dstfile << srcfile.rdbuf();
	if (dstfile.bad()) {
		LOGINFO("Unable to copy file %s to %s\n", src.c_str(), dst.c_str());
		return -1;
	}

	srcfile.close();
	dstfile.close();
	if (chmod(dst.c_str(), mode) != 0) {
		LOGERR("Unable to chmod file: %s. Error: %s\n", dst.c_str(), strerror(errno));
		return -1;
	}
	return 0;
}

unsigned int TWFunc::Get_D_Type_From_Stat(string Path) {
	struct stat st;

	stat(Path.c_str(), &st);
	if (st.st_mode & S_IFDIR)
		return DT_DIR;
	else if (st.st_mode & S_IFBLK)
		return DT_BLK;
	else if (st.st_mode & S_IFCHR)
		return DT_CHR;
	else if (st.st_mode & S_IFIFO)
		return DT_FIFO;
	else if (st.st_mode & S_IFLNK)
		return DT_LNK;
	else if (st.st_mode & S_IFREG)
		return DT_REG;
	else if (st.st_mode & S_IFSOCK)
		return DT_SOCK;
	return DT_UNKNOWN;
}

int TWFunc::read_file(string fn, string& results) {
	ifstream file;
	file.open(fn.c_str(), ios::in);

	if (file.is_open()) {
		std::string line;
		while (std::getline(file, line)) {
			results += line;
		}
		file.close();
		return 0;
	}

	LOGINFO("Cannot find file %s\n", fn.c_str());
	return -1;
}

int TWFunc::read_file(string fn, vector<string>& results) {
	ifstream file;
	string line;
	file.open(fn.c_str(), ios::in);
	if (file.is_open()) {
		while (getline(file, line))
			results.push_back(line);
		file.close();
		return 0;
	}
	LOGINFO("Cannot find file %s\n", fn.c_str());
	return -1;
}

int TWFunc::read_file(string fn, uint64_t& results) {
	ifstream file;
	file.open(fn.c_str(), ios::in);

	if (file.is_open()) {
		file >> results;
		file.close();
		return 0;
	}

	LOGINFO("Cannot find file %s\n", fn.c_str());
	return -1;
}

bool TWFunc::write_to_file(const string& fn, const string& line) {
	FILE *file;
	file = fopen(fn.c_str(), "w");
	if (file != NULL) {
		fwrite(line.c_str(), line.size(), 1, file);
		fclose(file);
		return true;
	}
	LOGINFO("Cannot find file %s\n", fn.c_str());
	return false;
}

bool TWFunc::write_to_file(const string& fn, const std::vector<string> lines) {
	FILE *file;
	file = fopen(fn.c_str(), "a+");
	if (file != NULL) {
		for (auto&& line: lines) {
			fwrite(line.c_str(), line.size(), 1, file);
			fwrite("\n", sizeof(char), 1, file);
		}
		fclose(file);
		return true;
	}
	return false;
}


bool TWFunc::Try_Decrypting_Backup(string Restore_Path, string Password) {
	DIR* d;

	string Filename;
	Restore_Path += "/";
	d = opendir(Restore_Path.c_str());
	if (d == NULL) {
		gui_msg(Msg(msg::kError, "error_opening_strerr=Error opening: '{1}' ({2})")(Restore_Path)(strerror(errno)));
		return false;
	}

	struct dirent* de;
	while ((de = readdir(d)) != NULL) {
		Filename = Restore_Path;
		Filename += de->d_name;
		if (TWFunc::Get_File_Type(Filename) == ENCRYPTED) {
			if (TWFunc::Try_Decrypting_File(Filename, Password) < 2) {
				DataManager::SetValue("tw_restore_password", ""); // Clear the bad password
				DataManager::SetValue("tw_restore_display", "");  // Also clear the display mask
				closedir(d);
				return false;
			}
		}
	}
	closedir(d);
	return true;
}

string TWFunc::Get_Current_Date() {
	string Current_Date;
	time_t seconds = time(0);
	struct tm *t = localtime(&seconds);
	char timestamp[255];
	sprintf(timestamp,"%04d-%02d-%02d--%02d-%02d-%02d",t->tm_year+1900,t->tm_mon+1,t->tm_mday,t->tm_hour,t->tm_min,t->tm_sec);
	Current_Date = timestamp;
	return Current_Date;
}

string TWFunc::System_Property_Get(string Prop_Name) {
	return Partition_Property_Get(Prop_Name, PartitionManager, PartitionManager.Get_Android_Root_Path(), "build.prop");
}

string TWFunc::Partition_Property_Get(string Prop_Name, TWPartitionManager &PartitionManager, string Mount_Point, string prop_file_name) {
	bool mount_state = PartitionManager.Is_Mounted_By_Path(Mount_Point);
	std::vector<string> buildprop;
	string propvalue;
	string prop_file;
	if (!PartitionManager.Mount_By_Path(Mount_Point, true))
		return propvalue;
	if (Mount_Point == PartitionManager.Get_Android_Root_Path()) {
		prop_file = Mount_Point + "/system/" + prop_file_name;
	} else {
		prop_file = Mount_Point + "/" + prop_file_name;
	}
	if (!TWFunc::Path_Exists(prop_file)) {
		LOGINFO("Unable to locate file: %s\n", prop_file.c_str());
		return propvalue;
	}
	if (TWFunc::read_file(prop_file, buildprop) != 0) {
		LOGINFO("Unable to open %s for getting '%s'.\n", prop_file_name.c_str(), Prop_Name.c_str());
		DataManager::SetValue(TW_BACKUP_NAME, Get_Current_Date());
		if (!mount_state)
			PartitionManager.UnMount_By_Path(Mount_Point, false);
		return propvalue;
	}
	int line_count = buildprop.size();
	int index;
	size_t start_pos = 0, end_pos;
	string propname;
	for (index = 0; index < line_count; index++) {
		end_pos = buildprop.at(index).find("=", start_pos);
		propname = buildprop.at(index).substr(start_pos, end_pos);
		if (propname == Prop_Name) {
			propvalue = buildprop.at(index).substr(end_pos + 1, buildprop.at(index).size());
			if (!mount_state)
				PartitionManager.UnMount_By_Path(Mount_Point, false);
			return propvalue;
		}
	}
	if (!mount_state)
		PartitionManager.UnMount_By_Path(Mount_Point, false);
	return propvalue;
}

void TWFunc::Auto_Generate_Backup_Name() {
	string propvalue = System_Property_Get("ro.build.display.id");
	if (propvalue.empty()) {
		DataManager::SetValue(TW_BACKUP_NAME, Get_Current_Date());
		return;
	}
	else {
		//remove periods from build display so it doesn't confuse the extension code
		propvalue.erase(remove(propvalue.begin(), propvalue.end(), '.'), propvalue.end());
	}
	string Backup_Name = Get_Current_Date();
	Backup_Name += "_" + propvalue;
	if (Backup_Name.size() > MAX_BACKUP_NAME_LEN)
		Backup_Name.resize(MAX_BACKUP_NAME_LEN);
	// Trailing spaces cause problems on some file systems, so remove them
	string space_check, space = " ";
	space_check = Backup_Name.substr(Backup_Name.size() - 1, 1);
	while (space_check == space) {
		Backup_Name.resize(Backup_Name.size() - 1);
		space_check = Backup_Name.substr(Backup_Name.size() - 1, 1);
	}
	replace(Backup_Name.begin(), Backup_Name.end(), ' ', '_');
	if (PartitionManager.Check_Backup_Name(Backup_Name, false, true) != 0) {
		LOGINFO("Auto generated backup name '%s' is not valid, using date instead.\n", Backup_Name.c_str());
		DataManager::SetValue(TW_BACKUP_NAME, Get_Current_Date());
	} else {
		DataManager::SetValue(TW_BACKUP_NAME, Backup_Name);
	}
}

void TWFunc::Fixup_Time_On_Boot(const string& time_paths /* = "" */)
{
#ifdef QCOM_RTC_FIX
	static bool fixed = false;
	if (fixed)
		return;

	LOGINFO("TWFunc::Fixup_Time: Pre-fix date and time: %s\n", TWFunc::Get_Current_Date().c_str());

	struct timeval tv;
	uint64_t offset = 0;
	std::string sepoch = "/sys/class/rtc/rtc0/since_epoch";

	if (TWFunc::read_file(sepoch, offset) == 0) {

		LOGINFO("TWFunc::Fixup_Time: Setting time offset from file %s\n", sepoch.c_str());

		tv.tv_sec = offset;
		tv.tv_usec = 0;
		settimeofday(&tv, NULL);

		gettimeofday(&tv, NULL);

		if (tv.tv_sec > 1517600000) { // Anything older then 2 Feb 2018 19:33:20 GMT will do nicely thank you ;)

			LOGINFO("TWFunc::Fixup_Time: Date and time corrected: %s\n", TWFunc::Get_Current_Date().c_str());
			fixed = true;
			return;

		}

	} else {

		LOGINFO("TWFunc::Fixup_Time: opening %s failed\n", sepoch.c_str());

	}

	LOGINFO("TWFunc::Fixup_Time: will attempt to use the ats files now.\n");

	// Devices with Qualcomm Snapdragon 800 do some shenanigans with RTC.
	// They never set it, it just ticks forward from 1970-01-01 00:00,
	// and then they have files /data/system/time/ats_* with 64bit offset
	// in miliseconds which, when added to the RTC, gives the correct time.
	// So, the time is: (offset_from_ats + value_from_RTC)
	// There are multiple ats files, they are for different systems? Bases?
	// Like, ats_1 is for modem and ats_2 is for TOD (time of day?).
	// Look at file time_genoff.h in CodeAurora, qcom-opensource/time-services

	std::vector<std::string> paths; // space separated list of paths
	if (time_paths.empty()) {
		paths = Split_String("/data/system/time/ /data/time/ /data/vendor/time/", " ");
		if (!PartitionManager.Mount_By_Path("/data", false))
			return;
	} else {
		// When specific path(s) are used, Fixup_Time needs those
		// partitions to already be mounted!
		paths = Split_String(time_paths, " ");
	}

	FILE *f;
	offset = 0;
	struct dirent *dt;
	std::string ats_path;

	// Prefer ats_2, it seems to be the one we want according to logcat on hammerhead
	// - it is the one for ATS_TOD (time of day?).
	// However, I never saw a device where the offset differs between ats files.
	for (size_t i = 0; i < paths.size(); ++i)
	{
		DIR *d = opendir(paths[i].c_str());
		if (!d)
			continue;

		while ((dt = readdir(d)))
		{
			if (dt->d_type != DT_REG || strncmp(dt->d_name, "ats_", 4) != 0)
				continue;

			if (ats_path.empty() || strcmp(dt->d_name, "ats_2") == 0)
				ats_path = paths[i] + dt->d_name;
		}

		closedir(d);
	}

	if (ats_path.empty()) {
		LOGINFO("TWFunc::Fixup_Time: no ats files found, leaving untouched!\n");
	} else if ((f = fopen(ats_path.c_str(), "r")) == NULL) {
		LOGINFO("TWFunc::Fixup_Time: failed to open file %s\n", ats_path.c_str());
	} else if (fread(&offset, sizeof(offset), 1, f) != 1) {
		LOGINFO("TWFunc::Fixup_Time: failed load uint64 from file %s\n", ats_path.c_str());
		fclose(f);
	} else {
		fclose(f);

		LOGINFO("TWFunc::Fixup_Time: Setting time offset from file %s, offset %llu\n", ats_path.c_str(), (unsigned long long) offset);
		DataManager::SetValue("tw_qcom_ats_offset", (unsigned long long) offset, 1);
		fixed = true;
	}

	if (!fixed) {
#ifdef TW_QCOM_ATS_OFFSET
		// Offset is the difference between the current time and the time since_epoch
		// To calculate the offset in Android, the following expression (from a root shell) can be used:
		// echo "$(( ($(date +%s) - $(cat /sys/class/rtc/rtc0/since_epoch)) ))"
		// Add 3 zeros to the output and use that in the TW_QCOM_ATS_OFFSET flag in your BoardConfig.mk
		// For example, if the result of the calculation is 1642433544, use 1642433544000 as the offset
		offset = (uint64_t) TW_QCOM_ATS_OFFSET;
		DataManager::SetValue("tw_qcom_ats_offset", (unsigned long long) offset, 1);
		LOGINFO("TWFunc::Fixup_Time: Setting time offset from TW_QCOM_ATS_OFFSET, offset %llu\n", (unsigned long long) offset);
#else
		// Failed to get offset from ats file, check twrp settings
		unsigned long long value;
		if (DataManager::GetValue("tw_qcom_ats_offset", value) < 0) {
			return;
		} else {
			offset = (uint64_t) value;
			LOGINFO("TWFunc::Fixup_Time: Setting time offset from twrp setting file, offset %llu\n", (unsigned long long) offset);
			// Do not consider the settings file as a definitive answer, keep fixed=false so next run will try ats files again
		}
#endif
	}

	gettimeofday(&tv, NULL);

	tv.tv_sec += offset/1000;
#ifdef TW_CLOCK_OFFSET
// Some devices are even quirkier and have ats files that are offset from the actual time
	tv.tv_sec = tv.tv_sec + TW_CLOCK_OFFSET;
#endif
	tv.tv_usec += (offset%1000)*1000;

	while (tv.tv_usec >= 1000000)
	{
		++tv.tv_sec;
		tv.tv_usec -= 1000000;
	}

	settimeofday(&tv, NULL);

	LOGINFO("TWFunc::Fixup_Time: Date and time corrected: %s\n", TWFunc::Get_Current_Date().c_str());
#endif
}

std::vector<std::string> TWFunc::Split_String(const std::string& str, const std::string& delimiter, bool removeEmpty)
{
	std::vector<std::string> res;
	size_t idx = 0, idx_last = 0;

	while (idx < str.size())
	{
		idx = str.find_first_of(delimiter, idx_last);
		if (idx == std::string::npos)
			idx = str.size();

		if (idx-idx_last != 0 || !removeEmpty)
			res.push_back(str.substr(idx_last, idx-idx_last));

		idx_last = idx + delimiter.size();
	}

	return res;
}

bool TWFunc::Create_Dir_Recursive(const std::string& path, mode_t mode, uid_t uid, gid_t gid)
{
	std::vector<std::string> parts = Split_String(path, "/");
	std::string cur_path;
	struct stat info;
	for (size_t i = 0; i < parts.size(); ++i)
	{
		cur_path += "/" + parts[i];
		if (stat(cur_path.c_str(), &info) < 0 || !S_ISDIR(info.st_mode))
		{
			if (mkdir(cur_path.c_str(), mode) < 0)
				return false;
			chown(cur_path.c_str(), uid, gid);
		}
	}
	return true;
}

int TWFunc::Set_Brightness(std::string brightness_value)
{
	int result = -1;
	std::string secondary_brightness_file;

	if (DataManager::GetIntValue("tw_has_brightnesss_file")) {
		LOGINFO("TWFunc::Set_Brightness: Setting brightness control to %s\n", brightness_value.c_str());
		result = TWFunc::write_to_file(DataManager::GetStrValue("tw_brightness_file"), brightness_value);
		DataManager::GetValue("tw_secondary_brightness_file", secondary_brightness_file);
		if (!secondary_brightness_file.empty()) {
			LOGINFO("TWFunc::Set_Brightness: Setting secondary brightness control to %s\n", brightness_value.c_str());
			TWFunc::write_to_file(secondary_brightness_file, brightness_value);
		}
	}
	return result ? 0 : -1;
}

bool TWFunc::Toggle_MTP(bool enable) {
#ifdef TW_HAS_MTP
	static int was_enabled = false;

	if (enable && was_enabled) {
		if (!PartitionManager.Enable_MTP())
			PartitionManager.Disable_MTP();
	} else {
		was_enabled = DataManager::GetIntValue("tw_mtp_enabled");
		PartitionManager.Disable_MTP();
		usleep(500);
	}
	return was_enabled;
#else
	return false;
#endif
}

void TWFunc::SetPerformanceMode(bool mode) {
	if (mode) {
		property_set("recovery.perf.mode", "1");
	} else {
		property_set("recovery.perf.mode", "0");
	}
	// Some time for events to catch up to init handlers
	usleep(500000);
}

std::string TWFunc::to_string(unsigned long value) {
	std::ostringstream os;
	os << value;
	return os.str();
}

void TWFunc::Disable_Stock_Recovery_Replace(void) {
	if (PartitionManager.Mount_By_Path(PartitionManager.Get_Android_Root_Path(), false)) {
		// Disable flashing of stock recovery
		if (TWFunc::Path_Exists("/system/recovery-from-boot.p")) {
			rename("/system/recovery-from-boot.p", "/system/recovery-from-boot.bak");
			gui_msg("rename_stock=Renamed stock recovery file in /system to prevent the stock ROM from replacing TWRP.");
			sync();
		}
		PartitionManager.UnMount_By_Path(PartitionManager.Get_Android_Root_Path(), false);
	}
}

unsigned long long TWFunc::IOCTL_Get_Block_Size(const char* block_device) {
	unsigned long block_device_size;
	int ret = 0;

	int fd = open(block_device, O_RDONLY);
	if (fd < 0) {
		LOGINFO("Find_Partition_Size: Failed to open '%s', (%s)\n", block_device, strerror(errno));
	} else {
		ret = ioctl(fd, BLKGETSIZE, &block_device_size);
		close(fd);
		if (ret) {
			LOGINFO("Find_Partition_Size: ioctl error: (%s)\n", strerror(errno));
		} else {
			return (unsigned long long)(block_device_size) * 512LLU;
		}
	}
	return 0;
}

void TWFunc::copy_kernel_log(string curr_storage) {
	std::string dmesgDst = curr_storage + "/dmesg.log";
	std::string dmesgCmd = "/system/bin/dmesg";

	std::string result;
	Exec_Cmd(dmesgCmd, result, false);
	write_to_file(dmesgDst, result);
	gui_msg(Msg("copy_kernel_log=Copied kernel log to {1}")(dmesgDst));
	tw_set_default_metadata(dmesgDst.c_str());
}

void TWFunc::copy_logcat(string curr_storage) {
	std::string logcatDst = curr_storage + "/logcat.txt";
	std::string logcatCmd = "logcat -d";

	std::string result;
	Exec_Cmd(logcatCmd, result, false);
	write_to_file(logcatDst, result);
	gui_msg(Msg("copy_logcat=Copied logcat to {1}")(logcatDst));
	tw_set_default_metadata(logcatDst.c_str());
}

bool TWFunc::isNumber(string strtocheck) {
	int num = 0;
	std::istringstream iss(strtocheck);

	if (!(iss >> num).fail())
		return true;
	else
		return false;
}

int TWFunc::stream_adb_backup(string &Restore_Name) {
	string cmd = "/system/bin/bu --twrp stream " + Restore_Name;
	LOGINFO("stream_adb_backup: %s\n", cmd.c_str());
	int ret = TWFunc::Exec_Cmd(cmd);
	if (ret != 0)
		return -1;
	return ret;
}

std::string TWFunc::get_log_dir() {
	if (_useTmpfsCache) return CACHE_LOGS_DIR;
	if (PartitionManager.Find_Partition_By_Path(CACHE_LOGS_DIR) == NULL) {
		if (PartitionManager.Find_Partition_By_Path(DATA_LOGS_DIR) == NULL) {
			LOGINFO("Unable to find a directory to store TWRP logs.");
			return "";
		} else {
			return DATA_LOGS_DIR;
		}
	}
	else {
		return CACHE_LOGS_DIR;
	}
}

void TWFunc::check_selinux_support() {
	if (TWFunc::Path_Exists("/prebuilt_file_contexts")) {
		if (TWFunc::Path_Exists("/file_contexts")) {
			printf("Renaming regular /file_contexts -> /file_contexts.bak\n");
			rename("/file_contexts", "/file_contexts.bak");
		}
		printf("Moving /prebuilt_file_contexts -> /file_contexts\n");
		rename("/prebuilt_file_contexts", "/file_contexts");
	}
	struct selinux_opt selinux_options[] = {
		{ SELABEL_OPT_PATH, "/file_contexts" }
	};
	selinux_handle = selabel_open(SELABEL_CTX_FILE, selinux_options, 1);
	if (!selinux_handle)
		printf("No file contexts for SELinux\n");
	else
		printf("SELinux contexts loaded from /file_contexts\n");
	{ // Check to ensure SELinux can be supported by the kernel
		char *contexts = NULL;
		std::string cacheDir = TWFunc::get_log_dir();
		std::string se_context_check = cacheDir + "recovery/";
		int ret = 0;

		if (cacheDir == CACHE_LOGS_DIR) {
			PartitionManager.Mount_By_Path(CACHE_LOGS_DIR, false);
		}
		if (TWFunc::Path_Exists(se_context_check)) {
			ret = lgetfilecon(se_context_check.c_str(), &contexts);
			if (ret < 0) {
				LOGINFO("Could not check %s SELinux contexts.\n", se_context_check.c_str());
			}
		}
		if (ret < 0) {
			gui_warn("no_kernel_selinux=Kernel does not have support for reading SELinux contexts.");
		} else {
			free(contexts);
			gui_msg("full_selinux=Full SELinux support is present.");
		}
	}
}

int TWFunc::Property_Override(string Prop_Name, string Prop_Value) {
#ifdef TW_INCLUDE_LIBRESETPROP
    return setprop(Prop_Name.c_str(), Prop_Value.c_str(), false);
#else
    return NOT_AVAILABLE;
#endif
}

int TWFunc::Delete_Property(string Prop_Name) {
#ifdef TW_INCLUDE_LIBRESETPROP
    return delprop(Prop_Name.c_str(), false);
#else
    return NOT_AVAILABLE;
#endif
}

void TWFunc::List_Mounts() {
	std::vector<std::string> mounts;
	read_file("/proc/mounts", mounts);
	LOGINFO("Mounts:\n");
	for (auto&& mount: mounts) {
		LOGINFO("%s\n", mount.c_str());
	}
}

string TWFunc::Check_For_TwrpFolder() {
	string oldFolder = "";
	vector<string> customTWRPFolders;
	string mainPath = DataManager::GetCurrentStoragePath();
	DIR* d;
	struct dirent* de;

	if (DataManager::GetIntValue(TW_IS_ENCRYPTED) && DataManager::GetIntValue(TW_CRYPTO_PWTYPE)) {
		goto exit;
	}


	d = opendir(mainPath.c_str());
	if (d == NULL) {
		goto exit;
	}

	while ((de = readdir(d)) != NULL) {
		string name = de->d_name;
		string fullPath = mainPath + '/' + name;
		unsigned char type = de->d_type;

		if (name == "." || name == "..") continue;

		if (type == DT_UNKNOWN) {
			type = Get_D_Type_From_Stat(fullPath);
		}

		if (type == DT_DIR && Path_Exists(fullPath + "/.twrpcf")) {
			if ('/' + name == TW_DEFAULT_RECOVERY_FOLDER) {
				oldFolder = name;
			} else {
				customTWRPFolders.push_back(name);
			}
		}
	}

	closedir(d);

	if (oldFolder == "" && customTWRPFolders.empty()) {
		LOGINFO("No recovery folder found. Using default folder.\n");
		goto exit;
	} else if (customTWRPFolders.empty()) {
		LOGINFO("No custom recovery folder found. Using TWRP as default.\n");
		goto exit;
	} else {
		if (customTWRPFolders.size() > 1) {
			LOGINFO("More than one custom recovery folder found. Using first one from the list.\n");
		} else {
			LOGINFO("One custom recovery folder found.\n");
		}
		string customPath =  '/' + customTWRPFolders.at(0);

		if (Path_Exists(mainPath + TW_DEFAULT_RECOVERY_FOLDER)) {
			string oldBackupFolder = mainPath + TW_DEFAULT_RECOVERY_FOLDER + "/BACKUPS/" + DataManager::GetStrValue("device_id");
			string newBackupFolder = mainPath + customPath + "/BACKUPS/" + DataManager::GetStrValue("device_id");

			if (Path_Exists(oldBackupFolder)) {
				vector<string> backups;
				d = opendir(oldBackupFolder.c_str());

				if (d != NULL) {
					while ((de = readdir(d)) != NULL) {
						string name = de->d_name;
						unsigned char type = de->d_type;

						if (name == "." || name == "..") continue;

						if (type == DT_UNKNOWN) {
							type = Get_D_Type_From_Stat(mainPath + '/' + name);
						}

						if (type == DT_DIR) {
							backups.push_back(name);
						}
					}
					closedir(d);
				}

				for (auto it = backups.begin(); it != backups.end(); it++) {
					Exec_Cmd("mv -f \"" + oldBackupFolder + '/' + *it + "\" \"" + newBackupFolder + '/' + *it + (Path_Exists(newBackupFolder + '/' + *it) ? "_new\"" : "\""));
				}
			}
			Exec_Cmd("rm -rf \"" + mainPath + TW_DEFAULT_RECOVERY_FOLDER + '\"');
		}

		return customPath;
	}

exit:
	return TW_DEFAULT_RECOVERY_FOLDER;
}

bool TWFunc::Check_Xml_Format(const std::string filename) {
	std::string buffer(' ', 4);
	std::string abx_hdr("ABX\x00", 4);
	std::ifstream File;
	File.open(filename);
	if (File.is_open()) {
		File.get(&buffer[0], buffer.size());
		File.close();
		// Android Binary Xml start from these bytes
		if(!buffer.compare(0, abx_hdr.size(), abx_hdr))
			return false; // ABX format - requires conversion
	}
	return true; // good format, possible to parse
}

// return true=successful conversion (return the name of the converted file in "result");
// return false=an error happened (leave "result" alone)
bool TWFunc::abx_to_xml(const std::string path, std::string &result) {
	if (!TWFunc::Path_Exists(path))
		return false;

	std::filesystem::path dir = "/tmp/abx2xml";
	if (!TWFunc::Path_Exists(dir)) {
		if (mkdir(dir.c_str(), 0700) != 0)
			dir = "/tmp";
	}

	std::filesystem::path tmpl = dir / "abxXXXXXX";
	int fd = mkstemp(tmpl.string().data());
	if (fd < 0) {
		LOGINFO("Error. The abx conversion of %s has failed (mkstemp errno %d).\n",
				path.c_str(), errno);
		return false;
	}
	close(fd);  // abx2xml() reopens the path itself
	std::string tmp_path(tmpl);

	if (abx2xml(path, tmp_path, /*in_place=*/false) != 0 ||
	    !TWFunc::Path_Exists(tmp_path)) {
		LOGINFO("Error. The abx conversion of %s has failed.\n", path.c_str());
		unlink(tmp_path.c_str());
		return false;
	}
	result = tmp_path;
	return true;
}

std::string GetFstabPath() {
	for (const char* prop : {"fstab_suffix", "hardware", "hardware.platform"}) {
		std::string suffix;

		if (!fs_mgr_get_boot_config(prop, &suffix)) continue;

		for (const char* prefix : {// late-boot/post-boot locations
			"/odm/etc/fstab.", "/vendor/etc/fstab.",
			// early boot locations
			"/system/etc/fstab.", "/first_stage_ramdisk/system/etc/fstab.",
			"/fstab.", "/first_stage_ramdisk/fstab."}) {
				std::string fstab_path = prefix + suffix;
				LOGINFO("%s: %s\n", __func__, fstab_path.c_str());
				if (access(fstab_path.c_str(), F_OK) == 0) return fstab_path;
		}
	}

	return "";
}

bool TWFunc::Find_Fstab(string &fstab) {
	fstab = GetFstabPath();
	if (fstab == "") return false;
	return true;
}

static inline std::string Get_Version_From_FQ(std::string name) {
	int start, end;
	start = name.find('@') + 1;
	end = name.find(":") - start;
	return name.substr(start, end);
}

bool TWFunc::Get_Service_From_Manifest(std::string basepath, std::string service, std::string &res) {
	std::string manifestpath, filename, platform;
	manifestpath = basepath + "/etc/vintf/";
	bool ret = false;

	// Prefer using ro.boot.product.vendor.sku property, following AOSP VintfObject::fetchVendorHalManifest
	// If not set, also try ro.board.platform.
	platform = android::base::GetProperty("ro.boot.product.vendor.sku", "");
	if (platform.empty()) {
		LOGINFO("Property ro.boot.product.vendor.sku not found, trying to get vintf manifest file name from ro.board.platform\n");
		platform = android::base::GetProperty("ro.board.platform", "");
	}

	// Let's find the service xml if exists
	Exec_Cmd("find " + manifestpath + "manifest/ -type f -name *" + service + "*", filename, false);
	if (filename.empty()) {
		LOGINFO("Separate manifest doesn't exist for '%s'\n", service.c_str());
		// Look for manifest_PLATFORM.xml
		filename = manifestpath + "manifest_" + platform + ".xml";
		if (!Path_Exists(filename)) {
			// Use legacy manifest path if platform manifest is not found.
			LOGINFO("%s not found. Using default path for manifest.xml\n", filename.c_str());
			filename = manifestpath + "manifest.xml";
		}
	}
	if (Path_Exists(filename)) {
		char* manifest = PageManager::LoadFileToBuffer(filename, NULL);
		LOGINFO("Looking for '%s' service in manifest\n", service.c_str());
		rapidxml::xml_document<>* vintfManifest = new rapidxml::xml_document<>();
		vintfManifest->parse<0>(manifest);
		rapidxml::xml_node<>* manifestNode = vintfManifest->first_node("manifest");
		std::string version;
		if (manifestNode) {
			for (rapidxml::xml_node<>* child = manifestNode->first_node(); child; child = child->next_sibling()) {
				std::string type = child->name();
				if (type == "hal") {
					rapidxml::xml_node<>* nameNode = child->first_node("name");
					type = nameNode->value();
					if (type == service) {
						rapidxml::xml_node<> *versionNode = child->first_node("version");
						if (versionNode != nullptr) {
							LOGINFO("Found version in manifest: %s\n", versionNode->value());
						} else {
							versionNode = child->first_node("fqname");
							if (versionNode == nullptr) return ret;
							LOGINFO("Found fqname in manifest: %s\n", versionNode->value());
						}
						version = versionNode->value();
						if (version.find('@') == std::string::npos) {
							res = version;
						} else {
							res = Get_Version_From_FQ(version);
						}
						ret = true;
					}
				}
			}
		}
	}
	return ret;
}

#endif // ndef BUILD_TWRPTAR_MAIN

#ifndef BUILD_TWRPTAR_MAIN
/* =====================================================================================
 * OrangeFox(OFRP)移植段
 *   来源:/orangefox14/bootable/recovery/twrp-functions.cpp (分支 fox_14.1,基点 fox_12.1)
 *   规则:只移植"TWRP16 基版本中不存在的函数"(按函数名筛选);同名函数沿用上面 TWRP16 的实现,
 *        避免用较老的 TWRP 实现回退 TWRP16 的新代码。依赖 OF 常量(variables.h 移植段)、
 *        orangefox.hpp、abx-functions.hpp 以及 FOX_/OF_ 编译宏(见 fox_common.go)。
 * ===================================================================================== */

// ---- OF 文件级全局状态(原样) ----
// Globals
static string tmp = Fox_tmp_dir; // "/tmp/orangefox/"
static string split_img = tmp + "/split_img";
static string ramdisk = tmp + "/ramdisk";
static string tmp_boot = tmp + "/boot.img";
static string fstab1 = PartitionManager.Get_Android_Root_Path() + "/vendor/etc"; // /system/vendor/etc
static string Internal_SD = PartitionManager.Get_Internal_Storage_Path();
static string fstab2 = "/vendor/etc";
static string exec_error_str = "EXEC_ERROR!";
static string popen_error_str = "popen error!";
int Fox_Current_ROM_IsTreble = 0;
int ROM_IsRealTreble = 0;
int New_Fox_Installation = 0;
int OrangeFox_Startup_Executed = 0;
int Fox_Has_Welcomed = 0;
string Fox_Current_ROM = "";

/* OF 原码 Read_Write_Specific_Partition() 等用 TWFunc::Exec_Cmd(cmd, null) 丢弃命令输出,
   但 OF 树里没有 null 的定义(疑似遗留);这里补一个丢弃用空串。
   (原先同样使用它的 MIUI 相关实现已按"MIUI 属遗留代码"移除) */
static string null;

// ---- OF 独有实现(保持 OF 文件原顺序) ----

/* gui/listbox.cpp 需要:string 版之外的宽字符版 */
int TWFunc::read_file(string fn, vector < wstring > &results)
{
  wifstream file;
  wstring line;
  file.open(fn.c_str(), ios::in);
  if (file.is_open())
    {
      while (getline(file, line))
	results.push_back(line);
      file.close();
      return 0;
    }
  LOGINFO("Cannot find file %s\n", fn.c_str());
  return -1;
}


/* is this an A/B device? */
static bool Is_AB_Device() 
{
  #if defined(AB_OTA_UPDATER) || defined(FOX_AB_DEVICE)
     return true;
  #endif
  string s = TWFunc::Fox_Property_Get("ro.boot.slot_suffix");
  string u = TWFunc::Fox_Property_Get("ro.build.ab_update");
  return (!s.empty() && u == "true");
}

/* Get the display ID of the installed ROM */


/* Get the display ID of the installed ROM */
static string GetInstalledRom(void)
{
   if (!Fox_Current_ROM.empty())
    {
      return Fox_Current_ROM;
    }
   
   string s = TWFunc::System_Property_Get ("ro.build.display.id");
   if (s.empty())
   {
      s = TWFunc::System_Property_Get ("ro.build.id");
      if (s.empty())
         s = TWFunc::System_Property_Get ("ro.build.flavor");
      if (s.empty())
         s = TWFunc::System_Property_Get ("ro.build.description");
   }
   return s;
}

/* Get the value of a named variable from the prop file */


/* Get the value of a named variable from the prop file */
static string Get_Property (const string propname)
{
   string ret = TWFunc::Exec_With_Output ("getprop " + propname);
   if (ret == exec_error_str)
       return "";
   else
      return ret;//(Trim_Trailing_NewLine (ret));
}

/* remove trailing newline from string */


/* remove trailing newline from string */
static string Trim_Trailing_NewLine (const string src)
{
   string ret = src;
   ret.erase(std::remove(ret.begin(), ret.end(), '\n'), ret.end());   
   return ret;
}

/* is this a real treble device? (else, treble is emulated via /cust) */


/* is this a real treble device? (else, treble is emulated via /cust) */
static bool Is_Real_Treble(void)
{
   if (ROM_IsRealTreble == 1)
   {
      return true;
   }
   else
   {
      if (Get_Property ("orangefox.realtreble.rom") == "1" || TWFunc::Has_Vendor_Partition())
        {
           ROM_IsRealTreble = 1;
           return true;
        }
      else 
           return false;
   }
}

/* Are we running a Treble ROM (old or freshly installed) ? */


/* Are we running a Treble ROM (old or freshly installed) ? */
static bool Treble_Is_Running(void)
{ 
   int treble = DataManager::GetIntValue(FOX_ZIP_INSTALLER_TREBLE);
   if (Fox_Current_ROM_IsTreble == 1 || treble == 1 || ROM_IsRealTreble == 1 || Is_Real_Treble())
      return true;
   else
      return false; 
}

/* Return whether the device's storage is encrypted */


/* Return whether the device's storage is encrypted */
static bool StorageIsEncrypted(void)
{
  return (PartitionManager.Storage_Is_Encrypted());
}


std::string strReturnCurrentTime()
{
  time_t rawtime;
  struct tm * timeinfo;
  char buffer[80];

  time (&rawtime);
  timeinfo = localtime(&rawtime);

  strftime(buffer,sizeof(buffer),"%Y%m%d_%H%M%S",timeinfo);
  std::string str(buffer);
  return str;
}


int TWFunc::string_to_int(string String, int def_value)
{
int tmp;
  if ((istringstream(String) >> tmp)) 
      return tmp;
  else
      return def_value;
}


long TWFunc::string_to_long(string String, long def_value)
{
long tmp;
  if ((istringstream(String) >> tmp)) 
      return tmp;
  else
      return def_value;
}


uint64_t TWFunc::string_to_long(string String, uint64_t def_value)
{
uint64_t tmp;
  if ((istringstream(String) >> tmp)) 
      return tmp;
  else
      return def_value;
}

/* return whether there is a real vendor partition */


/* return whether there is a real vendor partition */
bool TWFunc::Has_Vendor_Partition(void)
{
   if (TWFunc::Path_Exists ("/dev/block/bootdevice/by-name/vendor") || TWFunc::Path_Exists ("/dev/block/by-name/vendor"))
       return true;
   else
      return false;
}

/* run startup script, if not already run by init */


/* run startup script, if not already run by init */
bool TWFunc::RunStartupScript(void)
{
string tprop = Get_Property("orangefox.postinit.status");
bool i = Path_Exists(orangefox_cfg);
   
   if (i == true || tprop == "1")
      {
         LOGINFO("DEBUG: OrangeFox: the startup script has been executed.\n");
         return false;
      }
   
   LOGINFO("DEBUG: OrangeFox: running the startup script...\n");
   TWFunc::Set_Sbin_Dir_Executable_Flags();
   Exec_Cmd(FOX_STARTUP_SCRIPT);

   // set the incremental version to the ROM's
   if (TWFunc::Path_Exists(orangefox_cfg)) {
  	string incr_version = TWFunc::File_Property_Get (orangefox_cfg, "INCREMENTAL_VERSION");
  	if (!incr_version.empty()) {
  	   LOGINFO("- Using the ROM's incremental version (%s)\n", incr_version.c_str());
  	   TWFunc::Fox_Property_Set("ro.build.version.incremental", incr_version);
  	}
    }

   return true;
}


bool TWFunc::MIUI_ROM_SetProperty(const int code)
{
string res = "0";
bool ret = false;
	
    if (code != 0) // whether a ROM was installed, and it is MIUI
      {
    	ret = (code == 2 || code == 3 || code == 22 || code == 23);
      }
    // MIUI 属遗留代码,检测已移除(本函数保留是因为 orangefox.cpp 仍调用它)
    else
      {
    	ret = false;
      }

    if (ret)
       res = "1";

    Fox_Property_Set("orangefox.miui.rom", res);
    return ret;
}


bool TWFunc::RunFoxScript(const std::string script, const std::string args)
{
    if (!Path_Exists(script))
       return false;

    chmod(script.c_str(), S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
    if (!args.empty())
	TWFunc::Exec_Cmd(script + " '" + args + "'");
    else
	TWFunc::Exec_Cmd(script);
    usleep(500000);
    return true;
}

/* function to run just before every reboot */


/* function to run just before every reboot */
void TWFunc::Run_Before_Reboot(void)
{
    // AVB20
    Patch_AVB20(true);
    usleep(4096);

    // Run any custom script before rebooting
    TWFunc::MIUI_ROM_SetProperty(0);
    TWFunc::RunFoxScript(FOX_BEFORE_REBOOT_SCRIPT, "");

    // remove openrecovery command file
    string COMMAND_FILE = "/data/cache/command";
    if (TWFunc::Path_Exists(COMMAND_FILE)) {
	unlink(COMMAND_FILE.c_str());
    }

    // logs & stuff
    string Logs_Dir = Fox_Logs_Dir;
    bool failed_decryption = (TWFunc::Fox_Property_Get("of_decryption_failed") == "true");
#if defined(FOX_USE_DATA_RECOVERY_FOR_SETTINGS) || !defined(FOX_MISCELLANEOUS_ROOT_DIRECTORY)
    // check whether decryption failed, and, if so, store the lastrecovery log under /data/recovery/
    if (failed_decryption) {
    	Logs_Dir = OF_STORAGE_PATH;
    	Logs_Dir += "/Fox/logs";
    }
#endif
    if (!Path_Exists(Logs_Dir)) {
	  TWFunc::Create_Dir_Recursive(Fox_Logs_Dir, 0777, AID_MEDIA_RW, AID_MEDIA_RW);
    }

    //[f/d] release info json for app
    TWFunc::write_to_file(Logs_Dir + "/releaseinfo.json",
"{\"json_ver\":\"2\",\"codename\":\"" + DataManager::GetStrValue(FOX_COMPATIBILITY_DEVICE) +
                     "\",\"type\":\"" + FOX_BUILD_TYPE                                     +
                  "\",\"version\":\"" + FOX_BUILD                                          +
                   "\",\"commit\":\"" + FOX_CURRENT_DEV_STR                                +
                     "\",\"date\":\"" + DataManager::GetStrValue("FOX_BUILD_DATE_REAL")    +
                   "\",\"branch\":\"" + FOX_BRANCH                                         +
                  "\",\"variant\":\"" + FOX_VARIANT                                        +
               "\",\"release_id\":\"" + TWFunc::System_Property_Get("ro.build.id")         + "\"}");

    copy_file("/tmp/recovery.log", Logs_Dir + "/lastrecoverylog.log", 0777);
    TWFunc::set_media_rw_permissions(Logs_Dir);
    TWFunc::set_media_rw_permissions(Logs_Dir + "/lastrecoverylog.log");
    TWFunc::set_media_rw_permissions(Logs_Dir + "/releaseinfo.json");

// set permissions and selinux contexts on reboot
    TWFunc::update_permissions_on_reboot();

// don't backup historic logs
#ifdef OF_DONT_KEEP_LOG_HISTORY
	return;
#endif

    // if decryption failed, don't backup historic logs
    if (failed_decryption) {
	#ifdef FOX_MISCELLANEOUS_ROOT_DIRECTORY
	std::string tmp1 = FOX_MISCELLANEOUS_ROOT_DIRECTORY;
	if (tmp1.find("/sdcard/") != string::npos) {
		// if we're trying to write to /sdcard with decryption failure, bail out
		return;
	}
	#endif

	#ifdef FOX_USE_DATA_RECOVERY_FOR_SETTINGS
		// we aren't writing to /sdcard, so continue
	#else
		return;
	#endif
    }

    // proceed
    struct timeval tv;
    std::string log_file = "/recovery";
    if (gettimeofday(&tv, NULL) == 0)
     {
        std::string tmp = strReturnCurrentTime();
        log_file = log_file + "_" + tmp + ".log";
     }
   else
     {
         log_file = log_file + "_undated.log";
     }

   log_file = Logs_Dir + log_file;
   copy_file("/tmp/recovery.log", log_file, 0777);
   if (Path_Exists(Fox_Bin_Dir + "/pigz"))
     {
        string cmd = Fox_Bin_Dir + "/pigz -K --best " + log_file;
        Exec_Cmd (cmd);
        TWFunc::set_media_rw_permissions(log_file + ".zip");
     }
}

/* Execute a command */


/* run a command and return its output */
string TWFunc::Exec_With_Output(const string &cmd)
{
  string data;
  FILE *stream;
  const int max_buffer = 256;
  char buffer[max_buffer];
  string s = cmd + " 2>&1";

  stream = popen(s.c_str(), "r");
  if (stream)
    {
      while (!feof(stream))
	{
	  if (fgets(buffer, max_buffer, stream) != NULL)
	     data.append(buffer);
	}
      pclose(stream);
      return (Trim_Trailing_NewLine (data));
    }
 else 
    return exec_error_str;
}


bool TWFunc::Is_SymLink(string Path) {
  struct stat st;
  if ((lstat(Path.c_str(), &st) == 0) && (S_ISLNK(st.st_mode)))
     return true;
  else
     return false;
}


bool TWFunc::Wait_For_Battery(std::chrono::nanoseconds timeout) {
	std::string battery_path;
#ifdef TW_CUSTOM_BATTERY_PATH
	battery_path = EXPAND(TW_CUSTOM_BATTERY_PATH);
#else
	battery_path = "/sys/class/power_supply/battery";
#endif
	if (!battery_path.empty()) return TWFunc::Wait_For_File(battery_path, timeout);

	return false;
}


string TWFunc::wstr_to_str(wstring wstr) {
  using convert_type = std::codecvt_utf8<wchar_t>;
  std::wstring_convert<convert_type, wchar_t> converter;

  return converter.to_bytes(wstr);
}


string TWFunc::Product_Property_Get(string Prop_Name) {
	return Product_Property_Get(Prop_Name, PartitionManager, "product", "build.prop");
}


string TWFunc::Product_Property_Get(string Prop_Name, TWPartitionManager &PartitionManager, string Mount_Point, string prop_file_name) {
	bool mount_state = PartitionManager.Is_Mounted_By_Path(Mount_Point);
	std::vector<string> buildprop;
	string propvalue;
	if (!PartitionManager.Mount_By_Path(Mount_Point, false))
		return propvalue;
	string prop_file = Mount_Point + "/etc/" + prop_file_name;
	if (!TWFunc::Path_Exists(prop_file)) {
		LOGINFO("Unable to locate file: %s\n", prop_file.c_str());
		return propvalue;
	}
	if (TWFunc::read_file(prop_file, buildprop) != 0) {
		LOGINFO("Unable to open %s for getting '%s'.\n", prop_file_name.c_str(), Prop_Name.c_str());
		DataManager::SetValue(TW_BACKUP_NAME, Get_Current_Date());
		if (!mount_state)
			PartitionManager.UnMount_By_Path(Mount_Point, false);
		return propvalue;
	}
	int line_count = buildprop.size();
	int index;
	size_t start_pos = 0, end_pos;
	string propname;
	for (index = 0; index < line_count; index++) {
		end_pos = buildprop.at(index).find("=", start_pos);
		propname = buildprop.at(index).substr(start_pos, end_pos);
		if (propname == Prop_Name) {
			propvalue = buildprop.at(index).substr(end_pos + 1, buildprop.at(index).size());
			if (!mount_state)
				PartitionManager.UnMount_By_Path(Mount_Point, false);
			return propvalue;
		}
	}
	if (!mount_state)
		PartitionManager.UnMount_By_Path(Mount_Point, false);
	return propvalue;
}


string TWFunc::Vendor_Property_Get(string Prop_Name) {
	return Vendor_Property_Get(Prop_Name, PartitionManager, "vendor", "build.prop");
}


string TWFunc::Vendor_Property_Get(string Prop_Name, TWPartitionManager &PartitionManager, string Mount_Point, string prop_file_name) {
	bool mount_state = PartitionManager.Is_Mounted_By_Path(Mount_Point);
	std::vector<string> buildprop;
	string propvalue;
	if (!PartitionManager.Mount_By_Path(Mount_Point, false))
		return propvalue;
	string prop_file = Mount_Point + "/" + prop_file_name;
	if (!TWFunc::Path_Exists(prop_file)) {
		LOGINFO("Unable to locate file: %s\n", prop_file.c_str());
		return propvalue;
	}
	if (TWFunc::read_file(prop_file, buildprop) != 0) {
		LOGINFO("Unable to open %s for getting '%s'.\n", prop_file_name.c_str(), Prop_Name.c_str());
		DataManager::SetValue(TW_BACKUP_NAME, Get_Current_Date());
		if (!mount_state)
			PartitionManager.UnMount_By_Path(Mount_Point, false);
		return propvalue;
	}
	int line_count = buildprop.size();
	int index;
	size_t start_pos = 0, end_pos;
	string propname;
	for (index = 0; index < line_count; index++) {
		end_pos = buildprop.at(index).find("=", start_pos);
		propname = buildprop.at(index).substr(start_pos, end_pos);
		if (propname == Prop_Name) {
			propvalue = buildprop.at(index).substr(end_pos + 1, buildprop.at(index).size());
			if (!mount_state)
				PartitionManager.UnMount_By_Path(Mount_Point, false);
			return propvalue;
		}
	}
	if (!mount_state)
		PartitionManager.UnMount_By_Path(Mount_Point, false);
	return propvalue;
}


std::string TWFunc::File_Property_Get(const std::string File_Path, const std::string Prop_Name) {
	std::vector <string> buildprop;
	std::string propname, propvalue;
	if (TWFunc::read_file(File_Path, buildprop) != 0) {
		return propvalue;
	}
	int line_count = buildprop.size();
	int index;
	size_t start_pos = 0, end_pos;
	for (index = 0; index < line_count; index++) {
		end_pos = buildprop.at(index).find("=", start_pos);
		propname = buildprop.at(index).substr(start_pos, end_pos);
		if (propname == Prop_Name) {
			propvalue = buildprop.at(index).substr(end_pos + 1, buildprop.at(index).size());
			return propvalue;
		}
	}
	return propvalue;
}


void TWFunc::Disable_Stock_Recovery_Replace_Func(void)
{
     if (DataManager::GetIntValue(FOX_DONT_REPLACE_STOCK) == 1)
      	return;

     usleep(128);
     if ((DataManager::GetIntValue(FOX_ADVANCED_STOCK_REPLACE) == 1) 
      ||  (Fox_Force_Deactivate_Process == 1))
	{
      	  bool we_mounted = false;
      	  bool we_mounted_sys = false;
      	  string thedir = "/system";
          string rootdir = PartitionManager.Get_Android_Root_Path();

      	// system-as-root stuff
      	  bool Is_SysRoot = Has_System_Root();
      	  if (Is_SysRoot)
            {
	       if (TWFunc::Path_Exists(rootdir + "/system") && TWFunc::Path_Exists(rootdir + "/system/etc"))
	         {
                     rootdir = rootdir + "/system";
                 }
               else
              	 {
            	   if (!PartitionManager.Is_Mounted_By_Path("/system"))
             	      {
                	if (PartitionManager.Mount_By_Path("/system", false))
                	   {
                              we_mounted_sys = true;
                   	   }
             	      }
            	   if ((PartitionManager.Is_Mounted_By_Path("/system")) && (TWFunc::Path_Exists("/system/system")))
            	     {
                	rootdir = "/system/system";
                     }
              	 }
            }
          else // it is not system-as-root
            {
              if (rootdir != "/system")
                {
	           if (rootdir != "/")
	              rootdir = rootdir + "/";
	       
	           if (!PartitionManager.Is_Mounted_By_Path(rootdir + "system"))
	              {
	           	if (PartitionManager.Mount_By_Path(rootdir + "system", false))
	              	   {
	                	we_mounted_sys = true;
	                	thedir = rootdir + "system";
	                   }
	              }

	       	   if (TWFunc::Path_Exists(rootdir + "system") && TWFunc::Path_Exists(rootdir + "system/etc"))
	              {
                          rootdir = rootdir + "system";
                      }
                }
            }
     	// system-as-root stuff //

          LOGINFO("OrangeFox: Disabling stock recovery [search-dir=%s]...\n", rootdir.c_str());

	// using rootdir/ as determined here
	  usleep(512);
	  if (TWFunc::Path_Exists(rootdir))
	    {
          	LOGINFO("OrangeFox: checking %s ...\n", rootdir.c_str());
	  	if (Path_Exists(rootdir + "/bin/install-recovery.sh"))
	      		Rename_File(rootdir + "/bin/install-recovery.sh",
		     	  	rootdir + "/bin/wlfx0install-recoverybak0xwlf");

	  	if (Path_Exists(rootdir + "/etc/install-recovery.sh"))
	      		Rename_File(rootdir + "/etc/install-recovery.sh",
		   	 	rootdir + "/etc/wlfx0install-recoverybak0xwlf");

	  	if (Path_Exists(rootdir + "/etc/recovery-resource.dat"))
	      		Rename_File(rootdir + "/etc/recovery-resource.dat",
		   	  	rootdir + "/etc/wlfx0recovery-resource0xwlf");

          	if (Path_Exists(rootdir + "/recovery-from-boot.p"))
  	     	   {
	         	Rename_File(rootdir + "/recovery-from-boot.p",
		      	     rootdir + "/wlfx0recovery-from-boot.bak0xwlf");
	          	sync();
	     	   }
	     }

	// using hardcoded /system/vendor/
	  we_mounted = false;
	  
	  if (!Is_SymLink("/system") && !PartitionManager.Is_Mounted_By_Path("/system"))
             {
               if (PartitionManager.Mount_By_Path("/system", false))
                 {
                    we_mounted = true;
                 }
             }

	  if (!Is_SymLink("/system") && PartitionManager.Is_Mounted_By_Path("/system"))
	     {
          	LOGINFO("OrangeFox: checking /system ...\n");
	  	if (Path_Exists("/system/vendor/bin/install-recovery.sh"))
	    		rename("/system/vendor/bin/install-recovery.sh",
		   		"/system/vendor/bin/wlfx0install-recoverybak0xwlf");

	  	if (Path_Exists("/system/vendor/etc/install-recovery.sh"))
	    		rename("/system/vendor/etc/install-recovery.sh",
		   		"/system/vendor/etc/wlfx0install-recoverybak0xwlf");

	  	if (Path_Exists("/system/vendor/etc/recovery-resource.dat"))
	    		rename("/system/vendor/etc/recovery-resource.dat",
		   		"/system/vendor/etc/wlfx0recovery-resource0xwlf");

          	if (Path_Exists("/system/vendor/recovery-from-boot.p"))
  	     	   {
	         	Rename_File("/system/vendor/recovery-from-boot.p",
		      	     "/system/vendor/wlfx0recovery-from-boot.bak0xwlf");
	     	   }

	  	usleep(512);	  	
	  	if (we_mounted) // cleanup
	     	    PartitionManager.UnMount_By_Path("/system", false);
	     }

	  usleep(512);
	  if (we_mounted_sys) // cleanup
	    {
	       if (PartitionManager.Is_Mounted_By_Path(thedir))
	          PartitionManager.UnMount_By_Path(thedir, false);
	    }

	// using hardcoded /vendor/
	  usleep(512);
	  we_mounted = false;
	  if (!PartitionManager.Is_Mounted_By_Path("/vendor"))
             {
               if (PartitionManager.Mount_By_Path("/vendor", false))
                 {
                    we_mounted = true;
                 }
             }

	  if (PartitionManager.Is_Mounted_By_Path("/vendor"))
	     {
          	LOGINFO("OrangeFox: checking /vendor ...\n");
	  	if (Path_Exists("/vendor/bin/install-recovery.sh"))
	    		rename("/vendor/bin/install-recovery.sh",
		   	"/vendor/bin/wlfx0install-recoverybak0xwlf");

	  	if (Path_Exists("/vendor/etc/install-recovery.sh"))
	    		rename("/vendor/etc/install-recovery.sh",
		   	"/vendor/etc/wlfx0install-recoverybak0xwlf");

	  	if (Path_Exists("/vendor/etc/recovery-resource.dat"))
	    		rename("/vendor/etc/recovery-resource.dat",
		   	"/vendor/etc/wlfx0recovery-resource0xwlf");

          	if (Path_Exists("/vendor/recovery-from-boot.p"))
  	     	   {
	         	Rename_File("/vendor/recovery-from-boot.p",
		      	     "/vendor/wlfx0recovery-from-boot.bak0xwlf");
	     	   }
	  	
	  	usleep(512);
	  	if (we_mounted) // cleanup
	     	    PartitionManager.UnMount_By_Path("/vendor", false);
	     }
      usleep(64);
      sync();
      }
}

// Disable flashing of stock recovery



bool TWFunc::CheckWord(std::string filename, std::string search)
{
  std::string line;
  ifstream File;
  File.open(filename);
  if (File.is_open())
    {
      while (!File.eof())
	{
	  std::getline(File, line);
	  if (line.find(search) != string::npos)
	    {
	      File.close();
	      return true;
	    }
	}
      File.close();
    }
  return false;
}


void TWFunc::Replace_Word_In_File(string file_path, string search,
				  string word)
{
  std::string contents_of_file, local, renamed = file_path + ".wlfx";
  if (TWFunc::Path_Exists(renamed))
    unlink(renamed.c_str());
  std::rename(file_path.c_str(), renamed.c_str());
  std::ifstream old_file(renamed.c_str());
  std::ofstream new_file(file_path.c_str());
  size_t start_pos, end_pos, pos;
  while (std::getline(old_file, contents_of_file))
    {
      start_pos = 0;
      pos = 0;
      end_pos = search.find(";", start_pos);
      while (end_pos != string::npos && start_pos < search.size())
	{
	  local = search.substr(start_pos, end_pos - start_pos);
	  if (contents_of_file.find(local) != string::npos)
	    {
	      while ((pos =
		      contents_of_file.find(local, pos)) != string::npos)
		{
		  contents_of_file.replace(pos, local.length(), word);
		  pos += word.length();
		}
	    }
	  start_pos = end_pos + 1;
	  end_pos = search.find(";", start_pos);
	}
      new_file << contents_of_file << '\n';
    }
  unlink(renamed.c_str());
  chmod(file_path.c_str(), 0644);
}


void TWFunc::Replace_Word_In_File(std::string file_path, std::string search)
{
  std::string contents_of_file, local, renamed = file_path + ".wlfx";
  if (TWFunc::Path_Exists(renamed))
    unlink(renamed.c_str());
  std::rename(file_path.c_str(), renamed.c_str());
  std::ifstream old_file(renamed.c_str());
  std::ofstream new_file(file_path.c_str());
  size_t start_pos, end_pos, pos;
  while (std::getline(old_file, contents_of_file))
    {
      start_pos = 0;
      pos = 0;
      end_pos = search.find(";", start_pos);
      while (end_pos != string::npos && start_pos < search.size())
	{
	  local = search.substr(start_pos, end_pos - start_pos);
	  if (contents_of_file.find(local) != string::npos)
	    {
	      while ((pos =
		      contents_of_file.find(local, pos)) != string::npos)
		contents_of_file.replace(pos, local.length(), "");
	    }
	  start_pos = end_pos + 1;
	  end_pos = search.find(";", start_pos);
	}
      new_file << contents_of_file << '\n';
    }
  unlink(renamed.c_str());
  chmod(file_path.c_str(), 0644);
}


void TWFunc::Remove_Word_From_File(std::string file_path, std::string search)
{
   Replace_Word_In_File(file_path, search);
}


void TWFunc::Set_New_Ramdisk_Property(std::string file_path, std::string prop,
				      bool enable)
{
  if (TWFunc::CheckWord(file_path, prop))
    {
      if (enable)
	{
	  std::string expected_value = prop + "=0";
	  prop += "=1";
	  TWFunc::Replace_Word_In_File(file_path, expected_value, prop);
	}
      else
	{
	  std::string expected_value = prop + "=1";
	  prop += "=0";
	  TWFunc::Replace_Word_In_File(file_path, expected_value, prop);
	}
    }
  else
    {
      ofstream File(file_path.c_str(), std::ios::app);
      if (File.is_open())
	{
	  if (enable)
	    prop += "=1";
	  else
	    prop += "=0";
	  File << prop;
	  File.close();
	}
    }
}


string TWFunc::sdknum_to_text(int sdk) {
const int sdk_asize=11;
   int sdk_num[sdk_asize] =  {29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39};
   string sdk_string[sdk_asize] = {"10","11","12","12L","13","14","15","16","17","18"};
   int i;
   string s = "12"; // default
   for (i = 0; i < sdk_asize; i++) {
	if (sdk == sdk_num[i]) {
		s = sdk_string[i];
		break;
	}
   }
   return "Android " + s;
}


void TWFunc::Welcome_Message(void)
{
   if (Fox_Has_Welcomed > 0) {
    return;
   }
    gui_print("--------------------------\n");
    gui_msg(Msg(msg::kGreen, "fox_welcome=Welcome to OrangeFox Recovery!"));
    gui_msg(Msg("fox_release=[Release]   : {1}")(FOX_BUILD));
    gui_msg(Msg("fox_variant=[Variant]   : {1}")(FOX_VARIANT));
    gui_msg(Msg("fox_codebase=[Codebase]  : {1}, {2}")(Fox_Property_Get("ro.build.version.sdk").c_str())(FOX_CURRENT_DEV_STR));
    gui_print("[Branch]    : %s\n", OF_CURRENT_BRANCH);
#ifdef FOX_SETTINGS_ROOT_DIRECTORY
    gui_msg(Msg("fox_settings=[Settings]  : {1}")(Fox_Settings_Path.c_str()));
#endif
#ifdef FOX_MISCELLANEOUS_ROOT_DIRECTORY
    gui_msg(Msg("fox_misc=[Misc]      : {1}")(Fox_Home.c_str()));
#endif
    gui_msg(Msg("fox_build_date=[Build date]: {1}")(DataManager::GetStrValue("FOX_BUILD_DATE_REAL").c_str()));
    
    if (uppercase(FOX_BUILD) == "UNOFFICIAL")
      	gui_msg(Msg(msg::kWarning, "fox_build_type_unofficial=[Build type]: Unofficial. No official support for unofficial builds"));
    else {
    	gui_msg(Msg("fox_build_type=[Build type]: {1}")(FOX_BUILD_TYPE));
    	if (uppercase(FOX_BUILD_TYPE) == "BETA" || uppercase(FOX_BUILD_TYPE) == "STABLE") {
    	    string tg_link = "https://t.me/OrangeFoxChat";
    	    gui_msg(Msg("fox_support=[Support]   : {1}")(tg_link.c_str()));
    	} else {
    	    gui_msg(Msg(msg::kWarning, "fox_nosupport=[Support]   : No official support for unknown builds"));
    	}
    }
#ifdef OF_ENABLE_LAB
    gui_print_color("error", "\n*** CONFIDENTIAL ALPHA. NOT FOR RELEASE!! ***\n\n");
#endif

    gui_print("\n");
    gui_msg(Msg(msg::kGreen, "fox_websites=OrangeFox websites:"));
    string download_link = "https://orangefox.download/";
    string faq_link = "https://wiki.orangefox.tech/guides/";
    gui_msg(Msg("fox_downloads=[Downloads] : {1}")(download_link.c_str()));
    gui_msg(Msg("fox_faq=[Guides/FAQ]: {1}")(faq_link.c_str()));

    gui_print("--------------------------\n");
    Fox_Has_Welcomed++;
}


void TWFunc::Fox_Set_Current_Device_CodeName(void)
{
  string tmp01 = TWFunc::Fox_Property_Get("ro.product.device");
  string currdev = DataManager::GetStrValue(FOX_COMPATIBILITY_DEVICE);
  string tmp02 = TWFunc::File_Property_Get (Fox_Cfg, "FOX_CURRENT_DEVICE");

  if (!tmp02.empty()) {
    Fox_Current_Device = tmp02;
    //TWFunc::Fox_Property_Set("ro.product.device", tmp02);
  }
  else if (!tmp01.empty() && tmp01 != currdev) {
     Fox_Current_Device = tmp01;
  }
  else Fox_Current_Device = currdev;

  DataManager::SetValue(FOX_COMPATIBILITY_DEVICE, Fox_Current_Device);
  TWFunc::Fox_Property_Set("ro.product.device", Fox_Current_Device);
}


std::string TWFunc::Get_Balanced_Governor(void)
{
  std::string avail_path = "/sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors";
  std::vector<string> governors = {"schedutil", "interactive", "ondemand", "conservative"};
  // Default fallback
  std::string balanced_gov = "ondemand";

  if (TWFunc::Path_Exists(avail_path)) {
	for (auto gov : governors) {
		if (TWFunc::CheckWord(avail_path, gov)) {
			balanced_gov = gov;
			break;
		}
	}
    }

  return balanced_gov;
}


void TWFunc::OrangeFox_Startup(void)
{
  int i;
  std::string cpu_one, cpu_two, a, loaded_password;
  cpu_one = "/sys/devices/system/cpu/cpu";
  cpu_two = "/cpufreq/scaling_governor";
  std::string enable = "1";
  std::string disable = "0";
  std::string t2w = "/sys/android_touch/doubletap2wake";
  std::string fsync = "/sys/module/sync/parameters/fsync_enabled";
  std::string fast_charge = "/sys/kernel/fast_charge/force_fast_charge";
  std::string performance = "performance";
  std::string powersave = "powersave";
  std::string interactive = "interactive";
  std::string kernel_proc_check = "/proc/touchpanel/capacitive_keys_";
  std::string device_one = kernel_proc_check + "enable";
  std::string device_two = kernel_proc_check + "disable";

  //gui_print("DEBUG: - OrangeFox_Startup_Executed=%i\n", OrangeFox_Startup_Executed);
  
  // mark that this function has been called
  DataManager::SetValue("fox_startup_executed", "1");

  // don't repeat this
  if (OrangeFox_Startup_Executed > 0)
     return;

  OrangeFox_Startup_Executed++;

  if (TWFunc::Path_Exists(FOX_PS_BIN)) 
      chmod (FOX_PS_BIN, 0755);
  
  Fox_Current_ROM = "";
  
  TWFunc::Welcome_Message();
  

  if (TWFunc::Path_Exists(device_one))
    TWFunc::write_to_file(device_one, disable);

  if (TWFunc::Path_Exists(device_two))
    TWFunc::write_to_file(device_two, enable);

  if (TWFunc::Path_Exists(t2w))
    {
      if (DataManager::GetIntValue(FOX_T2W_CHECK) == 1)
       {
	   TWFunc::write_to_file(t2w, enable);
       } 
       else
          TWFunc::write_to_file(t2w, disable);
    } 

  if (DataManager::GetIntValue(FOX_FSYNC_CHECK) == 1)
    {
      if (TWFunc::Path_Exists(fsync))
	TWFunc::write_to_file(fsync, disable);
    }

  if (DataManager::GetIntValue(FOX_FORCE_FAST_CHARGE_CHECK) == 1)
    {
      if (TWFunc::Path_Exists(fast_charge))
	{
	  TWFunc::write_to_file(fast_charge, enable);
	}
    }

  if (DataManager::GetIntValue(FOX_PERFORMANCE_CHECK) == 1)
    {
      DataManager::SetValue(FOX_GOVERNOR_STABLE, performance);
      for (i = 0; i < 9; i++)
	{
	  std::string k = to_string(i);
	  a = cpu_one + k + cpu_two;
	  if (TWFunc::Path_Exists(a))
	    TWFunc::write_to_file(a, performance);
	}
    }

  if (DataManager::GetIntValue(FOX_POWERSAVE_CHECK) == 1)
    {
      DataManager::SetValue(FOX_GOVERNOR_STABLE, powersave);
      for (i = 0; i < 9; i++)
	{
	  std::string k = to_string(i);
	  a = cpu_one + k + cpu_two;
	  if (TWFunc::Path_Exists(a))
	    TWFunc::write_to_file(a, powersave);
	}
    }

  if (DataManager::GetIntValue(FOX_BALANCE_CHECK) == 1)
    {
      std::string balance = TWFunc::Get_Balanced_Governor();
      DataManager::SetValue(FOX_GOVERNOR_STABLE, balance);

      for (i = 0; i < 9; i++)
	{
	  std::string k = to_string(i);
	  a = cpu_one + k + cpu_two;
	  if (TWFunc::Path_Exists(a))
	    TWFunc::write_to_file(a, balance);
	}
    }
  //string info = TWFunc::System_Property_Get("ro.build.display.id");
  string info = GetInstalledRom();
  if (info.empty())
    {
      LOGINFO("ROM Status: Is not installed\n");
    }
  else
    {
      LOGINFO("ROM Status: %s\n", info.c_str());
    }

  DataManager::SetValue("fox_home_files_dir", Fox_Home_Files.c_str());

  if (TWFunc::Path_Exists(FFiles_dir.c_str()))
    {
      DataManager::SetValue("fox_resource_dir", FFiles_dir.c_str());
      if (TWFunc::Path_Exists(Fox_sdcard_aroma_cfg)) // is there a backup CFG file on /sdcard/Fox/?
	{
	  if (TWFunc::Path_Exists(Fox_Home_Files + "/AromaFM"))
	     TWFunc::copy_file(Fox_sdcard_aroma_cfg, Fox_aroma_cfg, 0644);
	}
      else
	{
	  if (!Path_Exists(Fox_Home))
	    {
	      if (!Create_Dir_Recursive(Fox_Home,  S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH, AID_MEDIA_RW, AID_MEDIA_RW))
		  LOGINFO("Error making %s directory: %s\n", Fox_Home.c_str(), strerror(errno));
	    }         
	  if (Path_Exists(Fox_Home))
	    {
	      if (Path_Exists(Fox_aroma_cfg))
		TWFunc::copy_file(Fox_aroma_cfg, Fox_sdcard_aroma_cfg, 0644);
	    }
	} // else
    }
  else
    {
      DataManager::SetValue("fox_resource_dir", Fox_Home_Files.c_str());
    }

  if (!Path_Exists(Fox_Settings_Path))
    {
      if (!Create_Dir_Recursive(Fox_Settings_Path,  S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH, AID_MEDIA_RW, AID_MEDIA_RW))
        LOGINFO("Error making %s directory: %s\n", Fox_Settings_Path.c_str(), strerror(errno));
    }

  if (!Path_Exists(Fox_Logs_Dir))
      {
	  TWFunc::Create_Dir_Recursive(Fox_Logs_Dir, 0777, AID_MEDIA_RW, AID_MEDIA_RW);
      }

  TWFunc::Fresh_Fox_Install();

//==== themes version matching
#ifndef FOX_ALLOW_EARLY_SETTINGS_LOAD
  TWFunc::FoxThemeCheck();
#endif
//====
  
  // start mtp manually, if enabled
  #ifdef TW_HAS_MTP
 // if (DataManager::GetIntValue("tw_mtp_enabled") == 1 && !PartitionManager.is_MTP_Enabled())
 //    PartitionManager.Enable_MTP();
  #endif
}


void TWFunc::create_fingerprint_file(string file_path, string fingerprint)
{
  if (TWFunc::Path_Exists(file_path))
    unlink(file_path.c_str());
  ofstream file;
  file.open(file_path.c_str());
  file << fingerprint;
  file.close();
  tw_set_default_metadata(file_path.c_str());
}


bool TWFunc::Verify_Loaded_OTA_Signature(std::string loadedfp,
					 std::string ota_folder)
{
  std::string datafp;
  string ota_info = ota_folder + Fox_OTA_info;
  if (TWFunc::Path_Exists(ota_info))
    {
      if (TWFunc::read_file(ota_info, datafp) == 0)
	{
	  if (!datafp.empty() && datafp.size() > FOX_MIN_EXPECTED_FP_SIZE
	      && !loadedfp.empty()
	      && loadedfp.size() > FOX_MIN_EXPECTED_FP_SIZE
	      && datafp == loadedfp)
	    {
	      return true;
	    }
	}
    }
  return false;
}


bool TWFunc::PackRepackImage_MagiskBoot(bool do_unpack, bool is_boot)
{
  string result, tmpstr, output;
  std::string k = "/";
  std::string cd_dir = "cd ";
  std::string end_command = "; ";
  std::string cpio = "ramdisk.cpio";
  std::string tmp_cpio = Fox_tmp_dir + k + cpio;
  std::string ramdisk_cpio = Fox_ramdisk_dir + k + cpio;
  bool retval = false;
  bool keepverity = false;
  int res = 0;
  std::string cmd_script =  "/tmp/do_magisk-unpack.sh";
  std::string cmd_script2 = "/tmp/do_magisk-repack.sh";
  std::string shebang = "#!/system/bin/sh";

  std::string magiskboot_sbin = Get_MagiskBoot();
  if (!TWFunc::Path_Exists(magiskboot_sbin))
     {
     	LOGERR("TWFunc::PackRepackImage_MagiskBoot: Cannot find magiskboot!");
  	TWFunc::tw_reboot(rb_recovery);
     }
 /*
  if ( (!PartitionManager.Is_Mounted_By_Path(PartitionManager.Get_Android_Root_Path())) 
    && (!PartitionManager.Mount_By_Path(PartitionManager.Get_Android_Root_Path(), false)))
     {
     	LOGERR("TWFunc::PackRepackImage_MagiskBoot: Failed to mount system!");
        return false;
     }
 */
  TWPartition *Boot = PartitionManager.Find_Partition_By_Path("/boot");

#if (defined(AB_OTA_UPDATER) || defined(FOX_AB_DEVICE)) && !defined(OF_AB_DEVICE_WITH_RECOVERY_PARTITION)
  if (Boot != NULL)
    {
       tmpstr = Boot->Actual_Block_Device;
#else 
  TWPartition *Recovery = PartitionManager.Find_Partition_By_Path("/recovery");
  if (Boot != NULL && Recovery != NULL)
    {
      if (is_boot)
	tmpstr = Boot->Actual_Block_Device;
      else
	tmpstr = Recovery->Actual_Block_Device;
#endif
      if (do_unpack) // unpack
	{
	  if (TWFunc::Path_Exists(Fox_tmp_dir))
	      TWFunc::removeDir(Fox_tmp_dir, false);
	    
	  if (TWFunc::Recursive_Mkdir(Fox_ramdisk_dir))
	    {
	        CreateNewFile (cmd_script);
	        chmod (cmd_script.c_str(), 0755);
	        AppendLineToFile (cmd_script, shebang);
	        AppendLineToFile (cmd_script, "LOGINFO() { echo \"$1\"; echo \"$1\" >> /tmp/recovery.log;}");
	        // if we need to backup the script, for debugging
	        if (New_Fox_Installation == 1) 
	         {
	           AppendLineToFile (cmd_script, 
	           "BackUp() { cp -f /tmp/recovery.log " + Fox_Home + "/logs/post-install.log; cp -af " + cmd_script + " " + Fox_Home + "/logs/cmd_script1.log; }");
	         }
	        else 
	           AppendLineToFile (cmd_script, "BackUp() { cp -af " + cmd_script + " " + Fox_Home + "/logs/cmd_script1.log; }");
	        
	        //AppendLineToFile (cmd_script, "abort() { LOGINFO \"$1\"; BackUp; exit 1; }");
	        AppendLineToFile (cmd_script, "abort() { LOGINFO \"$1\"; exit 1; }");
	        AppendLineToFile (cmd_script, "mkdir -p " + Fox_tmp_dir);
	        AppendLineToFile (cmd_script, "mkdir -p " + Fox_ramdisk_dir);
	        AppendLineToFile (cmd_script, cd_dir + Fox_tmp_dir);
	        AppendLineToFile (cmd_script, "LOGINFO \"- Unpacking boot/recovery image - block device=\"");
	        AppendLineToFile (cmd_script, "LOGINFO \"[" + tmpstr + "] \"");
	        AppendLineToFile (cmd_script, magiskboot_sbin + " unpack -h \"" + tmpstr + "\" > /dev/null 2>&1");
	        AppendLineToFile (cmd_script, "[ $? == 0 ] && LOGINFO \"- Succeeded.\" || abort \"- Unpacking image failed.\"");
	        AppendLineToFile (cmd_script, "#");
	        // processing boot image?
		if (is_boot)
		   {
	              AppendLineToFile (cmd_script, cd_dir + Fox_tmp_dir);
		      std::string keepdmverity, keepforcedencryption;
		      if ((DataManager::GetIntValue(FOX_DISABLE_DM_VERITY) == 1)/* || (Fox_Force_Deactivate_Process == 1)*/)
		      	{
		           keepverity = false;
		           keepdmverity = "false ";
		        }
		      	else
		      	{
		           keepverity = true;
		           keepdmverity = "true ";
		        }
		      
		      	if ((DataManager::GetIntValue(FOX_DISABLE_FORCED_ENCRYPTION) == 1)/* || (Fox_Force_Deactivate_Process == 1)*/)
		      	  {
		      	     #ifdef OF_DONT_PATCH_ENCRYPTED_DEVICE
		             if (StorageIsEncrypted())
		                keepforcedencryption = "true";
		             else
		             #endif
		                keepforcedencryption = "false";
		          }
		      	else
		             keepforcedencryption = "true";

	              AppendLineToFile (cmd_script, "cp -af ramdisk.cpio ramdisk.cpio.orig");
	              AppendLineToFile (cmd_script, "LOGINFO \"- Patching ramdisk (verity/encryption) ...\"");
	              AppendLineToFile (cmd_script, magiskboot_sbin + " cpio ramdisk.cpio \"patch " + keepdmverity + keepforcedencryption + "\" > /dev/null 2>&1");
	              AppendLineToFile (cmd_script, "[ $? == 0 ] && LOGINFO \"- Succeeded.\" || abort \"- Ramdisk patch failed.\"");
	              AppendLineToFile (cmd_script, "rm -f ramdisk.cpio.orig");
	              if (keepverity == false)
	                 {
	              	    AppendLineToFile (cmd_script, "[ -f dtb ] && " + magiskboot_sbin + " dtb dtb patch > /dev/null 2>&1");
	              	    AppendLineToFile (cmd_script, "[ -f extra ] && " + magiskboot_sbin + " dtb extra patch > /dev/null 2>&1");	              	    
	                 }
	           } // is_boot

	        // continue processing the rest
	        AppendLineToFile (cmd_script, "#");
	        AppendLineToFile (cmd_script, "mv " + tmp_cpio + " " + ramdisk_cpio);
	        AppendLineToFile (cmd_script, cd_dir + Fox_ramdisk_dir);
	        AppendLineToFile (cmd_script, "LOGINFO \"- Extracting ramdisk files ...\"");
	        /*
	        #ifdef FOX_USE_UPDATED_MAGISKBOOT
	        AppendLineToFile (cmd_script, "/system/bin/cpio -idu < " + ramdisk_cpio);
	        #else
	        AppendLineToFile (cmd_script, magiskboot_sbin + " cpio " + ramdisk_cpio + " extract > /dev/null 2>&1");
	        #endif
	        */
		// prefer system cpio over magiskboot cpio (avoiding cpio bug in some magiskboot versions)
		AppendLineToFile (cmd_script, "/system/bin/cpio -idu < " + ramdisk_cpio);

	        AppendLineToFile (cmd_script, "[ $? == 0 ] && LOGINFO \"- Succeeded.\" || abort \"- Ramdisk file extraction failed.\"");
	        AppendLineToFile (cmd_script, "rm -f " + ramdisk_cpio);
	        
	        AppendLineToFile (cmd_script, "exit 0");
	        res = Exec_Cmd (cmd_script, result);
	        if (res == 0) 
	           retval = true;
	        usleep (128);
	        
		unlink(cmd_script.c_str());
	    } // if
	} // do_unpack
      else // repack
	{
	  	CreateNewFile (cmd_script2);
	  	chmod (cmd_script2.c_str(), 0755);
	        AppendLineToFile (cmd_script2, shebang);
	        AppendLineToFile (cmd_script2, "LOGINFO() { echo \"$1\"; echo \"$1\" >> /tmp/recovery.log;}");
	        // if we need to backup the script, for debugging	        
	        if (New_Fox_Installation == 1) 
	         {
	           AppendLineToFile (cmd_script2, 
	           "BackUp() { cp -af /tmp/recovery.log " + Fox_Home + "/logs/post-install.log; cp -f " + cmd_script2 + " " + Fox_Home + "/logs/cmd_script2.log; }");
	         }
	        else
	           AppendLineToFile (cmd_script2, "BackUp() { cp -f " + cmd_script2 + " " + Fox_Home + "/logs/cmd_script2.log; }");

	        //AppendLineToFile (cmd_script2, "abort() { LOGINFO \"$1\"; BackUp; exit 1; }");
	        AppendLineToFile (cmd_script2, "abort() { LOGINFO \"$1\"; exit 1; }");
	        AppendLineToFile (cmd_script2, cd_dir + Fox_ramdisk_dir);
	        AppendLineToFile (cmd_script2, "LOGINFO \"- Archiving ramdisk.cpio ...\"");
	        AppendLineToFile (cmd_script2, "find | cpio -o -H newc > \"" + tmp_cpio + "\"");
	        AppendLineToFile (cmd_script2, "[ $? == 0 ] && LOGINFO \"- Succeeded.\" || abort \"- Archiving of ramdisk.cpio failed.\"");
	        AppendLineToFile (cmd_script2, cd_dir + Fox_tmp_dir);

	        AppendLineToFile (cmd_script2, "LOGINFO \"- Repacking boot/recovery image ...\"");
	        AppendLineToFile (cmd_script2, magiskboot_sbin + " repack \"" + tmpstr + "\" > /dev/null 2>&1");
	        AppendLineToFile (cmd_script2, "[ $? == 0 ] && LOGINFO \"- Succeeded.\" || abort \"- Repacking of image failed.\"");

	        /*
	        // work around problems with the new magiskboot on A-only devices - patch the AVBv2 footer
	        if (Magiskboot_Repack_Patch_VBMeta() && is_boot == false)
	           AppendLineToFile (cmd_script2, magiskboot_sbin + " hexpatch new-boot.img 0000000300000000617662746f6f6c 0000000000000000617662746f6f6c > /dev/null 2>&1");
		*/
	        AppendLineToFile (cmd_script2, "LOGINFO \"- Flashing repacked image ...\"");
	        #if defined(AB_OTA_UPDATER) || defined(FOX_AB_DEVICE)
	        AppendLineToFile (cmd_script2, "dd if=new-boot.img of=" + tmpstr + " > /dev/null 2>&1");
	        #else
	        AppendLineToFile (cmd_script2, "flash_image \"" +  tmpstr + "\" new-boot.img");
	        #endif
	        AppendLineToFile (cmd_script2, "[ $? == 0 ] && LOGINFO \"- Succeeded.\" || abort \"- Flashing repacked image failed.\"");
	        AppendLineToFile (cmd_script2, magiskboot_sbin + " cleanup > /dev/null 2>&1");

	        AppendLineToFile (cmd_script2, "exit 0");
	        res = Exec_Cmd (cmd_script2, result);
		usleep (128);
		
		unlink(cmd_script2.c_str());
		
	        if (res == 0) 
	          retval = true;
	  	TWFunc::removeDir(Fox_tmp_dir, false);
	}
    } // boot != null
    else
    {
        LOGERR("TWFunc::PackRepackImage_MagiskBoot: Failed to mount boot/recovery!");
    }
  PartitionManager.UnMount_By_Path(PartitionManager.Get_Android_Root_Path(), false);
  return retval;
}


void TWFunc::Read_Write_Specific_Partition(string path, string partition_name,
					   bool backup) // credits PBRP
{
  TWPartition *Partition =
    PartitionManager.Find_Partition_By_Path(partition_name);
  if (Partition == NULL || Partition->Current_File_System != "emmc")
    {
      LOGERR("Read_Write_Specific_Partition: Unable to find %s\n",
	     partition_name.c_str());
      return;
    }
  string Read_Write, oldfile, null;
  unsigned long long Remain, Remain_old;
  oldfile = path + ".bak";
  if (backup)
    Read_Write = "dump_image " + Partition->Actual_Block_Device + " " + path;
  else
    {
      Read_Write =
	"flash_image " + Partition->Actual_Block_Device + " " + path;
      if (TWFunc::Path_Exists(oldfile))
	{
	  Remain_old = TWFunc::Get_File_Size(oldfile);
	  Remain = TWFunc::Get_File_Size(path);
	  if (Remain_old < Remain)
	    {
	      return;
	    }
	}
      TWFunc::Exec_Cmd(Read_Write, null);
      return;
    }
  if (TWFunc::Path_Exists(path))
    unlink(path.c_str());
  TWFunc::Exec_Cmd(Read_Write, null);
  return;
}



string TWFunc::Load_File(string extension)
{
  string line, path = split_img + "/" + extension;
  ifstream File;
  File.open(path);
  if (File.is_open())
    {
      getline(File, line);
      File.close();
    }
  return line;
}

/* DJ9 */


/* DJ9 */
std::string DataToHexString(char *data, const int len)
{
    std::stringstream ss;
    ss<<std::hex;
    for(int i(0);i<len;++i)
        ss<<(int)(data[i] & 0xff);
    return ss.str();
}


std::string GetFileHeaderMagic (string fname)
{
  FILE *f = fopen(fname.c_str(), "rb");
  char head[2];
  int len = sizeof(head);
  size_t read_len;
  if (!f)
  {
     return "00";
  }
  memset (head, 0, len);
  read_len = fread(head, 1, len, f);
  fclose (f);
  return DataToHexString(head, len);
}


bool TWFunc::Repack_Image(string mount_point)
{
  bool is_boot = (mount_point == "/boot");
  return (PackRepackImage_MagiskBoot(false, is_boot));
}


bool TWFunc::Unpack_Image(string mount_point)
{
  bool is_boot = (mount_point == "/boot");
  return (PackRepackImage_MagiskBoot(true, is_boot));
}


bool TWFunc::Fresh_Fox_Install()
{
  std::string fox_file = get_log_dir() + "recovery/Fox_Installed";
  bool CanProceed = true;
  New_Fox_Installation = 0;
  std::string build_theme_ver = DataManager::GetStrValue("fox_theme_version");
  if (build_theme_ver.empty())
     build_theme_ver = "0";

  if (get_log_dir() == CACHE_LOGS_DIR)
    {
      CanProceed = (PartitionManager.Is_Mounted_By_Path(CACHE_LOGS_DIR) 
                 || PartitionManager.Mount_By_Path(CACHE_LOGS_DIR, true));
    }

  if (CanProceed)
    {
	if (!Path_Exists(fox_file))
	    return false;

	unlink(fox_file.c_str());
	
  	DataManager::SetValue("first_start", "1");
  	DataManager::SetValue("of_themes_version", build_theme_ver);

	#ifdef OF_QUICK_BACKUP_LIST
  	DataManager::SetValue("tw_backup_list_quick", OF_QUICK_BACKUP_LIST);
	#endif

	#ifdef OF_DONT_PATCH_ON_FRESH_INSTALLATION
	    gui_print("Fresh OrangeFox installation - not running the dm-verity/forced-encryption patches\n");
	#else
	    New_Fox_Installation = 1;
	    gui_print("Fresh OrangeFox installation - about to run the dm-verity/forced-encryption patches\n");
     	    if (Fox_Current_ROM_IsMIUI == 1)
     	       {
		  Fox_Force_Deactivate_Process = 1;
		  DataManager::SetValue(FOX_FORCE_DEACTIVATE_PROCESS, 1);
	       }
	    TWFunc::Deactivation_Process();
	    usleep(16384);
	    TWFunc::Patch_AVB20(false);
	    usleep(16384);
	    New_Fox_Installation = 0;
	#endif // OF_DONT_PATCH_ON_FRESH_INSTALLATION

	LOGINFO ("DEBUG [Fresh_Fox_Install()] - copying log to:%s/logs/post-install.log \n", Fox_Home.c_str());
	copy_file("/tmp/recovery.log",  Fox_Home + "/logs/post-install.log", 0644);

	return true;
   }    
   else
   {
      	if (Path_Exists(fox_file)) {
      	    unlink(fox_file.c_str());
      	    DataManager::SetValue("of_themes_version", build_theme_ver);
      	 }
      	return false;
   }
}



void TWFunc::Patch_Verity_Flags(string path)
{
   TWFunc::Replace_Word_In_File(path, "ro.config.dmverity=true;", "ro.config.dmverity=false");
   usleep(64000); 
   if (TWFunc::CheckWord(path, "ro.config.dmverity=true"))
   {
      string root = Get_Root_Path (path);
      if ((root == "/vendor") || (root == PartitionManager.Get_Android_Root_Path()))
      {
        LOGINFO("OrangeFox: Patch_Encryption_Flags: trying again...\n");
	int res;
	string result;
	string cmd_script = "/tmp/dmver.sh";
   	CreateNewFile (cmd_script);
   	chmod (cmd_script.c_str(), 0755);
   	AppendLineToFile (cmd_script, "#!/system/bin/sh");
   	AppendLineToFile (cmd_script, "mount -o rw,remount " + root);
   	AppendLineToFile (cmd_script, "mount -o rw,remount " + root + " " + root);
   	AppendLineToFile (cmd_script, "sed -i -e \"s|ro.config.dmverity=true|ro.config.dmverity=false|g\" " + path);
   	AppendLineToFile (cmd_script, "umount " + root + " > /dev/null 2>&1");
    	//AppendLineToFile (cmd_script, "chmod 0644 " + path);  	
   	AppendLineToFile (cmd_script, "");
   	AppendLineToFile (cmd_script, "exit 0");
   	res = Exec_Cmd (cmd_script, result);
   	unlink(cmd_script.c_str());
      }    
  }
}


bool TWFunc::Fstab_Has_Verity_Flag(std::string path)
{
    if (
       (TWFunc::CheckWord(path, "verify")) 
    || (TWFunc::CheckWord(path, "support_scfs"))
    || (TWFunc::CheckWord(path, "avb"))
       )
        return true;
   else
        return false;
}


bool TWFunc::Fstab_Has_Encryption_Flag(std::string path)
{
   if (
        (CheckWord(path, "forceencrypt")) 
     || (CheckWord(path, "forcefdeorfbe"))
     || (CheckWord(path, "fileencryption"))
//     || (CheckWord(path, "errors=panic")) 
//     || (CheckWord(path, "discard"))
      )
        return true;
   else
        return false;
}



void TWFunc::Patch_Encryption_Flags(std::string path)
{
   LOGINFO("OrangeFox: Patch_Encryption_Flags: processing file:%s\n", path.c_str());
   TWFunc::Replace_Word_In_File(path, "fileencryption=ice;", "encryptable=footer");
   TWFunc::Replace_Word_In_File(path, "forcefdeorfbe=;forceencrypt=;fileencryption=;", "encryptable=");
   usleep(64000); 
   if (Fstab_Has_Encryption_Flag(path))
   {
      string root = Get_Root_Path (path);
      if ((root == "/vendor") || (root == PartitionManager.Get_Android_Root_Path()))
      {
        LOGINFO("OrangeFox: Patch_Encryption_Flags: trying again...\n");
	int res;
	string result;
	string cmd_script = "/tmp/fenc.sh";
   	CreateNewFile (cmd_script);
   	chmod (cmd_script.c_str(), 0755);
   	AppendLineToFile (cmd_script, "#!/system/bin/sh");
   	AppendLineToFile (cmd_script, "mount -o rw,remount " + root);
   	AppendLineToFile (cmd_script, "mount -o rw,remount " + root + " " + root);
   	AppendLineToFile (cmd_script, "sed -i -e \"s|fileencryption=ice|encryptable=footer|g\" " + path);
   	AppendLineToFile (cmd_script, "sed -i -e \"s|forcefdeorfbe=|encryptable=|g\" " + path);
   	AppendLineToFile (cmd_script, "sed -i -e \"s|forceencrypt=|encryptable=|g\" " + path);
   	AppendLineToFile (cmd_script, "sed -i -e \"s|fileencryption=|encryptable=|g\" " + path);
   	AppendLineToFile (cmd_script, "umount " + root + " > /dev/null 2>&1");
   	AppendLineToFile (cmd_script, "");
   	AppendLineToFile (cmd_script, "exit 0");
   	res = Exec_Cmd (cmd_script, result);
   	unlink(cmd_script.c_str());
      }    
   }
//   string remove = "errors=panic,;errors=panic;discard,;,discard;";
//   TWFunc::Replace_Word_In_File(path, remove);
}


void TWFunc::PrepareToFinish(void)
{
   // unmount stuff
   if (PartitionManager.Is_Mounted_By_Path("/vendor"))
	PartitionManager.UnMount_By_Path("/vendor", false);
   //else 
   if (PartitionManager.Is_Mounted_By_Path("/cust"))
	PartitionManager.UnMount_By_Path("/cust", false);
  
   if (PartitionManager.Is_Mounted_By_Path(PartitionManager.Get_Android_Root_Path()))
     PartitionManager.UnMount_By_Path(PartitionManager.Get_Android_Root_Path(), false);
  //
  
  Fox_Zip_Installer_Code = DataManager::GetIntValue(FOX_ZIP_INSTALLER_CODE);
  Fox_Force_Deactivate_Process = DataManager::GetIntValue(FOX_FORCE_DEACTIVATE_PROCESS);

  // increment value, to show how many times we have called this
  Fox_IsDeactivation_Process_Called++;

  // Check AromaFM Config
  if (
     (DataManager::GetIntValue(FOX_SAVE_LOAD_AROMAFM) == 1)
  && (PartitionManager.Mount_By_Path("/sdcard", false))
     )
    {
      string aromafm_path = Fox_Home;
      string aromafm_file = aromafm_path + "/aromafm.cfg";
      if (!Path_Exists(aromafm_path))
	{
	  if (mkdir
	      (aromafm_path.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH))
	    {
	      LOGERR("Error creating %s directory: %s\n", aromafm_path.c_str(), strerror(errno));
	    }
	}

      // Save AromaFM config (AromaFM.cfg)
      if (Path_Exists(Fox_aroma_cfg))
      	 {
      	   if (copy_file(Fox_aroma_cfg, aromafm_file, 0644))
	      {
	         LOGERR("Error copying AromaFM config\n");
	      }
      	 }
      PartitionManager.UnMount_By_Path("/sdcard", false);
    }

  // restore the stock recovery ?
  #ifndef FOX_VANILLA_BUILD
  if (
     (DataManager::GetIntValue(FOX_DONT_REPLACE_STOCK) == 1)
  && (PartitionManager.Mount_By_Path(PartitionManager.Get_Android_Root_Path(), false))
     )
    {
      if (Path_Exists("/system/wlfx0recovery-from-boot.bak0xwlf"))
	{
	  rename("/system/wlfx0recovery-from-boot.bak0xwlf",
		 "/system/recovery-from-boot.p");
	}
      else if (Path_Exists("/system/system/wlfx0recovery-from-boot.bak0xwlf"))
	{
	  rename("/system/system/wlfx0recovery-from-boot.bak0xwlf",
		 "/system/system/recovery-from-boot.p");
	}
	
      PartitionManager.UnMount_By_Path(PartitionManager.Get_Android_Root_Path(), false);
    }
   #endif
}


bool TWFunc::DontPatchBootImage(void)
{
  // check whether to patch on new OrangeFox installations 
  if (New_Fox_Installation == 1)
     { 
        if (Is_AB_Device()) // don't patch the boot image of A/B devices at post-install stage
        {
           return true;
        }

        if ((DataManager::GetIntValue(FOX_DISABLE_DM_VERITY) != 1) 
        && (DataManager::GetIntValue(FOX_DISABLE_FORCED_ENCRYPTION) != 1))
           {  // if we get here, the user has turned off these settings manually
            return true;
           }
     }

   // proceed with other checks
   Fox_Force_Deactivate_Process = DataManager::GetIntValue(FOX_FORCE_DEACTIVATE_PROCESS);
   if (
          (Fox_Force_Deactivate_Process == 1) || 
          (DataManager::GetIntValue(FOX_DISABLE_DM_VERITY) == 1) || 
          (DataManager::GetIntValue(FOX_DISABLE_FORCED_ENCRYPTION) == 1)
      )
      return false;
   else
     {
        return true;
     }
}


std::string TWFunc::Get_Version_From_Service(std::string name) {
	int start, end;
	start = name.find('@') + 1;
	end = name.find("-") - start;
	return name.substr(start, end);
}


void TWFunc::Deactivation_Process(void)
{
  if (TWFunc::To_Skip_OrangeFox_Process())
     {
	LOGINFO("\nOrangeFox: Skipping the OrangeFox Process.\n");
	New_Fox_Installation = 0;
	Fox_Force_Deactivate_Process = 0;
	DataManager::SetValue(FOX_FORCE_DEACTIVATE_PROCESS, 0);
	return;
     }

  // don't call this on first boot following fresh installation
  if (New_Fox_Installation != 1)
     {
         PrepareToFinish();
     }
   
  // advanced stock replace
  Disable_Stock_Recovery_Replace();

// patch ROM's fstab
  
// Should we skip the boot image patches?
  if (DontPatchBootImage() == true)
     {
	LOGINFO("OrangeFox: skipping patching of boot image on device: %s\n", Fox_Current_Device.c_str());
	New_Fox_Installation = 0;
        Fox_Force_Deactivate_Process = 0;
        DataManager::SetValue(FOX_FORCE_DEACTIVATE_PROCESS, 0);	
        return;
     }   
// end
  
  Fox_Force_Deactivate_Process = 0;
  DataManager::SetValue(FOX_FORCE_DEACTIVATE_PROCESS, 0);
}


void TWFunc::Patch_AVB20(bool silent)
{
#if defined(OF_PATCH_AVB20) && !defined(OF_SKIP_ORANGEFOX_PROCESS) && !defined(FOX_VANILLA_BUILD) && !defined(FOX_AB_DEVICE) && !defined(AB_OTA_UPDATER)
std::string zipname = FFiles_dir + "/OF_avb20/OF_avb20.zip";
int res=0, wipe_cache=0;
std::string magiskboot = TWFunc::Get_MagiskBoot();

  if (DataManager::GetIntValue(FOX_ADVANCED_STOCK_REPLACE) != 1) {
        gui_print("- NOTE: you have disabled the stock recovery deactivation feature.\n- Your ROM's recovery will now probably overwrite OrangeFox!\n");
  	return;
  }

  if (!TWFunc::Path_Exists(magiskboot))
     {
        gui_print("ERROR - cannot find magiskboot\n");
  	return;
     }

   if (!TWFunc::Path_Exists(zipname))
     {
        gui_print("ERROR - cannot find %s\n", zipname.c_str());
  	return;
     }

   DataManager::SetValue(FOX_INSTALL_PREBUILT_ZIP, "1");
 
   if (silent)
     setenv("AVB_REPORT_PROGRESS", "0", 1);
   else
     setenv("AVB_REPORT_PROGRESS", "1", 1);
   usleep(4096);
   res = TWinstall_zip(zipname.c_str(), &wipe_cache);
   usleep(4096);
   setenv("AVB_REPORT_PROGRESS", "", 1);
   DataManager::SetValue(FOX_INSTALL_PREBUILT_ZIP, "0");
#endif
}


bool TWFunc::Has_System_Root(void)
{
 string info = TWFunc::System_Property_Get("ro.build.system_root_image");
 return (info == "true");
}


int TWFunc::Rename_File(std::string oldname, std::string newname)
{
   return rename(oldname.c_str(), newname.c_str());
}


int TWFunc::Get_Android_SDK_Version(void)
{
int sdkver = 29;
string sdkverstr = TWFunc::System_Property_Get("ro.build.version.sdk");

 if (sdkverstr.empty())
    sdkverstr = TWFunc::System_Property_Get("ro.system.build.version.sdk");

 if (sdkverstr.empty())
    sdkverstr = TWFunc::Fox_Property_Get("orangefox.rom.sdk");

 if (!sdkverstr.empty()) {
      sdkver = atoi(sdkverstr.c_str());
  }

 return sdkver;
}


string TWFunc::Get_MagiskBoot(void)
{
string s = Fox_Bin_Dir + "/magiskboot";
  if (TWFunc::Path_Exists(s))
     return s;
  s = "/sbin/magiskboot";
  if (TWFunc::Path_Exists(s))
     return s;
  else
     return "magiskboot";
}

// hopefully, this function will be obsolete one day ... //


// hopefully, this function will be obsolete one day ... //
void TWFunc::Setup_Verity_Forced_Encryption(void) {
  DataManager::SetValue(FOX_DISABLE_DM_VERITY, "0");
  DataManager::SetValue(FOX_DISABLE_FORCED_ENCRYPTION, "0");
  #ifdef FOX_VANILLA_BUILD
  DataManager::SetValue(FOX_ADVANCED_STOCK_REPLACE, "0");
  #endif
}


void TWFunc::Dump_Current_Settings(void)
{
  // now just a placeholder
  return;
}


void TWFunc::Reset_Clock(void)
{
#ifdef QCOM_RTC_FIX
   string fox_build_date_utc = TWFunc::File_Property_Get (Fox_Cfg, "ro.build.date.utc_fox");
   if (!fox_build_date_utc.empty())
      {
        TWFunc::Exec_With_Output("date -s \"@" + fox_build_date_utc + "\" > /dev/null");
      }
#endif
}


bool TWFunc::Check_OrangeFox_Overwrite_FromROM(bool WarnUser, const std::string name)
{
#ifndef OF_CHECK_OVERWRITE_ATTEMPTS
   return false;
#else
  // turn on debug screen (for OTA)
  DataManager::SetValue("ota_new_screen", "1");
  
  // proceed
  if (WarnUser)
    {
      int i;
      int j = 40;
      int k = 5;
      gui_print_color("error", "\nALERT!\nThis ROM (%s) wants to overwrite your recovery partition!!!\n\n", name.c_str());
      gui_print_color("error", "I will pause for %i seconds.\n\nTo stop this installation, hard-reboot the device now!\n\n", j);
      gui_print("The %i-second countdown will start in %i seconds ...\n", j, k);
      sleep(k);
      for (i = j; i > 0; i--)
         {
            gui_print("Reboot now! %i seconds left!\n", i);
            sleep(1);
         }
      gui_print_color("error", "\n\nSo, you have chosen to continue with \"%s\"! That seems *very* trusting! Good luck!\n\n", name.c_str());
      return true;
    }
  else
    {
      if (DataManager::GetIntValue("found_fox_overwriting_rom") == 1)
      {
      	gui_print_color("error",
      	"\nALERT!\nThis ROM (%s) may now have overwritten your recovery partition!\n\nGood luck!\n\n", name.c_str());
      }
      DataManager::SetValue("found_fox_overwriting_rom", "0");
      return true;
    }
#endif
}


void TWFunc::AppendLineToFile(string file_path, string line)
{
    std::ofstream file;
    file.open(file_path, std::ios::out | std::ios::app);
    file << line << std::endl;
}


void TWFunc::CreateNewFile(string file_path)
{
  string blank = "";
  string bak = file_path;
  if (TWFunc::Path_Exists(bak))
    unlink(file_path.c_str());
  ofstream file;
  file.open(file_path.c_str());
  file << blank;
  file.close();
  chmod (file_path.c_str(), 0644);
}


bool TWFunc::To_Skip_OrangeFox_Process(void)
{
  #if defined(OF_SKIP_ORANGEFOX_PROCESS) || defined(FOX_VANILLA_BUILD)
     return true;
  #else
     return false;
  #endif
}


string TWFunc::ConvertTime(time_t time)
{
  char buff[32];
  strftime(buff, sizeof(buff), "%d %b %Y | %H:%M", localtime(&time));
  return buff;
}


void TWFunc::UseSystemFingerprint(void)
{
string rom_finger_print = "";
string tmp = "\"";

  rom_finger_print = TWFunc::Fox_Property_Get("orangefox.system.fingerprint");

  if (rom_finger_print.empty()) {
     if (TWFunc::Path_Exists(orangefox_cfg))
       {
	rom_finger_print = File_Property_Get(orangefox_cfg, "ROM_FINGERPRINT");
       }
   }

  if (rom_finger_print.empty())
 	rom_finger_print = System_Property_Get("ro.system.build.fingerprint");

  if (rom_finger_print.empty())
  	rom_finger_print = System_Property_Get("ro.build.fingerprint");

  if (rom_finger_print.empty())
  	rom_finger_print = System_Property_Get("ro.build.thumbprint");

  if (rom_finger_print.empty())
  	rom_finger_print = System_Property_Get("ro.vendor.build.thumbprint");

  if (!rom_finger_print.empty())
     {
  	LOGINFO("- Using the ROM's fingerprint (%s)\n", rom_finger_print.c_str());
  	TWFunc::Fox_Property_Set("ro.build.fingerprint", tmp + rom_finger_print + tmp);
     }
  else LOGINFO("- ROM fingerprint not available\n");
}


string TWFunc::get_assert_device(const string filename)
{
string str = "";
string temp = find_phrase(filename, "ro.product.device");

   if (temp.empty())
	temp = find_phrase(filename, "ro.build.product");

   if (temp.empty())
      return str;

#ifdef FOX_AB_DEVICE // we shouldn't even reach here, as we should be using update_engine/payload.bin
   // deal with inept attempts to bypass update_engine/payload.bin
   if ((temp.find("assert") != std::string::npos && temp.find("getprop") != std::string::npos) || (temp.find("abort") != std::string::npos && temp.find("getprop") != std::string::npos)) {
	gui_print("This ROM installer bypasses update_engine/payload.bin! Proceeding with the target device check...\n");
   }
   else
	return str;

#else // use the original code

   // either assert or getprop should be on the ro.product.device line
   if (temp.find("assert") == std::string::npos && temp.find("getprop") == std::string::npos)
      return str;

   // we are also looking for E3004 and abort on the same line
   if (temp.find("E3004") != std::string::npos && temp.find("abort") != std::string::npos)
      {
        //gui_print("- Found E3004 and abort on the ro.product.device line !\n");
      }
   else   
      {
        //gui_print("- E3004 and abort not found on the ro.product.device line! Search again ...\n");
        string temp2 = find_phrase(filename, "E3004");
   	if (temp2.empty()) // we really need this error code
   	   return str;
   	   
   	if (temp2.find("abort") == std::string::npos) // we also need the abort statement
           return str;
        //gui_print("- Finally found E3004 and abort!\n");
      }

#endif

   // parse the string to extract the device name
   str = DeleteBefore(temp, "==", true);// remove everything before "=="
   str = DeleteAfter(str, "||"); 	// remove everything after "||"
   str = removechar(str, '"');   	// remove quotation marks
   str = removechar(str, ' ');		// remove spaces

   return str;
}


string TWFunc::get_assert_device_zip(const string filename, const ZipArchiveHandle Zip) {
string metadata_sg_path = "META-INF/com/android/metadata";
const string take_out_metadata = "/tmp/zip_tmp_metadata";

   // META-INF/com/android/metadata is not in zip, or we can't extract it
   if (!zip_EntryExists(Zip, metadata_sg_path) || !zip_ExtractEntry(Zip, metadata_sg_path, take_out_metadata, 0644))
   	return TWFunc::get_assert_device(filename);

   // look for "pre-device"
   string metadata_devices = TWFunc::File_Property_Get(take_out_metadata, "pre-device");
   unlink(take_out_metadata.c_str());

   if (metadata_devices.empty()) {
   	return TWFunc::get_assert_device(filename);   
   }
   return metadata_devices;
}


string TWFunc::removechar(const string src, const char chars)
{
std::string str = src;
int i = str.find(chars);
   while (i != (int)std::string::npos)
   {
     str.erase(i, 1);
     i = str.find(chars);
   }
   return str;
}


string TWFunc::lowercase (const string src)
{
   string str = src;
   transform(str.begin(), str.end(), str.begin(), ::tolower);
   return str;
}


string TWFunc::uppercase (const string src)
{
   string str = src;
   transform(str.begin(), str.end(), str.begin(), ::toupper);
   return str;
}

/* find the position of "subs" in "str" (or -1 if not found) */


/* find the position of "subs" in "str" (or -1 if not found) */
int TWFunc::pos (const string subs, const string str)
{
  return str.find(subs);
}


string TWFunc::ltrim(std::string str, const std::string chars)
{
    str.erase(0, str.find_first_not_of(chars));
    return str;
}

 
string TWFunc::rtrim(std::string str, const std::string chars)
{
    str.erase(str.find_last_not_of(chars) + 1);
    return str;
}


string TWFunc::trim(std::string str, const std::string chars)
{
    return ltrim(rtrim(str, chars), chars);
}


int TWFunc::DeleteFromIndex(std::string &Str, int Index, int Size)
{
  int len = Str.length();
  if (Index < 0 || Index > len || Size < 1)
     return -1;
  int i = (Size - Index);
  if (i >= len) 
     Size = i;
  Str.erase (Index, Size);
  return Size;
}


string TWFunc::DeleteBefore(const std::string Str, const std::string marker, bool removemarker)
{
  std::string src = Str;
  int i = src.find(marker);
  if (i == (int)std::string::npos) 
     return Str;
  if (removemarker) 
     i += marker.length();
  src.erase (0, i);
  return src;
}


string TWFunc::DeleteAfter(const std::string Str, const std::string marker)
{
  std::string src = Str;
  int i = src.find(marker);
  if (i == (int)std::string::npos) 
     return Str;
  src.erase (i, src.length());
  return src;
}


string TWFunc::find_phrase(std::string filename, std::string search)
{
  std::string line = "";
  std::string str = "";
  std::ifstream File;
  File.open(filename);
  if (File.is_open())
    {
      while (!File.eof())
	{
	  std::getline(File, line);
	  if (line.find(search) != std::string::npos)
	    {
	      File.close();
	      return line;
	    }
	}
      File.close();
    }
  return str;
}


bool TWFunc::HasDelimitedWord(const std::string& str, const std::string& word) {
	for (size_t pos = str.find(word); pos != std::string::npos; pos = str.find(word, pos + word.length())) {
		bool isStartValid = (pos == 0 || (!std::isalnum(str[pos - 1]) && !std::ispunct(str[pos - 1])));
		bool isEndValid = (pos + word.length() == str.length() || (!std::isalnum(str[pos + word.length()]) && !std::ispunct(str[pos + word.length()])));
		if (isStartValid && isEndValid)
			return true;
	}

	return false;
}


string TWFunc::Fox_Property_Get(string Prop_Name) {
	return android::base::GetProperty(Prop_Name, "");
}


bool TWFunc::Fox_Property_Set(const std::string Prop_Name, const std::string Value) {
  usleep(2048);
  bool res = android::base::SetProperty(Prop_Name, Value);
  if (!res && Fox_Property_Get(Prop_Name) != Value) {
    	usleep(1028);
	string tmp = "\"";
	string cmd = Fox_ResetProp_Bin;

  	if (!Path_Exists(cmd))
    		cmd = Fox_Bin_Dir + "/resetprop";

  	if (!Path_Exists(cmd))
    		cmd = Fox_Bin_Dir + "/setprop";

    	if (Path_Exists(cmd)) {
  	    int ret = Exec_Cmd(cmd + " " + Prop_Name + " " + tmp + Value + tmp);
  	    //gui_print("DEBUG rerun TWFunc::Fox_Property_Set() - return value of property_set of (%s => %s)=%i\n", Prop_Name.c_str(), Value.c_str(), ret);
  	    res = (ret == 0);
    	}
  }
  usleep(2048);
  return res;
}


bool TWFunc::Has_Dynamic_Partitions(void) {
	return (Fox_Property_Get("ro.boot.dynamic_partitions") == "true" || DataManager::GetIntValue("fox_dynamic_device") == 1);
}


bool TWFunc::Has_Virtual_AB_Partitions(void) {
	if (Fox_Property_Get("ro.virtual_ab.enabled") == "true")
	   return true;
	#ifdef FOX_VIRTUAL_AB_DEVICE
	   return true;
	#else
	   return false;
	#endif
}


void TWFunc::Mapper_to_BootDevice(const std::string block_device, const std::string partition_name) {
	LOGINFO("Symlinking %s => /dev/block/bootdevice/by-name/%s \n", block_device.c_str(), partition_name.c_str());
	symlink(block_device.c_str(), ("/dev/block/bootdevice/by-name/" + partition_name).c_str());

	LOGINFO("Symlinking %s => /dev/block/by-name/%s \n", block_device.c_str(), partition_name.c_str());
	symlink(block_device.c_str(), ("/dev/block/by-name/" + partition_name).c_str());
}


void TWFunc::PostWipeEncryption(void) {
#ifdef OF_BIND_MOUNT_SDCARD_ON_FORMAT
	// deal with MTP issues after formatting data
	std::string dir = "/data/media/0";
	LOGINFO("Recreating %s...\n", dir.c_str());
	TWFunc::Recursive_Mkdir(dir, false);
	chmod(dir.c_str(), 0770);
	PartitionManager.Add_MTP_Storage("/data/media");
	// bind mount: this can be problematic for encryption
	LOGINFO("Bind mounting /data/media/0 to /sdcard after formatting\n");
	mount(dir.c_str(), "/sdcard", "", MS_BIND, NULL);
#endif
	// run the OrangeFox postformatdata script here
	TWFunc::RunFoxScript(FOX_POST_DATA_FORMAT_SCRIPT, "");
}


void TWFunc::Set_Sbin_Dir_Executable_Flags(void) {
  system("chmod 0755 /sbin/*");
}


bool TWFunc::IsBinaryXML(const std::string filename) {
  const uint32_t binary_xml_signature = 0x584241;
  int fd = open(filename.c_str(), O_RDONLY);
  if (fd >= 0) {
  	uint32_t i;
     	read(fd, &i, sizeof(uint32_t));
     	close(fd);
      	if (i == binary_xml_signature)
          return true;
  }
  return false;
}


// return the full path to the converted string, or empty string on error
std::string TWFunc::abx_to_xml_string(const std::string path) {
std::string res = path;
  if (abx_to_xml(path, res))
	return res;
  else
	return "";
}

/* for magiskboot 24+
   whether magiskboot repack should patch vbmeta
   returns:
 	true  = it should be patched
 	false = no patching is needed
*/


void TWFunc::FoxThemeCheck()
{
	// 首次开机(mBackingFile 已由 ReadSettingsFile()->LoadValues() 指到
	// Fox_Settings_Path/.foxs)时,设置文件并不存在 —— InfoManager::LoadValues()
	// 在文件缺失时只是返回 -1,并不会创建它。于是那些"持久化变量"在内存里没有值,
	// 主题里依赖它们的控件(例如锁屏的解锁按钮/滑块,条件用 lock_btn)就不渲染,
	// 必须手动进一次设置、触发写盘后,重启才正常。
	// 这里补一次幂等的落盘:文件不存在就写出当前内存值,让首次开机即等于"改过一次设置"。
	if (!TWFunc::Path_Exists(DataManager::GetSettingsStoragePath() + "/" + TW_SETTINGS_FILE)) {
		LOGINFO("Settings file not found - creating it now (%s/%s)\n",
			DataManager::GetSettingsStoragePath().c_str(), TW_SETTINGS_FILE);
		DataManager::Flush();
	}

	string theme_ver = DataManager::GetStrValue("of_themes_version");
	if (theme_ver.empty())
		theme_ver = "0";

	string build_theme_ver = DataManager::GetStrValue("fox_theme_version");
	if (build_theme_ver.empty())
		build_theme_ver = "0";

	if (theme_ver == build_theme_ver) {
		LOGINFO("Themes version: %s\n", build_theme_ver.c_str());
	} else {
		bool has_themes_dir = TWFunc::Path_Exists(FOX_THEME_PATH);
		if (has_themes_dir)
			gui_print_color("warning","* Themes version mismatch (old='%s'; new='%s')\n", theme_ver.c_str(), build_theme_ver.c_str());
		else
			LOGINFO("Themes version mismatch (old='%s'; new='%s')\n", theme_ver.c_str(), build_theme_ver.c_str());

		DataManager::SetValue("of_themes_version", build_theme_ver);
		DataManager::Flush();
		if (has_themes_dir) {
			gui_print_color("warning", "* Resetting the themes...\n");
			TWFunc::removeDir(FOX_THEME_PATH, false);
		}

		if (TWFunc::Path_Exists(FOX_NAVBAR_PATH)) {
			gui_print_color("warning", "* Resetting the navbar...\n");
			TWFunc::removeDir(FOX_NAVBAR_PATH, false);
		}
	}
}


bool TWFunc::IsRecoveryOverwritten(bool only_update) {
	static std::pair<string, string> previous_checksums;
	TWPartition* target_partition = PartitionManager.Find_Partition_By_Path("/boot");
#if defined(FOX_VENDOR_BOOT_RECOVERY) || defined(BOARD_MOVE_RECOVERY_RESOURCES_TO_VENDOR_BOOT)
	target_partition = PartitionManager.Find_Partition_By_Path("/vendor_boot");
#endif
#ifdef OF_AB_DEVICE_WITH_RECOVERY_PARTITION
	target_partition = PartitionManager.Find_Partition_By_Path("/recovery");
#endif
	if (!target_partition)
		return false;

	std::pair<string, string> current_checksums = PartitionManager.Get_Partition_Checksums(target_partition);
	if (current_checksums.first.empty()) {
		LOGINFO("%s: Cannot get checksums\n", __func__);
		return false;
	}

	if (only_update) {
		previous_checksums = current_checksums;
		return false;
	}

	if (previous_checksums.first.empty() || previous_checksums == current_checksums) {
		previous_checksums = current_checksums;
		LOGINFO("%s: The checksums match for %s\n", __func__, target_partition->Get_Mount_Point().c_str());
		return false;
	}

	LOGINFO("%s: The checksums do not match for %s\n", __func__, target_partition->Get_Mount_Point().c_str());
	return true;
}


void TWFunc::set_media_rw_permissions(const string pathname) {
	if (Path_Exists(pathname)) {
		setfilecon(pathname.c_str(), FOX_MEDIA_RW_DATA_FILE);
		chown(pathname.c_str(), AID_MEDIA_RW, AID_MEDIA_RW);
	}
}


void TWFunc::update_permissions_on_reboot() {
  if (android::base::GetProperty("ro.orangefox.substitute_permissions", "") == "1") {
	TWFunc::set_media_rw_permissions(Fox_Settings_Path);
	TWFunc::set_media_rw_permissions(FOX_NAVBAR_PATH);
	TWFunc::set_media_rw_permissions(FOX_NAVBAR_PATH + "/navbar.xml");
	TWFunc::set_media_rw_permissions(FOX_THEME_PATH);
	TWFunc::set_media_rw_permissions(FOX_THEME_PATH + "/accent.xml");
	TWFunc::set_media_rw_permissions(FOX_THEME_PATH + "/style.xml");
	TWFunc::set_media_rw_permissions("/data/recovery");
	TWFunc::set_media_rw_permissions(DataManager::GetStrValue(TW_BACKUPS_FOLDER_VAR));
	sync();
  }
}


bool TWFunc::Block_Operations_Until_Reboot() {
#ifdef OF_BLOCK_OPERATIONS_AFTER_ROM_FLASH
	if (TWFunc::Fox_Property_Get("fox_block_operations_pending_reboot") == "blocking") {
		gui_print_color("error", "\n\nThis operation has been blocked. Reboot OrangeFox (NOW!) before doing anything else.\n\n");
		return true;
	}
	return false;
#else
	return false;
#endif
}
//

#endif // ndef BUILD_TWRPTAR_MAIN
