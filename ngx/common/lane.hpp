// VKD3D-Proton-MacOS's NGX core and NVAPI: what they share. They run on this lane only (Wine loads them from its
// lib/vkd3d-proton folder for GRAPHICS_BACKEND=vkd3d), with VKD3D-Proton's D3D12 and DXVK's DXGI and D3D11, and
// answer only programs DXVK's DXGI shows an NVIDIA GPU (dxgi.customVendorId = 10de in DXVK_CONFIG or dxvk.conf),
// as MetalSharp Wine sets up DLSS games.
//
// This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser General
// Public License as published by the Free Software Foundation; either version 2.1 of the License, or (at your
// option) any later version.
#pragma once

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace lane {

// "info", "warn" or "err" lines on stderr, the way Wine's and VKD3D-Proton's go to the game's log
inline void log(const char *level, const char *format, ...) {
  char text[1024];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  fprintf(stderr, "%s:vkd3d-ngx: %s\n", level, text);
}

inline std::string trim(std::string text) {
  size_t begin = text.find_first_not_of(" \t\r\n"), end = text.find_last_not_of(" \t\r\n");
  return begin == std::string::npos ? std::string() : text.substr(begin, end - begin + 1);
}

// a "key = value" line of DXVK's configuration; *value without its quotes
inline bool config_line(const std::string &line, const char *key, std::string *value) {
  size_t equals = line.find('=');
  if (equals == std::string::npos || trim(line.substr(0, equals)) != key)
    return false;
  *value = trim(line.substr(equals + 1));
  if (value->size() >= 2 && value->front() == '"' && value->back() == '"')
    *value = value->substr(1, value->size() - 2);
  return true;
}

inline std::string env(const char *name) {
  char value[4096];
  DWORD length = GetEnvironmentVariableA(name, value, sizeof(value));
  return length && length < sizeof(value) ? std::string(value, length) : std::string();
}

// a DXVK option for this program, as DXVK reads it: dxvk.conf (DXVK_CONFIG_FILE, else the working directory's), its
// lines before any [program.exe] section and those of this program's, then DXVK_CONFIG's, ';' apart
inline std::string dxvk_option(const char *key) {
  std::string result, value, path = env("DXVK_CONFIG_FILE"), program;
  char module[MAX_PATH];
  DWORD length = GetModuleFileNameA(nullptr, module, sizeof(module));
  if (length && length < sizeof(module)) {
    program = module;
    program = program.substr(program.find_last_of("\\/") + 1);
  }
  if (path.empty())
    path = "dxvk.conf";
  if (FILE *file = fopen(path.c_str(), "r")) {
    char line[1024];
    bool active = true;
    while (fgets(line, sizeof(line), file)) {
      std::string text = trim(line);
      if (!text.empty() && text[0] == '[') {
        std::string section = text.substr(1, text.find(']') == std::string::npos ? std::string::npos : text.find(']') - 1);
        active = !program.empty() && !_stricmp(section.c_str(), program.c_str());
      } else if (active && config_line(text, key, &value)) {
        result = value;
      }
    }
    fclose(file);
  }
  std::string line_env = env("DXVK_CONFIG");
  for (size_t begin = 0; begin <= line_env.size();) {
    size_t end = line_env.find(';', begin);
    if (end == std::string::npos)
      end = line_env.size();
    if (config_line(line_env.substr(begin, end - begin), key, &value))
      result = value;
    begin = end + 1;
  }
  return result;
}

// whether DXVK's DXGI shows this program an NVIDIA GPU
inline bool shown_nvidia_gpu() {
  std::string vendor = dxvk_option("dxgi.customVendorId");
  return !_stricmp(vendor.c_str(), "10de") || !_stricmp(vendor.c_str(), "0x10de");
}

} // namespace lane
