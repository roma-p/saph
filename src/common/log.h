// Messages for whoever runs the host: "[MillSaph] ..." lines printed to the terminal
// (stderr) and added to MS_LOG_FILE, as hosts often run without a terminal.

#pragma once

#ifdef _WIN32
#define MS_LOG_FILE "%TEMP%\\millsaph.log"
#else
// In the home folder: /tmp can be shared between users, or seen differently by each application.
#define MS_LOG_FILE "~/millsaph.log"
#endif

// printf-style; a line break is added.
void ms_log(const char *format, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 1, 2)))
#endif
    ;
