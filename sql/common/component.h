/* Copyright (c) 2018, 2019, Alibaba and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef SQL_COMMON_COMPONENT_INCLUDED
#define SQL_COMMON_COMPONENT_INCLUDED

#include <string>

#include "map_helpers.h"
#include "mysql/components/services/bits/psi_memory_bits.h"
#include "mysql/psi/psi_memory.h"

namespace im  {

/**
  PSI memory detect interface;
*/
class PSI_memory_base {
 public:
  PSI_memory_base(PSI_memory_key key) : m_key(key) {}

  virtual ~PSI_memory_base() {}

  /* Getting */
  PSI_memory_key psi_key() { return m_key; }

  /* Setting */
  void set_psi_key(PSI_memory_key key) { m_key = key; }

 private:
  PSI_memory_key m_key;
};

/* Disable the copy and assign construct */
class Disable_copy_base {
 public:
  Disable_copy_base() {}
  virtual ~Disable_copy_base() {}

 private:
  Disable_copy_base(const Disable_copy_base &);
  Disable_copy_base(const Disable_copy_base &&);
  Disable_copy_base &operator=(const Disable_copy_base &);
};

/**
  String fundamental function, trim space of it's left and right
  T has to be std::basic_string type.
*/
template <typename T, bool NEED>
T &trim(T &s) {
  if (NEED) {
    s.erase(0, s.find_first_not_of(" "));
    s.erase(s.find_last_not_of(" ") + 1);
  }
  return s;
}
/* Split str by separator and push them into container */
template <typename T, typename C, bool TRIM>
void split(const char *str, const char *separator, C *container) {
  typename T::size_type pos1, pos2;
  if (str == nullptr || str[0] == '\0' || separator == nullptr) return;
  T t_str(str), sub;
  pos1 = 0;
  pos2 = t_str.find(separator);
  while (pos2 != T::npos) {
    sub = t_str.substr(pos1, pos2 - pos1);
    container->push_back(trim<T, TRIM>(sub));
    pos1 = pos2 + strlen(separator);
    pos2 = t_str.find(separator, pos1);
  }
  sub = t_str.substr(pos1);
  container->push_back(trim<T, TRIM>(sub));
  return;
}

/*
  Pair key map definition
*/
template <typename F, typename S>
using Pair_key_type = std::pair<F, S>;

template <typename F, typename S>
class Pair_key_comparator {
 public:
  bool operator()(const Pair_key_type<F, S> &lhs,
                  const Pair_key_type<F, S> &rhs) const;
};

template <typename F, typename S, typename T>
class Pair_key_unordered_map
    : public malloc_unordered_map<Pair_key_type<F, S>, const T *,
                                  std::hash<Pair_key_type<F, S>>,
                                  Pair_key_comparator<F, S>> {
 public:
  explicit Pair_key_unordered_map(PSI_memory_key key)
      : malloc_unordered_map<Pair_key_type<F, S>, const T *,
                             std::hash<Pair_key_type<F, S>>,
                             Pair_key_comparator<F, S>>(key) {}
};

} /* namespace im */

namespace std {
template <typename F, typename S>
struct hash<im::Pair_key_type<F, S>> {
 public:
  typedef typename im::Pair_key_type<F, S> argument_type;
  typedef size_t result_type;

  size_t operator()(const im::Pair_key_type<F, S> &p) const;
};

} /* namespace std */


#endif
