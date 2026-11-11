#include "http.hpp"

#import <Foundation/Foundation.h>

namespace beauty {

void httpResolve(const std::string &url, const std::string &userAgent,
		 std::function<void(int status, std::string finalUrl)> done)
{
	NSURL *u = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
	if (!u) {
		done(0, std::string());
		return;
	}
	NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:u
							   cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
						       timeoutInterval:15.0];
	req.HTTPMethod = @"HEAD";
	[req setValue:[NSString stringWithUTF8String:userAgent.c_str()] forHTTPHeaderField:@"User-Agent"];
	auto cb = std::make_shared<std::function<void(int, std::string)>>(std::move(done));
	NSURLSessionDataTask *task = [[NSURLSession sharedSession]
		dataTaskWithRequest:req
		  completionHandler:^(NSData *, NSURLResponse *resp, NSError *err) {
			  NSHTTPURLResponse *http = [resp isKindOfClass:[NSHTTPURLResponse class]]
							    ? (NSHTTPURLResponse *)resp
							    : nil;
			  int status = (err || !http) ? 0 : (int)http.statusCode;
			  std::string finalUrl = resp.URL ? std::string(resp.URL.absoluteString.UTF8String) : std::string();
			  (*cb)(status, finalUrl);
		  }];
	[task resume];
}

} // namespace beauty
