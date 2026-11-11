#include "http.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include <thread>
#include <vector>

namespace beauty {

static std::wstring widen(const std::string &s)
{
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
	std::wstring w(n > 0 ? n - 1 : 0, L'\0');
	if (n > 1)
		MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
	return w;
}

static std::string narrow(const std::wstring &w)
{
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
	std::string s(n > 0 ? n - 1 : 0, '\0');
	if (n > 1)
		WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
	return s;
}

void httpResolve(const std::string &url, const std::string &userAgent,
		 std::function<void(int status, std::string finalUrl)> done)
{
	std::thread([url, userAgent, done]() {
		int status = 0;
		std::string finalUrl;
		std::wstring wurl = widen(url);
		URL_COMPONENTS uc = {};
		uc.dwStructSize = sizeof(uc);
		wchar_t host[256] = {}, path[2048] = {};
		uc.lpszHostName = host;
		uc.dwHostNameLength = 256;
		uc.lpszUrlPath = path;
		uc.dwUrlPathLength = 2048;
		HINTERNET ses = nullptr, con = nullptr, req = nullptr;
		if (WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc) &&
		    (ses = WinHttpOpen(widen(userAgent).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
				       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)) != nullptr) {
			WinHttpSetTimeouts(ses, 15000, 15000, 15000, 15000);
			con = WinHttpConnect(ses, host, uc.nPort, 0);
			DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
			if (con)
				req = WinHttpOpenRequest(con, L"HEAD", path, nullptr, WINHTTP_NO_REFERER,
							 WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
			if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0,
						      0) &&
			    WinHttpReceiveResponse(req, nullptr)) {
				DWORD code = 0, len = sizeof(code);
				if (WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
							WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX))
					status = (int)code;
				DWORD ulen = 0; /* final URL after redirects */
				WinHttpQueryOption(req, WINHTTP_OPTION_URL, nullptr, &ulen);
				if (ulen > 0) {
					std::vector<wchar_t> buf(ulen / sizeof(wchar_t) + 1, L'\0');
					if (WinHttpQueryOption(req, WINHTTP_OPTION_URL, buf.data(), &ulen))
						finalUrl = narrow(std::wstring(buf.data()));
				}
			}
		}
		if (req)
			WinHttpCloseHandle(req);
		if (con)
			WinHttpCloseHandle(con);
		if (ses)
			WinHttpCloseHandle(ses);
		done(status, finalUrl);
	}).detach();
}

} // namespace beauty
