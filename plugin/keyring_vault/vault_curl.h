/* Copyright (c) 2018 Percona LLC and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#ifndef MYSQL_VAULT_CURL_H
#define MYSQL_VAULT_CURL_H

#include <curl/curl.h>
#include <boost/core/noncopyable.hpp>
#include <sstream>
#include "i_vault_curl.h"
#include "plugin/keyring/common/i_keyring_key.h"
#include "plugin/keyring/common/logger.h"
#include "plugin/keyring/common/secure_string.h"
#include "vault_credentials.h"
#include "vault_key.h"

namespace keyring {

class Vault_curl final : public IVault_curl, private boost::noncopyable {
 public:
  Vault_curl(ILogger *logger, uint timeout) noexcept
      : logger(logger),
        list(nullptr),
        timeout(timeout),
        vault_version(Vault_version_v1) {}

  ~Vault_curl() {
    if (list != nullptr) curl_slist_free_all(list);
  }

  bool init(const Vault_credentials &vault_credentials) override;
  bool list_keys(Secure_string *response) override;
  bool list_mount_points(Secure_string *response) override;
  bool write_key(const Vault_key &key, Secure_string *response) override;
  bool read_key(const Vault_key &key, Secure_string *response) override;
  bool delete_key(const Vault_key &key, Secure_string *response) override;
  void set_timeout(uint timeout) noexcept override { this->timeout = timeout; }

  void set_vault_version(Vault_version_type version) override;
  Vault_version_type get_vault_version() const override;

 private:
  bool setup_curl_session(CURL *curl);
  std::string get_error_from_curl(CURLcode curl_code);
  bool encode_key_signature(const Vault_key &key,
                            Secure_string *encoded_key_signature);
  bool get_key_url(const Vault_key &key, Secure_string *key_url);
  bool do_list(const Secure_string &url_to_list, Secure_string *response);

  Secure_string get_secret_url_metadata();
  Secure_string get_secret_url_data();
  Secure_string get_secret_url(const Secure_string &type_of_data);
  Secure_string get_write_key_postdata(const Vault_key &key,
                                       Secure_string &encoded_key_data);

  ILogger *logger;
  Secure_string token_header;
  Secure_string vault_url;
  char curl_errbuf[CURL_ERROR_SIZE];  // error from CURL
  Secure_ostringstream read_data_ss;
  struct curl_slist *list;
  Secure_string vault_ca;
  uint timeout;
  Vault_version_type vault_version;

  Vault_credentials vault_credentials;
};

}  // namespace keyring

#endif  // MYSQL_VAULT_CURL_H
