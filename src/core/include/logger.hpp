/*
 * APM - Android Package Manager
 *
 * Skidworks Open-Engineering - Software Branch
 * Copyright (C) 2027 Skidworks Open-Engineering
 *
 * File: logger.hpp
 * Purpose: Declare the logging facade used across the project.
 * Last Modified: November 15th, 2026. - 09:14 AM EST.
 * Author: Matthew DaLuz - Skidworks Founder
 *
 * APM is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * APM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with APM. If not, see <https://www.gnu.org/licenses/>.
 *
 */

#pragma once

#include <string>

namespace apm::logger {

// Log severity levels
enum class Level { Debug = 0, Info, Warn, Error };

// Set the log file path (default is /data/apm/logs/apm.log).
// Can be overridden at startup (e.g., for debugging on a PC).
void setLogFile(const std::string &path);

// Configure the file that controls global debug logging (true/false).
void setDebugControlFile(const std::string &path);

// Persist and immediately apply the global debug logging state.
bool setDebugEnabled(bool enabled, std::string *errorMsg = nullptr);

// Set minimum level to actually write.
// Messages below this level will be discarded.
void setMinLogLevel(Level level);

// Return whether global debug mode is currently enabled.
bool isDebugEnabled();

// Enable or disable mirroring logs to stderr as well as the log file.
void enableStderr(bool enable);

// Core logging function.
void log(Level level, const std::string &message);

// Convenience wrappers.
void debug(const std::string &message);
void info(const std::string &message);
void warn(const std::string &message);
void error(const std::string &message);

} // namespace apm::logger
