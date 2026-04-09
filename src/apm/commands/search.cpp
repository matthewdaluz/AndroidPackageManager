/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: search.cpp
 * Purpose: Implement client-side repository search and result printing.
 * Last Modified: 2026-03-15 11:56:16.536347864 -0400.
 * Author: Matthew DaLuz - RedHead Founder
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

#include "search.hpp"

#include "protocol.hpp"
#include "transport.hpp"

#include <iostream>
#include <vector>

namespace apm::cli {

// Search the merged repo indices for the provided pattern list and emit human
// readable matches.
int searchPackages(const std::vector<std::string> &patternsIn) {
  if (patternsIn.empty()) {
    std::cerr << "apm: 'search' requires a pattern\n";
    return 1;
  }

  apm::ipc::Request req;
  req.type = apm::ipc::RequestType::Search;

  std::string serializedPatterns;
  for (std::size_t i = 0; i < patternsIn.size(); ++i) {
    if (i > 0) {
      serializedPatterns.push_back('\n');
    }
    serializedPatterns += patternsIn[i];
  }
  req.rawFields["patterns"] = serializedPatterns;

  apm::ipc::Response resp;
  std::string err;
  if (!apm::ipc::sendRequestAuto(req, resp, &err)) {
    std::cerr << "Error: " << err << "\n";
    return 1;
  }

  if (!resp.message.empty())
    std::cout << resp.message << "\n";
  return resp.success ? 0 : 1;
}

} // namespace apm::cli
