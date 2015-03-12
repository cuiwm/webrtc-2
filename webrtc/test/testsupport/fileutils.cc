/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "webrtc/test/testsupport/fileutils.h"

#include <assert.h>

#ifdef WIN32
#include <direct.h>
#include <tchar.h>
#include <windows.h>
#include <algorithm>

#include "webrtc/system_wrappers/interface/utf_util_win.h"
#define GET_CURRENT_DIR _getcwd
#else
#include <unistd.h>

#include "webrtc/base/scoped_ptr.h"
#define GET_CURRENT_DIR getcwd
#endif

#include <sys/stat.h>  // To check for directory existence.
#ifndef S_ISDIR  // Not defined in stat.h on Windows.
#define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "webrtc/typedefs.h"  // For architecture defines

namespace webrtc {
namespace test {

namespace {

#ifdef WIN32
const char* kPathDelimiter = "\\";
#else
const char* kPathDelimiter = "/";
#endif

#ifdef WEBRTC_ANDROID
const char* kRootDirName = "/sdcard/";
#else
// The file we're looking for to identify the project root dir.
const char* kProjectRootFileName = "DEPS";
const char* kOutputDirName = "out";
const char* kFallbackPath = "./";
#endif
const char* kResourcesDirName = "resources";

char relative_dir_path[FILENAME_MAX];
bool relative_dir_path_set = false;

}  // namespace

const char* kCannotFindProjectRootDir = "ERROR_CANNOT_FIND_PROJECT_ROOT_DIR";

void SetExecutablePath(const std::string& path) {
  std::string working_dir = WorkingDir();
  std::string temp_path = path;

  // Handle absolute paths; convert them to relative paths to the working dir.
  if (path.find(working_dir) != std::string::npos) {
    temp_path = path.substr(working_dir.length() + 1);
  }
  // On Windows, when tests are run under memory tools like DrMemory and TSan,
  // slashes occur in the path as directory separators. Make sure we replace
  // such cases with backslashes in order for the paths to be correct.
#ifdef WIN32
  std::replace(temp_path.begin(), temp_path.end(), '/', '\\');
#endif

  // Trim away the executable name; only store the relative dir path.
  temp_path = temp_path.substr(0, temp_path.find_last_of(kPathDelimiter));
  strncpy(relative_dir_path, temp_path.c_str(), FILENAME_MAX);
  relative_dir_path_set = true;
}

bool FileExists(std::string& file_name) {
  struct stat file_info = {0};
  return stat(file_name.c_str(), &file_info) == 0;
}

#ifdef WEBRTC_ANDROID

std::string ProjectRootPath() {
  return kRootDirName;
}

std::string OutputPath() {
  return kRootDirName;
}

std::string WorkingDir() {
  return kRootDirName;
}

#else // WEBRTC_ANDROID

std::string ProjectRootPath() {
  std::string path = WorkingDir();
  if (path == kFallbackPath) {
    return kCannotFindProjectRootDir;
  }
  if (relative_dir_path_set) {
    path = path + kPathDelimiter + relative_dir_path;
  }
  // Check for our file that verifies the root dir.
  size_t path_delimiter_index = path.find_last_of(kPathDelimiter);
  while (path_delimiter_index != std::string::npos) {
    std::string root_filename = path + kPathDelimiter + kProjectRootFileName;
    if (FileExists(root_filename)) {
      return path + kPathDelimiter;
    }
    // Move up one directory in the directory tree.
    path = path.substr(0, path_delimiter_index);
    path_delimiter_index = path.find_last_of(kPathDelimiter);
  }
  // Reached the root directory.
  fprintf(stderr, "Cannot find project root directory!\n");
  return kCannotFindProjectRootDir;
}

std::string OutputPath() {
#if defined(WINRT)
  // TODO revisit this if needed as soon as the WorkingDir() is fixed for WinRT
  // output files are created in the local app folder
  auto folder = Windows::Storage::ApplicationData::Current->LocalFolder;
  wchar_t buffer[255];
  wcsncpy_s(buffer, 255, folder->Path->Data(), _TRUNCATE);
  return ToUtf8(buffer) + kPathDelimiter;
#else
  std::string path = ProjectRootPath();
  if (path == kCannotFindProjectRootDir) {
    return kFallbackPath;
  }
  path += kOutputDirName;
  if (!CreateDir(path)) {
    return kFallbackPath;
  }
  return path + kPathDelimiter;
#endif
}

#if defined(WINRT)
std::string WorkingDir() {
  // TODO (winrt): Is working directory relevant on WinRT?
  return kFallbackPath;
}
#else
std::string WorkingDir() {
  char path_buffer[FILENAME_MAX];
  if (!GET_CURRENT_DIR(path_buffer, sizeof(path_buffer))) {
    fprintf(stderr, "Cannot get current directory!\n");
    return kFallbackPath;
  } else {
    return std::string(path_buffer);
  }
}
#endif

#endif  // !WEBRTC_ANDROID

// Generate a temporary filename in a safe way.
// Largely copied from talk/base/{unixfilesystem,win32filesystem}.cc.
std::string TempFilename(const std::string &dir, const std::string &prefix) {
#if defined(WINRT)
  // TODO (winrt): Find a way to generate temporary filenames in a sync fashion.
  assert(false);
  return "";
#elif defined(WIN32)
  wchar_t filename[MAX_PATH];
  if (::GetTempFileName(ToUtf16(dir).c_str(),
                        ToUtf16(prefix).c_str(), 0, filename) != 0)
    return ToUtf8(filename);
  assert(false);
  return "";
#else
  int len = dir.size() + prefix.size() + 2 + 6;
  rtc::scoped_ptr<char[]> tempname(new char[len]);

  snprintf(tempname.get(), len, "%s/%sXXXXXX", dir.c_str(),
           prefix.c_str());
  int fd = ::mkstemp(tempname.get());
  if (fd == -1) {
    assert(false);
    return "";
  } else {
    ::close(fd);
  }
  std::string ret(tempname.get());
  return ret;
#endif
}

bool CreateDir(std::string directory_name) {
  struct stat path_info = {0};
  // Check if the path exists already:
  if (stat(directory_name.c_str(), &path_info) == 0) {
    if (!S_ISDIR(path_info.st_mode)) {
      fprintf(stderr, "Path %s exists but is not a directory! Remove this "
              "file and re-run to create the directory.\n",
              directory_name.c_str());
      return false;
    }
  } else {
#ifdef WIN32
    return _mkdir(directory_name.c_str()) == 0;
#else
    return mkdir(directory_name.c_str(),  S_IRWXU | S_IRWXG | S_IRWXO) == 0;
#endif
  }
  return true;
}

std::string ResourcePath(std::string name, std::string extension) {
  std::string platform = "win";
#ifdef WEBRTC_LINUX
  platform = "linux";
#endif  // WEBRTC_LINUX
#ifdef WEBRTC_MAC
  platform = "mac";
#endif  // WEBRTC_MAC

#ifdef WEBRTC_ARCH_64_BITS
  std::string architecture = "64";
#else
  std::string architecture = "32";
#endif  // WEBRTC_ARCH_64_BITS

  std::string resources_path = ProjectRootPath() + kResourcesDirName +
      kPathDelimiter;
  std::string resource_file = resources_path + name + "_" + platform + "_" +
      architecture + "." + extension;
  if (FileExists(resource_file)) {
    return resource_file;
  }
  // Try without architecture.
  resource_file = resources_path + name + "_" + platform + "." + extension;
  if (FileExists(resource_file)) {
    return resource_file;
  }
  // Try without platform.
  resource_file = resources_path + name + "_" + architecture + "." + extension;
  if (FileExists(resource_file)) {
    return resource_file;
  }

  // Fall back on name without architecture or platform.
  return resources_path + name + "." + extension;
}

size_t GetFileSize(std::string filename) {
  FILE* f = fopen(filename.c_str(), "rb");
  size_t size = 0;
  if (f != NULL) {
    if (fseek(f, 0, SEEK_END) == 0) {
      size = ftell(f);
    }
    fclose(f);
  }
  return size;
}

}  // namespace test
}  // namespace webrtc
