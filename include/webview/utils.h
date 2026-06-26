#pragma once

#include <QString>

namespace Utils
{

/**
 * Resolve a path relative to a base directory.
 * Absolute paths are returned as-is.
 */
QString resolvedPath(const QString &path, const QString &basePath);

} // namespace Utils
