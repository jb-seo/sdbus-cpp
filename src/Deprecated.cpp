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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace sdbus {

namespace {

constexpr const char* kEnvLogDeprecatedMethods = "SDBUSCPP_LOG_DEPRECATED_METHODS";

std::mutex handlerMutex;
DeprecatedMethodHandler handler;
bool envChecked = false;

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
        const char* env = std::getenv(kEnvLogDeprecatedMethods);
        if (env && *env != '\0' && std::strcmp(env, "0") != 0)
            handler = logDeprecatedMethodCall;
    }

    return handler;
}

}
