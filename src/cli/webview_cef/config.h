#pragma once
#include <string>
#include <vector>

/* One overlay group from the INI config (mirrors Qt GroupConfig). */
struct GroupConfig {
  std::string url;
  int width = 640;
  int height = 480;
  std::string match;                /* process-name regex, empty = no filter */
  std::vector<std::string> scripts; /* InjectScripts paths (resolved) */
};

/* Parse an INI file with [Section] + key=value (keys case-insensitive:
 * Url, Width, Height, Match, InjectScripts). Returns groups in file order. */
std::vector<GroupConfig> ParseConfig(const std::string &path);

/* Find an explicit --config path or a legacy positional config path without
 * treating values belonging to other options as config files. */
const char *FindConfigPath(int argc, char *argv[]);

/* Pick the view for |proc| (process name, may be empty):
 * - if any group has a Match= regex, take the first group whose regex
 *   matches |proc| (groups without Match= are skipped);
 * - otherwise take the first group with a URL.
 * Returns false when no group applies. */
bool SelectView(const std::vector<GroupConfig> &groups, const std::string &proc, GroupConfig *out);
