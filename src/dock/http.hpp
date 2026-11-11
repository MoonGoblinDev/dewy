// Minimal HTTPS helper for the dock, using the OS's own client (NSURLSession on macOS, WinHTTP on
// Windows): Qt's network module has no TLS backend inside OBS.
#pragma once

#include <functional>
#include <string>

namespace beauty {

/* HEAD request that follows redirects. `done` runs on a background thread with the HTTP status
 * (0 on network errors) and the final URL after redirects. */
void httpResolve(const std::string &url, const std::string &userAgent,
		 std::function<void(int status, std::string finalUrl)> done);

} // namespace beauty
