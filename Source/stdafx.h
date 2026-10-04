// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#undef SendMessage
#undef CreateFile
#undef DeleteFile
#undef CreateDirectory
#undef ERROR

#include <QtWidgets>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

constexpr size_t INVALID_INDEX = std::numeric_limits<size_t>::max();
