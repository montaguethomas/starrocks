// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/src/runtime/raw_value.h

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#pragma once

#include <string>

#include "common/logging.h"
#include "runtime/types.h"
#include "util/unaligned_access.h"

namespace starrocks {

class MemPool;
class SlotDescriptor;

// Useful utility functions for runtime values (which are passed around as void*).
class RawValue {
public:
    // Ascii output precision for double/float
    static const int ASCII_PRECISION;

    // Convert 'value' into ascii and write to 'stream'. NULL turns into "NULL". 'scale'
    // determines how many digits after the decimal are printed for floating point numbers,
    // -1 indicates to use the stream's current formatting.
    static void print_value(const void* value, const TypeDescriptor& type, int scale, std::stringstream* stream);

    // write ascii value to string instead of stringstream.
    static void print_value(const void* value, const TypeDescriptor& type, int scale, std::string* str);

    // Writes the byte representation of a value to a stringstream character-by-character
    static void print_value_as_bytes(const void* value, const TypeDescriptor& type, std::stringstream* stream);

    static uint32_t get_hash_value(const void* value, const PrimitiveType& type) {
        return get_hash_value(value, type, 0);
    }

    static uint32_t get_hash_value(const void* value, const PrimitiveType& type, uint32_t seed);

    // Returns hash value for 'value' interpreted as 'type'.  The resulting hash value
    // is combined with the seed value.
    static uint32_t get_hash_value(const void* value, const TypeDescriptor& type, uint32_t seed) {
        return get_hash_value(value, type.type, seed);
    }

    static uint32_t get_hash_value(const void* value, const TypeDescriptor& type) {
        return get_hash_value(value, type.type, 0);
    }

    // Get the hash value using the fvn hash function.  Using different seeds with FVN
    // results in different hash functions.  get_hash_value() does not have this property
    // and cannot be safely used as the first step in data repartitioning.
    // However, get_hash_value() can be significantly faster.
    // TODO: fix get_hash_value
    static uint32_t get_hash_value_fvn(const void* value, const PrimitiveType& type, uint32_t seed);

    static uint32_t get_hash_value_fvn(const void* value, const TypeDescriptor& type, uint32_t seed) {
        return get_hash_value_fvn(value, type.type, seed);
    }

    // Get the hash value using the fvn hash function.  Using different seeds with FVN
    // results in different hash functions.  get_hash_value() does not have this property
    // and cannot be safely used as the first step in data repartitioning.
    // However, get_hash_value() can be significantly faster.
    // TODO: fix get_hash_value
    static uint32_t zlib_crc32(const void* value, const TypeDescriptor& type, uint32_t seed);

    // Compares both values.
    // Return value is < 0  if v1 < v2, 0 if v1 == v2, > 0 if v1 > v2.
    static int compare(const void* v1, const void* v2, const TypeDescriptor& type);

    // Writes 'src' into 'dst' for type.
    // For string values, the string data is copied into 'pool' if pool is non-NULL.
    // src must be non-NULL.
    static void write(const void* src, void* dst, const TypeDescriptor& type, MemPool* pool);

    // Writes 'src' into 'dst' for type.
    // String values are copied into *buffer and *buffer is updated by the length. *buf
    // must be preallocated to be large enough.
    static void write(const void* src, const TypeDescriptor& type, void* dst, uint8_t** buf);

    // Returns true if v1 == v2.
    // This is more performant than compare() == 0 for string equality, mostly because of
    // the length comparison check.
    static bool eq(const void* v1, const void* v2, const TypeDescriptor& type);

    static bool lt(const void* v1, const void* v2, const TypeDescriptor& type);
};

} // namespace starrocks
