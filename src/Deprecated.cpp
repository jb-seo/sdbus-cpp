/**
 * (C) 2016 - 2021 KISTLER INSTRUMENTE AG, Winterthur, Switzerland
 * (C) 2016 - 2024 Stanislav Angelovic <stanislav.angelovic@protonmail.com>
 *
 * @file Deprecated.cpp
 *
 * Created on: Jan 1, 2025
 * Project: sdbus-c++
 * Description: Deprecated API call reporting helpers
 *
 * This file is part of sdbus-c++.
 *
 * sdbus-c++ is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * sdbus-c++ is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with sdbus-c++. If not, see <http://www.gnu.org/licenses/>.
 */

#include "sdbus-c++/Deprecated.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace sdbus {

namespace {

constexpr const char* kEnvLogDeprecatedMethods = "SDBUSCPP_LOG_DEPRECATED_METHODS";
constexpr const char* kEnvUdsPath = "SDBUSCPP_DEPRECATED_UDS_PATH";
constexpr const char* kDefaultUdsPath = "/run/sdbus-deprecated.sock";

std::mutex handlerMutex;
DeprecatedMethodHandler handler;
bool envChecked = false;
std::string udsPath;

void logDeprecatedMethodCall(const DeprecatedMethodCallInfo& info)
{
    const char* sender = info.sender.empty() ? "-" : info.sender.c_str();
    std::fprintf(stderr,
                 "sdbus-c++: deprecated method call: interface=%s method=%s object=%s sender=%s pid=%ld\n",
                 info.interfaceName.c_str(),
                 info.methodName.c_str(),
                 info.objectPath.c_str(),
                 sender,
                 static_cast<long>(info.pid));
}

void sendDeprecatedMethodCall(const DeprecatedMethodCallInfo& info)
{
    if (udsPath.empty())
        return;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (udsPath.size() >= sizeof(addr.sun_path))
    {
        close(fd);
        return;
    }
    std::strncpy(addr.sun_path, udsPath.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        close(fd);
        return;
    }

    std::string line = info.sender.empty() ? "-" : info.sender;
    line.append(" ");
    line.append(info.interfaceName);
    line.append(" ");
    line.append(info.objectPath);
    line.append(" ");
    line.append(info.methodName);
    line.append(" ");
    line.append(std::to_string(static_cast<long>(info.pid)));
    line.append("\n");

    const char* data = line.data();
    size_t remaining = line.size();
    while (remaining > 0)
    {
        ssize_t n = write(fd, data, remaining);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }
        data += n;
        remaining -= static_cast<size_t>(n);
    }

    close(fd);
}

} // namespace

void setDeprecatedMethodHandler(DeprecatedMethodHandler newHandler)
{
    std::lock_guard<std::mutex> lock(handlerMutex);
    handler = std::move(newHandler);
}

DeprecatedMethodHandler getDeprecatedMethodHandler()
{
    std::lock_guard<std::mutex> lock(handlerMutex);

    if (!handler && !envChecked)
    {
        envChecked = true;
        const char* logEnv = std::getenv(kEnvLogDeprecatedMethods);
        if (logEnv && *logEnv != '\0' && std::strcmp(logEnv, "0") != 0)
        {
            handler = logDeprecatedMethodCall;
            return handler;
        }

        const char* pathEnv = std::getenv(kEnvUdsPath);
        if (pathEnv && std::strcmp(pathEnv, "0") == 0)
            return handler;

        udsPath = pathEnv && *pathEnv != '\0' ? pathEnv : kDefaultUdsPath;
        handler = sendDeprecatedMethodCall;
    }

    return handler;
}

}
