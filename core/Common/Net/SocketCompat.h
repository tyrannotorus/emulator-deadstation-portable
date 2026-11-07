#pragma once

#include "ppsspp_config.h"

#include <unistd.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/mman.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <ifaddrs.h>
#include <fcntl.h>
#include <fcntl.h>
#include <errno.h>



// TODO: move this to some common set
#define socket_errno errno
#define SOCKET int
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define closesocket close
#ifndef ESHUTDOWN
#define ESHUTDOWN ENETDOWN
#endif
inline bool connectInProgress(int errcode) { return (errcode == EAGAIN || errcode == EWOULDBLOCK || errcode == EINPROGRESS || errcode == EALREADY); }
inline bool isDisconnected(int errcode) { return (errcode == EPIPE || errcode == ECONNRESET || errcode == ECONNABORTED || errcode == ESHUTDOWN); }

#ifndef SD_RECEIVE
#define SD_RECEIVE SHUT_RD //0x00
#endif

#ifndef SD_BOTH
#define SD_BOTH SHUT_RDWR //0x02
#endif

#ifndef MSG_NOSIGNAL
// Default value to 0x00 (do nothing) in systems where it's not supported.
#define MSG_NOSIGNAL 0x00
#endif

#ifdef ERROR
#undef ERROR
#endif
