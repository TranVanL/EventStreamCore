#pragma once
#include "event/Event.hpp"
#include <string> 
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif


struct ParsedResult {
    std::string topic;
    std::vector<uint8_t> payload;
};

ParsedResult parseFrame(const std::vector<uint8_t>& frame_body);
ParsedResult parseTCPFrame(const std::vector<uint8_t>& full_frame_include_length);


