#include "upscale.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

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

// A number as Mullion's config parser reads one (Config::parseOptionValue for float): digits, then optionally a
// point and more digits, whatever the locale. Accepted factors are never below 1 (as Mullion takes them) or above
// kMaxFactor (MetalVR's limit: past it the eyes are too small to be worth scaling).
bool parseFactor(const std::string &text, float *out) {
  size_t pos = 0;
  if (text.empty() || text[0] == '.')
    return false;
  double whole = 0, fraction = 0, divisor = 1;
  for (; pos < text.size() && text[pos] != '.'; ++pos) {
    if (text[pos] < '0' || text[pos] > '9')
      return false;
    whole = whole * 10 + (text[pos] - '0');
  }
  if (pos < text.size() && ++pos == text.size())
    return false; // "3." is not a number to Mullion
  for (; pos < text.size(); ++pos) {
    if (text[pos] < '0' || text[pos] > '9')
      return false;
    fraction = fraction * 10 + (text[pos] - '0');
    divisor *= 10;
  }
  const double value = whole + fraction / divisor;
  if (!std::isfinite(value))
    return false;
  *out = std::clamp(static_cast<float>(value), 1.0f, kMaxFactor);
  return true;
}

#ifdef _WIN32
std::string toUtf8(const WCHAR *text, int length = -1) {
  const int size = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
  std::string out(size > 0 ? size : 0, '\0');
  if (size > 0)
    WideCharToMultiByte(CP_UTF8, 0, text, length, out.data(), size, nullptr, nullptr);
  if (length < 0 && !out.empty())
    out.pop_back(); // the terminator
  return out;
}

std::wstring toWide(const std::string &text) {
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
  std::wstring out(size > 0 ? size : 0, L'\0');
  if (size > 0)
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), size);
  if (!out.empty())
    out.pop_back();
  return out;
}
#endif

// A variable as Mullion reads it (env::getEnvVar): UTF-8, and a MULLION_ name falls back to its DXMT_ one.
// On Windows it is read wide, as getenv would hand back the ANSI code page's version of a macOS path.
std::string envVar(const std::string &name) {
  std::string value;
#ifdef _WIN32
  const std::wstring wide = toWide(name);
  const DWORD size = GetEnvironmentVariableW(wide.c_str(), nullptr, 0);
  if (size > 0) {
    std::vector<WCHAR> buffer(size);
    const DWORD length = GetEnvironmentVariableW(wide.c_str(), buffer.data(), size);
    value = toUtf8(buffer.data(), static_cast<int>(length));
  }
#else
  if (const char *env = std::getenv(name.c_str()))
    value = env;
#endif
  if (value.empty() && name.rfind("MULLION_", 0) == 0)
    return envVar("DXMT_" + name.substr(8));
  return value;
}

// the file name of this process's executable, which Mullion's [section] lines are matched against (UTF-8)
std::string exeName() {
#ifdef _WIN32
  std::vector<WCHAR> path(32768);
  const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  const std::string name = toUtf8(path.data(), static_cast<int>(length));
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

// a config file by its UTF-8 name, as Mullion opens it
FILE *openConfig(const std::string &path) {
#ifdef _WIN32
  if (FILE *file = _wfopen(toWide(path).c_str(), L"rb"))
    return file;
  // under Wine the variable usually holds a macOS path, which only Wine can turn into one Windows opens
  // (wine_get_dos_file_name takes the Unix path in UTF-8)
  using DosName = WCHAR *(CDECL *)(const char *);
  static const auto to_dos = reinterpret_cast<DosName>(
      reinterpret_cast<void *>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "wine_get_dos_file_name")));
  if (to_dos && !path.empty() && path[0] == '/') {
    if (WCHAR *dos = to_dos(path.c_str())) {
      FILE *file = _wfopen(dos, L"rb");
      HeapFree(GetProcessHeap(), 0, dos);
      return file;
    }
  }
  return nullptr;
#else
  return std::fopen(path.c_str(), "rb");
#endif
}

// Mullion's options as Mullion would see them in this process (Config::getUserConfig): the file
// MULLION_CONFIG_FILE names, or mullion.conf (then dxmt.conf) in the working directory, then MULLION_CONFIG's
// lines over it
std::map<std::string, std::string> mullionOptions(std::string *where) {
  std::map<std::string, std::string> options;
  const std::string exe = exeName();
  std::string path = envVar("MULLION_CONFIG_FILE");
  FILE *file = nullptr;
  if (!path.empty()) {
    file = openConfig(path);
  } else if ((file = openConfig(path = "mullion.conf")) == nullptr) {
    file = openConfig(path = "dxmt.conf");
  }
  if (file) {
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
  if (const std::string all = envVar("MULLION_CONFIG"); !all.empty()) {
    bool active = true;
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

  if (const std::string env = envVar("METALVR_METALFX_SPATIAL"); !env.empty()) {
    const std::string value = lower(env);
    result.source = "METALVR_METALFX_SPATIAL";
    float factor = 1.0f;
    if (value == "0" || value == "off" || value == "false" || value == "no") {
      result.factor = 1.0f;
    } else if (value == "on" || value == "true" || value == "yes") {
      // on means smaller eyes: Mullion's factor when it asks for that, else Mullion's default
      result.enabled = true;
      result.factor = mullion_factor > 1.0f ? mullion_factor : kDefaultFactor;
    } else if (parseFactor(value, &factor)) {
      result.enabled = true;
      result.factor = factor;
    } else {
      result.source = "METALVR_METALFX_SPATIAL (not understood, so off)";
    }
    return result;
  }

  // Mullion's own switches (d3d11_swapchain.cpp): MULLION_METALFX_SPATIAL_SWAPCHAIN=1 or d3d11.metalSpatialUpscale
  std::string from;
  if (envVar("MULLION_METALFX_SPATIAL_SWAPCHAIN") == "1")
    from = "MULLION_METALFX_SPATIAL_SWAPCHAIN";
  else if (auto it = options.find("d3d11.metalSpatialUpscale"); it != options.end() && lower(it->second) == "true")
    from = "d3d11.metalSpatialUpscale";
  if (!from.empty()) {
    result.enabled = true;
    result.factor = mullion_factor;
    result.source = from + (mullion_has_factor ? ", d3d11.metalSpatialUpscaleFactor" : ", factor 2 (Mullion's default)") +
                    (where.empty() ? std::string() : " in " + where);
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
