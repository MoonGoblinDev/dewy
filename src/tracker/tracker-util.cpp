#include "tracker-util.hpp"

#include <cstdarg>
#include <cstdio>

#ifdef BEAUTY_HAVE_LIBOBS
#include <util/base.h>
#endif

namespace beauty {

void tracker_log(const char *fmt, ...)
{
	char msg[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
#ifdef BEAUTY_HAVE_LIBOBS
	blog(LOG_INFO, "[dewy] %s", msg);
#else
	fprintf(stderr, "[dewy] %s\n", msg);
#endif
}

} // namespace beauty
