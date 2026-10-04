// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2020 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////
// Created: 20.06.2020 19:47

#pragma once

#include <stdint.h>
#include <stddef.h>

enum class HashAlgorithm
{
    CRC16,
    CRC32
};

class HashCalculator
{
public:
    HashCalculator() {}
    HashCalculator(HashAlgorithm algorithm) { m_CRC = 0xffffffff; }

    void Start(HashAlgorithm algorithm) { m_CRC = 0xffffffff; }
    void AddData(const void* data, size_t length);
    uint32_t Finalize() { return ~m_CRC; }

private:
    uint32_t m_CRC = 0;

};
