#include "upscale.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <stdlib.h> // getprogname
#endif

namespace metalvr {

namespace {

constexpr float kDefaultFactor = 2.0f; // Mullion's default for d3d11.metalSpatialUpscaleFactor
constexpr float kMaxFactor = 4.0f;

std::string lower(std::string s) {
  for (char &c : s)
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
  return s;
}

// a factor as Mullion takes it: a plain decimal number, never below 1
bool parseFactor(const std::string &text, float *out) {
  if (text.empty())
    return false;
  char *end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (!end || *end || !std::isfinite(value))
    return false;
  *out = std::clamp(static_cast<float>(value), 1.0f, kMaxFactor);
  return true;
}

// the file name of this process's executable, which Mullion's [section] lines are matched against
std::string exeName() {
#ifdef _WIN32
  char path[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
  std::string name(path, length);
  const size_t slash = name.find_last_of("\\/");
  return slash == std::string::npos ? name : name.substr(slash + 1);
#else
  const char *name = getprogname();
  return name ? name : "";
#endif
}

// one line of Mullion's config format (src/util/config/config.cpp): `key = value`, `[program.exe]` starting a
// section that applies to that program only; anything else is ignored
void parseLine(const std::string &line, const std::string &exe, bool *active, std::map<std::string, std::string> *out) {
  size_t n = line.find_first_not_of(" \t");
  if (n == std::string::npos)
    return;
  if (line[n] == '[') {
    const size_t close = line.rfind(']');
    const std::string section = close != std::string::npos && close > n ? line.substr(n + 1, close - n - 1) : "";
    *active = section == exe;
    return;
  }
  std::string key;
  while (n < line.size() && (std::isalnum(static_cast<unsigned char>(line[n])) || line[n] == '.' || line[n] == '_'))
    key += line[n++];
  n = line.find_first_not_of(" \t", n);
  if (key.empty() || n == std::string::npos || line[n] != '=')
    return;
  n = line.find_first_not_of(" \t", n + 1);
  std::string value;
  bool quoted = false;
  for (; n != std::string::npos && n < line.size(); ++n) {
    if (!quoted && (line[n] == ' ' || line[n] == '\t' || line[n] == '\r'))
      break;
    if (line[n] == '"')
      quoted = !quoted;
    else
      value += line[n];
  }
  if (*active)
    (*out)[key] = value;
}

FILE *openConfig(const char *path) {
  if (FILE *file = std::fopen(path, "rb"))
    return file;
#ifdef _WIN32
  // under Wine the variable usually holds a macOS path, which only Wine can turn into one Windows opens
  using DosName = WCHAR *(CDECL *)(const char *);
  static const auto to_dos = reinterpret_cast<DosName>(
      reinterpret_cast<void *>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "wine_get_dos_file_name")));
  if (to_dos && path[0] == '/') {
    if (WCHAR *dos = to_dos(path)) {
      FILE *file = _wfopen(dos, L"rb");
      HeapFree(GetProcessHeap(), 0, dos);
      return file;
    }
  }
#endif
  return nullptr;
}

// Mullion's options as Mullion would see them in this process: the file, then MULLION_CONFIG's lines over it
std::map<std::string, std::string> mullionOptions(std::string *where) {
  std::map<std::string, std::string> options;
  const std::string exe = exeName();
  if (const char *path = std::getenv("MULLION_CONFIG_FILE"); path && *path) {
    if (FILE *file = openConfig(path)) {
      bool active = true;
      std::string line;
      char chunk[512];
      while (std::fgets(chunk, sizeof(chunk), file)) {
        line += chunk;
        if (!line.empty() && line.back() != '\n' && !std::feof(file))
          continue;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
          line.pop_back();
        parseLine(line, exe, &active, &options);
        line.clear();
      }
      std::fclose(file);
      *where = path;
    }
  }
  if (const char *lines = std::getenv("MULLION_CONFIG"); lines && *lines) {
    bool active = true;
    std::string all = lines;
    size_t start = 0;
    while (start <= all.size()) {
      const size_t end = std::min(all.find(';', start), all.size());
      parseLine(all.substr(start, end - start), exe, &active, &options);
      start = end + 1;
    }
    *where = where->empty() ? "MULLION_CONFIG" : *where + " and MULLION_CONFIG";
  }
  return options;
}

SpatialUpscale read() {
  SpatialUpscale result;
  std::string where;
  const auto options = mullionOptions(&where);
  float mullion_factor = kDefaultFactor;
  bool mullion_has_factor = false;
  if (auto it = options.find("d3d11.metalSpatialUpscaleFactor"); it != options.end())
    mullion_has_factor = parseFactor(it->second, &mullion_factor);

  if (const char *env = std::getenv("METALVR_METALFX_SPATIAL"); env && *env) {
    const std::string value = lower(env);
    result.source = "METALVR_METALFX_SPATIAL";
    float factor = 1.0f;
    if (value == "0" || value == "off" || value == "false" || value == "no")
      result.factor = 1.0f;
    else if (value == "on" || value == "true" || value == "yes")
      result.factor = mullion_factor;
    else if (parseFactor(value, &factor))
      result.factor = factor;
    else
      result.source = "METALVR_METALFX_SPATIAL (not understood, so off)";
    return result;
  }

  if (auto it = options.find("d3d11.metalSpatialUpscale"); it != options.end() && lower(it->second) == "true") {
    result.factor = mullion_factor;
    result.source = "d3d11.metalSpatialUpscale" + std::string(mullion_has_factor ? "Factor" : " (factor 2)") +
                    " in " + where;
  }
  return result;
}

} // namespace

const SpatialUpscale &spatialUpscale() {
  static const SpatialUpscale setting = read();
  return setting;
}

uint32_t upscaledRenderSize(uint32_t eye_size) {
  const float factor = spatialUpscale().factor;
  if (factor <= 1.0f || eye_size == 0)
    return eye_size;
  return std::max<uint32_t>(16, static_cast<uint32_t>(std::lround(eye_size / factor)));
}

} // namespace metalvr
