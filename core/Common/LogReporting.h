#pragma once

#include "Common/CommonTypes.h"
#include "Common/Log.h"

#define DEBUG_LOG_REPORT(t,...)   DEBUG_LOG(t, __VA_ARGS__)
#define ERROR_LOG_REPORT(t,...)   ERROR_LOG(t, __VA_ARGS__)
#define WARN_LOG_REPORT(t,...)    WARN_LOG(t, __VA_ARGS__)
#define NOTICE_LOG_REPORT(t,...)  NOTICE_LOG(t, __VA_ARGS__)
#define INFO_LOG_REPORT(t,...)    INFO_LOG(t, __VA_ARGS__)

#define LOG_N_TIMES_(n, ...)      do { static int logged_; if (logged_ < (n)) { logged_++; __VA_ARGS__; } } while (false)
#define DEBUG_LOG_REPORT_ONCE(n,t,...)   LOG_N_TIMES_(1, DEBUG_LOG(t, __VA_ARGS__))
#define ERROR_LOG_REPORT_ONCE(n,t,...)   LOG_N_TIMES_(1, ERROR_LOG(t, __VA_ARGS__))
#define WARN_LOG_REPORT_ONCE(n,t,...)    LOG_N_TIMES_(1, WARN_LOG(t, __VA_ARGS__))
#define NOTICE_LOG_REPORT_ONCE(n,t,...)  LOG_N_TIMES_(1, NOTICE_LOG(t, __VA_ARGS__))
#define INFO_LOG_REPORT_ONCE(n,t,...)    LOG_N_TIMES_(1, INFO_LOG(t, __VA_ARGS__))

#define ERROR_LOG_ONCE(n,t,...)   LOG_N_TIMES_(1, ERROR_LOG(t, __VA_ARGS__))
#define WARN_LOG_ONCE(n,t,...)    LOG_N_TIMES_(1, WARN_LOG(t, __VA_ARGS__))
#define NOTICE_LOG_ONCE(n,t,...)  LOG_N_TIMES_(1, NOTICE_LOG(t, __VA_ARGS__))
#define INFO_LOG_ONCE(n,t,...)    LOG_N_TIMES_(1, INFO_LOG(t, __VA_ARGS__))

#define ERROR_LOG_N_TIMES(s,n,t,...)   LOG_N_TIMES_(n, ERROR_LOG(t, __VA_ARGS__))
#define WARN_LOG_N_TIMES(s,n,t,...)    LOG_N_TIMES_(n, WARN_LOG(t, __VA_ARGS__))
#define NOTICE_LOG_N_TIMES(s,n,t,...)  LOG_N_TIMES_(n, NOTICE_LOG(t, __VA_ARGS__))
#define INFO_LOG_N_TIMES(s,n,t,...)    LOG_N_TIMES_(n, INFO_LOG(t, __VA_ARGS__))
