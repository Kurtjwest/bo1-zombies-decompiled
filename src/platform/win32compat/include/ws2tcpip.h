// <ws2tcpip.h> stand-in for the macOS port.
#pragma once
#include "winsock2.h"
#ifndef InetPtonA
#define InetPtonA inet_pton
#define InetNtopA inet_ntop
#define inet_pton inet_pton
#endif
