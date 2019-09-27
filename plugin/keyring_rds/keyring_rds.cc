/* Copyright (c) 2016, 2018, Alibaba and/or its affiliates. All rights reserved.

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

#include "my_config.h"

#include <mysql/components/my_service.h>
#include <mysql/components/services/log_builtins.h>
#include <mysql/plugin_keyring.h>
#include <memory>
#include "my_compiler.h"
#include "my_inttypes.h"
#include "my_io.h"
#include "my_psi_config.h"
#include "mysqld_error.h"

#include "keyring_rds.h"
#include "keyring_rds_common.h"
#include "keyring_rds_key_id.h"
#include "keyring_rds_kms_agent.h"
#include "keyring_rds_logger.h"

/**
  Validate the specified local file path.
  @return 0   OK
  @return 1   invalid
*/
static int check_key_id_file(MYSQL_THD thd MY_ATTRIBUTE((unused)),
                             SYS_VAR *var MY_ATTRIBUTE((unused)),
                             void *save MY_ATTRIBUTE((unused)),
                             st_mysql_value *value) {
  char buff[FN_REFLEN + 1];
  const char *keyring_filename;
  int len = static_cast<int>(sizeof(buff));

  keyring_filename = value->val_str(value, buff, &len);

  keyring_rds::Lock_helper wrlock(&keyring_rds::LOCK_keyring_rds, true);

  if (keyring_filename == NULL || keyring_filename[0] == 0) {
    keyring_rds::Logger::log(ERROR_LEVEL,
                             ER_KEYRING_FAILED_TO_SET_KEYRING_FILE_DATA);
    return 1;
  }

  return 0;
}

static MYSQL_SYSVAR_STR(
    key_id_file_dir,                                /* name       */
    keyring_rds::keyring_rds_dir,                   /* value      */
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,      /* flags      */
    "The path to the master key id file direction", /* comment    */
    check_key_id_file,                              /* check()    */
    NULL,                                           /* update()   */
    "__##keyring_rds##__"                           /* default    */
);

static MYSQL_SYSVAR_STR(kms_agent_cmd,              /* name       */
                        keyring_rds::kms_agent_cmd, /* value      */
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY, /* flags */
                        "KMS agent command line", /* comment    */
                        NULL,                     /* check()    */
                        NULL,                     /* update()   */
                        "/home/mysql/kms_agent"   /* default    */
);

static MYSQL_SYSVAR_INT(commandd_timeout_sec, keyring_rds::cmd_timeout_sec,
                        PLUGIN_VAR_OPCMDARG,
                        "KMS agent command execution timeout", NULL, NULL, 10,
                        1, std::numeric_limits<int>::max(), 0);

static SYS_VAR *keyring_rds_system_variables[] = {
    MYSQL_SYSVAR(key_id_file_dir), MYSQL_SYSVAR(kms_agent_cmd),
    MYSQL_SYSVAR(commandd_timeout_sec), NULL};

/* Log component service */
static SERVICE_TYPE(registry) *reg_srv = NULL;
SERVICE_TYPE(log_builtins) *log_bi = NULL;
SERVICE_TYPE(log_builtins_string) *log_bs = NULL;

static int keyring_rds_deinit(void *arg MY_ATTRIBUTE((unused))) {
  deinit_logging_service_for_plugin(&reg_srv, &log_bi, &log_bs);
  keyring_rds::keyring_rds_deinit();
  return false;
}

static int keyring_rds_init(MYSQL_PLUGIN plugin_info MY_ATTRIBUTE((unused))) {
  if (init_logging_service_for_plugin(&reg_srv, &log_bi, &log_bs)) return true;

  if (keyring_rds::keyring_rds_init()) {
    deinit_logging_service_for_plugin(&reg_srv, &log_bi, &log_bs);
    return true;
  }

  return false;
}

static bool mysql_key_fetch(const char *key_id, char **key_type,
                            const char *user_id, void **key, size_t *key_len) {
  return keyring_rds::key_imp->fetch(key_id, key_type, user_id, key, key_len);
}

static bool mysql_key_store(const char *key_id, const char *key_type,
                            const char *user_id, const void *key,
                            size_t key_len) {
  return keyring_rds::key_imp->store(key_id, key_type, user_id, key, key_len);
}

static bool mysql_key_remove(const char *key_id, const char *user_id) {
  return keyring_rds::key_imp->remove(key_id, user_id);
}

static bool mysql_key_generate(const char *key_id, const char *key_type,
                               const char *user_id, size_t key_len) {
  return keyring_rds::key_imp->generate(key_id, key_type, user_id, key_len);
}

static void mysql_key_iterator_init(void **key_iterator) {
  return keyring_rds::key_imp->iterator_init(key_iterator);
}

static void mysql_key_iterator_deinit(void *key_iterator) {
  return keyring_rds::key_imp->iterator_deinit(key_iterator);
}

static bool mysql_key_iterator_get_key(void *key_iterator, char *key_id,
                                       char *user_id) {
  return keyring_rds::key_imp->iterator_get_key(key_iterator, key_id, user_id);
}

/* Plugin type-specific descriptor */
static struct st_mysql_keyring keyring_descriptor = {
    MYSQL_KEYRING_INTERFACE_VERSION,
    mysql_key_store,
    mysql_key_fetch,
    mysql_key_remove,
    mysql_key_generate,
    mysql_key_iterator_init,
    mysql_key_iterator_deinit,
    mysql_key_iterator_get_key};

mysql_declare_plugin(keyring_rds){
    MYSQL_KEYRING_PLUGIN,    /*   type                            */
    &keyring_descriptor,     /*   descriptor                      */
    "keyring_rds",           /*   name                            */
    "Yongping.Liu, Alibaba", /*   author                          */
    "fetch authentication data from KMS", /*   description */
    PLUGIN_LICENSE_GPL,
    keyring_rds_init,             /*   init function (when loaded)     */
    NULL,                         /*   check uninstall function        */
    keyring_rds_deinit,           /*   deinit function (when unloaded) */
    0x0100,                       /*   version                         */
    NULL,                         /*   status variables                */
    keyring_rds_system_variables, /*   system variables                */
    NULL,
    0,
} mysql_declare_plugin_end;
