/*
 * Copyright 2019 Peifeng Yu <peifeng@umich.edu>
 * 
 * This file is part of Salus
 * (see https://github.com/SymbioticLab/Salus).
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *    http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SALUS_UTILS_MACROS_H
#define SALUS_UTILS_MACROS_H

#include "config.h"
#include <cstddef>
#include <functional>

#define UNUSED(x) (void) (x)

#if !defined(HAS_CXX_ENUM_HASH)
namespace std {
template<class E>
class hash
{
    using sfinae = typename std::enable_if<std::is_enum<E>::value, E>::type;

public:
    size_t operator()(const E &e) const
    {
        return std::hash<typename std::underlying_type<E>::type>()(e);
    }
};
} // namespace std
#endif // HAS_CXX_ENUM_HASH

// GCC/Clang can be told that a certain branch is not likely to be taken (for
// instance, a CHECK failure), and use that information in static analysis.
// Giving it this information can help it optimize for the common case in
// the absence of better information (ie. -fprofile-arcs).
#if defined(HAS_CXX_BUILTIN_EXPECT)
#define SALUS_PREDICT_FALSE(x) (__builtin_expect(x, 0))
#define SALUS_PREDICT_TRUE(x) (__builtin_expect(!!(x), 1))
#else
#define SALUS_PREDICT_FALSE(x) (x)
#define SALUS_PREDICT_TRUE(x) (x)
#endif // HAS_CXX_BUILTIN_EXPECT

// A macro to disallow the copy constructor and operator= functions
// This is usually placed in the private: declarations for a class.
#define SALUS_DISALLOW_COPY_AND_ASSIGN(TypeName)                                                             \
    TypeName(const TypeName &) = delete;                                                                     \
    void operator=(const TypeName &) = delete

constexpr std::size_t operator"" _sz(unsigned long long n)
{
    return n;
}

namespace sstl {
template<class T>
inline void hash_combine(std::size_t &seed, const T &v)
{
    std::hash<T> hasher;
    seed ^= hasher(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}
} // namespace sstl

#endif // SALUS_UTILS_MACROS_H
