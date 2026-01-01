/**
 * (C) 2016 - 2021 KISTLER INSTRUMENTE AG, Winterthur, Switzerland
 * (C) 2016 - 2024 Stanislav Angelovic <stanislav.angelovic@protonmail.com>
 *
 * @file Deprecated.h
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

#ifndef SDBUS_CXX_DEPRECATED_H_
#define SDBUS_CXX_DEPRECATED_H_

#include <functional>
#include <string>
#include <sys/types.h>

namespace sdbus {

    struct DeprecatedMethodCallInfo
    {
        std::string interfaceName;
        std::string methodName;
        std::string objectPath;
        std::string sender;
        pid_t pid{-1};
    };

    using DeprecatedMethodHandler = std::function<void(const DeprecatedMethodCallInfo&)>;

    void setDeprecatedMethodHandler(DeprecatedMethodHandler handler);
    DeprecatedMethodHandler getDeprecatedMethodHandler();

}

#endif /* SDBUS_CXX_DEPRECATED_H_ */
