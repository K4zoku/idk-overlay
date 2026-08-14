#include "config.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <regex>
#include <sstream>

#include "core/log.h"

static std::string Trim(const std::string &s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos)
    return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

/* Strip one level of surrounding double quotes (QSettings does this). */
static std::string Unquote(const std::string &s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
    return s.substr(1, s.size() - 2);
  return s;
}

static std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return s;
}

static std::string KeyOf(const std::string &line) {
  size_t eq = line.find('=');
  if (eq == std::string::npos)
    return "";
  return Trim(line.substr(0, eq));
}

static std::string ValueOf(const std::string &line) {
  size_t eq = line.find('=');
  if (eq == std::string::npos)
    return "";
  return Unquote(Trim(line.substr(eq + 1)));
}

std::vector<GroupConfig> ParseConfig(const std::string &path) {
  std::vector<GroupConfig> groups;
  std::ifstream in(path);
  if (!in) {
    IDK_LOG("webview-cef", "config not readable: %s\n", path.c_str());
    return groups;
  }

  std::string line;
  size_t cur = SIZE_MAX;
  while (std::getline(in, line)) {
    std::string t = Trim(line);
    if (t.empty() || t[0] == ';' || t[0] == '#')
      continue;
    if (t.front() == '[' && t.back() == ']') {
      groups.emplace_back();
      cur = groups.size() - 1;
      continue;
    }
    if (cur == SIZE_MAX)
      continue; /* key outside any section */

    std::string key = Lower(KeyOf(t));
    std::string val = ValueOf(t);
    GroupConfig &g = groups[cur];
    if (key == "url") {
      g.url = val;
    } else if (key == "width") {
      g.width = atoi(val.c_str());
    } else if (key == "height") {
      g.height = atoi(val.c_str());
    } else if (key == "match") {
      g.match = val;
    } else if (key == "injectscripts") {
      /* Comma-separated file paths, resolved relative to the config dir. */
      std::string base = path.substr(0, path.find_last_of('/') + 1);
      std::stringstream ss(val);
      std::string part;
      while (std::getline(ss, part, ',')) {
        part = Trim(part);
        if (part.empty())
          continue;
        if (part[0] != '/')
          part = base + part;
        g.scripts.push_back(part);
      }
    }
  }
  return groups;
}

bool SelectView(const std::vector<GroupConfig> &groups, const std::string &proc, GroupConfig *out) {
  bool has_match = false;
  for (const auto &g : groups)
    if (!g.match.empty())
      has_match = true;

  if (has_match) {
    if (proc.empty())
      return false;
    for (const auto &g : groups) {
      if (g.match.empty())
        continue;
      try {
        std::regex re(g.match, std::regex::ECMAScript);
        if (std::regex_search(proc, re)) {
          *out = g;
          return true;
        }
      } catch (const std::regex_error &) {
        IDK_LOG("webview-cef", "invalid Match regex '%s'\n", g.match.c_str());
      }
    }
    return false;
  }

  for (const auto &g : groups) {
    if (!g.url.empty()) {
      *out = g;
      return true;
    }
  }
  return false;
}
