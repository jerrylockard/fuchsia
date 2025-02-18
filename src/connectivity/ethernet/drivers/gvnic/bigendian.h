// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_ETHERNET_DRIVERS_GVNIC_BIGENDIAN_H_
#define SRC_CONNECTIVITY_ETHERNET_DRIVERS_GVNIC_BIGENDIAN_H_

#include <byteswap.h>
#include <endian.h>
#include <stdint.h>

// Fuchsia only runs on little endian architectures.
// So this file assumes that it is true.  Just to be safe:
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__);

// Store an object of type 'T' as a big endian value doing automagic byte
// swapping when assigned or read.
template <typename T>
class __attribute__((packed)) BigEndian {
 private:
  // The actual in-memory object stored in big-endian form.
  T rep;

  // Function to do the swapping when needed.
  // Leaving the generic implementation commented out for now. Ideally, it
  // should never be needed.  The specializations below should be exhaustive.
  inline static T swap(const T& arg); /* {
    T ret;
    // Start by pointing 1 byte past the input arg.
    // And at the beginning of the value to return.
    // Then, bopy bytes backwards.
    const char* src = reinterpret_cast<const char*>(&arg + 1);
    char* tgt = reinterpret_cast<char*>(&ret);

    for (size_t i = 0; i < sizeof(T); i++)
      *dst++ = *--src;

    return ret;
  } */
 public:
  BigEndian() = default;
  // NOLINTNEXTLINE(google-explicit-constructor)
  BigEndian(const T& t) : rep(swap(t)) {}
  // NOLINTNEXTLINE(google-explicit-constructor)
  operator T() const { return swap(rep); }
  T val() const { return swap(rep); }
};

// Speciliazations to use the (presumably) more efficient bswap functions from
// byteswap.h for the basic integer types.

template <>
inline uint16_t BigEndian<uint16_t>::swap(const uint16_t& arg) {
  return bswap_16(arg);
}
static_assert(sizeof(BigEndian<uint16_t>) == sizeof(uint16_t));

template <>
inline uint32_t BigEndian<uint32_t>::swap(const uint32_t& arg) {
  return bswap_32(arg);
}
static_assert(sizeof(BigEndian<uint32_t>) == sizeof(uint32_t));

template <>
inline uint64_t BigEndian<uint64_t>::swap(const uint64_t& arg) {
  return bswap_64(arg);
}
static_assert(sizeof(BigEndian<uint64_t>) == sizeof(uint64_t));

/* Delete these if they are not used
template <>
int16_t BigEndian<int16_t>::swap(const int16_t& arg) {
  return reinterpret_cast<int16_t>(bswap_16(reinterpret_cast<uint16_t>(arg)));
}
static_assert(sizeof(BigEndian<int16_t>) == sizeof(int16_t));

template <>
int32_t BigEndian<int32_t>::swap(const int32_t& arg) {
  return reinterpret_cast<int32_t>(bswap_32(reinterpret_cast<uint32_t>(arg)));
}
static_assert(sizeof(BigEndian<int32_t>) == sizeof(int32_t));

template <>
int64_t BigEndian<int64_t>::swap(const int64_t& arg) {
  return reinterpret_cast<int64_t>(bswap_64(reinterpret_cast<uint64_t>(arg)));
}
static_assert(sizeof(BigEndian<int64_t>) == sizeof(int64_t));
*/

#endif  // SRC_CONNECTIVITY_ETHERNET_DRIVERS_GVNIC_BIGENDIAN_H_
