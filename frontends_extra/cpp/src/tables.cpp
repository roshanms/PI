/* Copyright 2013-present Barefoot Networks, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cstring>

#include <arpa/inet.h>

#include "PI/frontends/cpp/tables.h"
#include "PI/pi_p4info.h"

#include "pi_int.h"

#define SIZEOF_DST_ARR sizeof(((_compact_v_t *) 0)->bytes)

namespace pi {

namespace {

template <typename T>
T endianness(T v);

template <>
uint8_t endianness(uint8_t v) {
  return v;
}

template <>
uint16_t endianness(uint16_t v) {
  return htons(v);
}

template <>
uint32_t endianness(uint32_t v) {
  return htonl(v);
}

// TODO(antonin): portability
uint64_t htonll(uint64_t n) {
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  return n;
#else
  return (((uint64_t)htonl(n)) << 32) + htonl(n >> 32);
#endif
}

template <>
uint64_t endianness(uint64_t v) {
  return htonll(v);
}

}  // namespace

MatchKey::MatchKey(const pi_p4info_t *p4info, pi_p4_id_t table_id)
    : p4info(p4info), table_id(table_id) {
  size_t s = 0;
  size_t num_match_fields = pi_p4info_table_num_match_fields(p4info, table_id);
  // 2 compact blobs per field to accomodate all match types
  s += 2 * num_match_fields * sizeof(_compact_v_t);
  data_offset = s;
  for (size_t i = 0; i < num_match_fields; i++) {
    pi_p4info_match_field_info_t finfo;
    pi_p4info_table_match_field_info(p4info, table_id, i, &finfo);
    if (finfo.bitwidth > 64) {
      s += (finfo.bitwidth + 7) / 8;
    }
  }
  match_key = std::unique_ptr<char []>(new char[s]);
  data_offset_current = data_offset;
}

MatchKey::~MatchKey() { }

void
MatchKey::reset() {
  nset = 0;
  data_offset_current = data_offset;
}

template <typename T>
error_code_t
MatchKey::format(pi_p4_id_t f_id, T v, size_t index) {
  constexpr size_t type_bitwidth = sizeof(T) * 8;
  const size_t bitwidth = pi_p4info_field_bitwidth(p4info, f_id);
  const size_t bytes = (bitwidth + 7) / 8;
  const char byte0_mask = pi_p4info_field_byte0_mask(p4info, f_id);
  if (bitwidth > type_bitwidth) return 1;
  v = endianness(v);
  char *data = reinterpret_cast<char *>(&v);
  data += sizeof(T) - bytes;
  data[0] &= byte0_mask;
  pi_match_key_t *dst = reinterpret_cast<pi_match_key_t *>(match_key.get());
  memcpy(dst[index].bytes, data, bytes);
  return 0;
}

error_code_t
MatchKey::format(pi_p4_id_t f_id, const char *ptr, size_t s, size_t index) {
  // constexpr size_t type_bitwidth = sizeof(T) * 8;
  const size_t bitwidth = pi_p4info_field_bitwidth(p4info, f_id);
  const size_t bytes = (bitwidth + 7) / 8;
  const char byte0_mask = pi_p4info_field_byte0_mask(p4info, f_id);
  if (bytes != s) return 1;
  char *dst;
  pi_match_key_t *mkey = reinterpret_cast<pi_match_key_t *>(match_key.get());
  if (s <= SIZEOF_DST_ARR) {
    dst = mkey[index].bytes;
  } else {
    mkey[index].more_bytes = match_key.get() + data_offset_current;
    data_offset_current += s;
    dst = mkey[index].more_bytes;
  }
  memcpy(dst, ptr, bytes);
  dst[0] &= byte0_mask;
  return 0;
}

template <typename T>
typename std::enable_if<std::is_integral<T>::value, error_code_t>::type
MatchKey::set_exact(pi_p4_id_t f_id, T key) {
  // explicit instantiation below so compile time check not possible
  assert((!std::is_signed<T>::value) && "signed fields not supported yet");
  size_t f_index = pi_p4info_table_match_field_index(p4info, table_id, f_id);
  size_t index = f_index * 2;
  return format(f_id, key, index);
}

template error_code_t MatchKey::set_exact<uint8_t>(pi_p4_id_t, uint8_t);
template error_code_t MatchKey::set_exact<uint16_t>(pi_p4_id_t, uint16_t);
template error_code_t MatchKey::set_exact<uint32_t>(pi_p4_id_t, uint32_t);
template error_code_t MatchKey::set_exact<uint64_t>(pi_p4_id_t, uint64_t);
template error_code_t MatchKey::set_exact<int8_t>(pi_p4_id_t, int8_t);
template error_code_t MatchKey::set_exact<int16_t>(pi_p4_id_t, int16_t);
template error_code_t MatchKey::set_exact<int32_t>(pi_p4_id_t, int32_t);
template error_code_t MatchKey::set_exact<int64_t>(pi_p4_id_t, int64_t);

error_code_t
MatchKey::set_exact(pi_p4_id_t f_id, const char *key, size_t s) {
  size_t f_index = pi_p4info_table_match_field_index(p4info, table_id, f_id);
  size_t index = f_index * 2;
  return format(f_id, key, s, index);
}

template <typename T>
typename std::enable_if<std::is_integral<T>::value, error_code_t>::type
MatchKey::set_lpm(pi_p4_id_t f_id, T key, int prefix_length) {
  // explicit instantiation below so compile time check not possible
  assert((!std::is_signed<T>::value) && "signed fields not supported yet");
  size_t f_index = pi_p4info_table_match_field_index(p4info, table_id, f_id);
  size_t index = f_index * 2;
  error_code_t rc;
  rc = format(f_id, key, index);
  index += 1;
  pi_match_key_t *mkey = reinterpret_cast<pi_match_key_t *>(match_key.get());
  mkey[index].v = prefix_length;
  return rc;
}

template error_code_t MatchKey::set_lpm<uint8_t>(pi_p4_id_t, uint8_t, int);
template error_code_t MatchKey::set_lpm<uint16_t>(pi_p4_id_t, uint16_t, int);
template error_code_t MatchKey::set_lpm<uint32_t>(pi_p4_id_t, uint32_t, int);
template error_code_t MatchKey::set_lpm<uint64_t>(pi_p4_id_t, uint64_t, int);
template error_code_t MatchKey::set_lpm<int8_t>(pi_p4_id_t, int8_t, int);
template error_code_t MatchKey::set_lpm<int16_t>(pi_p4_id_t, int16_t, int);
template error_code_t MatchKey::set_lpm<int32_t>(pi_p4_id_t, int32_t, int);
template error_code_t MatchKey::set_lpm<int64_t>(pi_p4_id_t, int64_t, int);

error_code_t
MatchKey::set_lpm(pi_p4_id_t f_id, const char *key, size_t s,
                  int prefix_length) {
  size_t f_index = pi_p4info_table_match_field_index(p4info, table_id, f_id);
  size_t index = f_index * 2;
  error_code_t rc;
  rc = format(f_id, key, s, index);
  index += 1;
  pi_match_key_t *mkey = reinterpret_cast<pi_match_key_t *>(match_key.get());
  mkey[index].v = prefix_length;
  return rc;
}

template <typename T>
typename std::enable_if<std::is_integral<T>::value, error_code_t>::type
MatchKey::set_ternary(pi_p4_id_t f_id, T key, T mask) {
  // explicit instantiation below so compile time check not possible
  assert((!std::is_signed<T>::value) && "signed fields not supported yet");
  size_t f_index = pi_p4info_table_match_field_index(p4info, table_id, f_id);
  size_t index = f_index * 2;
  error_code_t rc;
  rc = format(f_id, key, index);
  index += 1;
  if (rc) return rc;
  rc = format(f_id, mask, index);
  return rc;
}

template error_code_t MatchKey::set_ternary<uint8_t>(pi_p4_id_t, uint8_t,
                                                     uint8_t);
template error_code_t MatchKey::set_ternary<uint16_t>(pi_p4_id_t, uint16_t,
                                                      uint16_t);
template error_code_t MatchKey::set_ternary<uint32_t>(pi_p4_id_t, uint32_t,
                                                      uint32_t);
template error_code_t MatchKey::set_ternary<uint64_t>(pi_p4_id_t, uint64_t,
                                                      uint64_t);
template error_code_t MatchKey::set_ternary<int8_t>(pi_p4_id_t, int8_t,
                                                    int8_t);
template error_code_t MatchKey::set_ternary<int16_t>(pi_p4_id_t, int16_t,
                                                     int16_t);
template error_code_t MatchKey::set_ternary<int32_t>(pi_p4_id_t, int32_t,
                                                     int32_t);
template error_code_t MatchKey::set_ternary<int64_t>(pi_p4_id_t, int64_t,
                                                     int64_t);

error_code_t
MatchKey::set_ternary(pi_p4_id_t f_id, const char *key, char *mask, size_t s) {
  size_t f_index = pi_p4info_table_match_field_index(p4info, table_id, f_id);
  size_t index = f_index * 2;
  error_code_t rc;
  rc = format(f_id, key, s, index);
  index += 1;
  if (rc) return rc;
  rc = format(f_id, mask, s, index);
  return rc;
}

ActionData::ActionData(const pi_p4info_t *p4info, pi_p4_id_t action_id)
    : p4info(p4info), action_id(action_id) {
  size_t s = 0;
  size_t num_params;
  const pi_p4_id_t *params = pi_p4info_action_get_params(p4info, action_id,
                                                         &num_params);
  s += num_params * sizeof(_compact_v_t);
  size_t data_offset = s;
  for (size_t i = 0; i < num_params; i++) {
    size_t bitwidth = pi_p4info_action_param_bitwidth(p4info, params[i]);
    if (bitwidth > 64) {
      s += (bitwidth + 7) / 8;
    }
  }
  action_data = std::unique_ptr<char []>(new char[s]);
  data_offset_current = data_offset;
}

ActionData::~ActionData() { }

void
ActionData::reset() {
  nset = 0;
  data_offset_current = data_offset;
}

template <typename T>
error_code_t
ActionData::format(pi_p4_id_t ap_id, T v, size_t index) {
  constexpr size_t type_bitwidth = sizeof(T) * 8;
  const size_t bitwidth = pi_p4info_action_param_bitwidth(p4info, ap_id);
  const size_t bytes = (bitwidth + 7) / 8;
  const char byte0_mask = pi_p4info_action_param_byte0_mask(p4info, ap_id);
  if (bitwidth > type_bitwidth) return 1;
  v = endianness(v);
  char *data = reinterpret_cast<char *>(&v);
  data += sizeof(T) - bytes;
  data[0] &= byte0_mask;
  pi_action_data_t *dst = reinterpret_cast<pi_action_data_t *>(action_data.get());
  memcpy(dst[index].bytes, data, bytes);
  return 0;
}

error_code_t
ActionData::format(pi_p4_id_t ap_id, const char *ptr, size_t s, size_t index) {
  // constexpr size_t type_bitwidth = sizeof(T) * 8;
  const size_t bitwidth = pi_p4info_action_param_bitwidth(p4info, ap_id);
  const size_t bytes = (bitwidth + 7) / 8;
  const char byte0_mask = pi_p4info_action_param_byte0_mask(p4info, ap_id);
  if (bytes != s) return 1;
  char *dst;
  pi_action_data_t *adata = reinterpret_cast<pi_action_data_t *>(action_data.get());
  if (s <= SIZEOF_DST_ARR) {
    dst = adata[index].bytes;
  } else {
    adata[index].more_bytes = action_data.get() + data_offset_current;
    data_offset_current += s;
    dst = adata[index].more_bytes;
  }
  memcpy(dst, ptr, bytes);
  dst[0] &= byte0_mask;
  return 0;
}

template <typename T>
typename std::enable_if<std::is_integral<T>::value, error_code_t>::type
ActionData::set_arg(pi_p4_id_t ap_id, T arg) {
  // explicit instantiation below so compile time check not possible
  assert((!std::is_signed<T>::value) && "signed params not supported yet");
  size_t index = ap_id & 0xff;
  return format(ap_id, arg, index);
}

error_code_t
ActionData::set_arg(pi_p4_id_t ap_id, const char *arg, size_t s) {
  size_t index = ap_id & 0xff;
  return format(ap_id, arg, s, index);
}

template error_code_t ActionData::set_arg<uint8_t>(pi_p4_id_t, uint8_t);
template error_code_t ActionData::set_arg<uint16_t>(pi_p4_id_t, uint16_t);
template error_code_t ActionData::set_arg<uint32_t>(pi_p4_id_t, uint32_t);
template error_code_t ActionData::set_arg<uint64_t>(pi_p4_id_t, uint64_t);
template error_code_t ActionData::set_arg<int8_t>(pi_p4_id_t, int8_t);
template error_code_t ActionData::set_arg<int16_t>(pi_p4_id_t, int16_t);
template error_code_t ActionData::set_arg<int32_t>(pi_p4_id_t, int32_t);
template error_code_t ActionData::set_arg<int64_t>(pi_p4_id_t, int64_t);


MatchTable::MatchTable(const pi_p4info_t *p4info, pi_p4_id_t table_id)
    : p4info(p4info), table_id(table_id) { }

error_code_t
MatchTable::entry_add(const MatchKey &match_key, pi_p4_id_t action_id,
                      const ActionData &action_data, bool overwrite,
                      pi_entry_handle_t *entry_handle) {
  if (match_key.table_id != table_id) return 1;
  if (action_id != action_data.action_id) return 1;
  // TODO(antonin): handle device id / pipleline mask
  const pi_dev_tgt_t dev_tgt = {0, 0xffff};
  pi_table_entry_t entry = {action_id, action_data.get(), NULL, NULL};
  return pi_table_entry_add(dev_tgt, table_id, match_key.get(),
                            &entry, overwrite, entry_handle);
}

error_code_t
MatchTable::entry_delete(pi_entry_handle_t entry_handle) {
  const uint16_t dev_id = 0;
  return pi_table_entry_delete(dev_id, table_id, entry_handle);
}

}  // namespace pi