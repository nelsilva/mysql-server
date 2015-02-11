/* Copyright (c) 2000, 2015, Oracle and/or its affiliates. All rights reserved.
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "sql_parse.h"                  /* check_access */
#include "rpl_filter.h"                 /* rpl_filter */
#include "sql_base.h"                   /* MYSQL_LOCK_IGNORE_TIMEOUT */
#include "sql_table.h"                  /* open_ltable */
#include "sql_plugin.h"                 /* lock_plugin_data etc. */
#include "password.h"                   /* my_make_scrambled_password */
#include "log_event.h"                  /* append_query_string */
#include "key.h"                        /* key_copy, key_cmp_if_same */
                                        /* key_restore */

#include "auth_internal.h"
#include "sql_auth_cache.h"
#include "sql_authentication.h"
#include "prealloced_array.h"
#include "tztime.h"
#include "crypt_genhash_impl.h"         /* CRYPT_MAX_PASSWORD_SIZE */
#include "current_thd.h"

/**
  Auxiliary function for constructing a  user list string.
  This function is used for error reporting and logging.
 
  @param thd     Thread context
  @param str     A String to store the user list.
  @param user    A LEX_USER which will be appended into user list.
  @param comma   If TRUE, append a ',' before the the user.
  @param ident   If TRUE, append ' IDENTIFIED BY/WITH...' after the user,
                 if the given user has credentials set with 'IDENTIFIED BY/WITH'
 */
void append_user(THD *thd, String *str, LEX_USER *user, bool comma= true,
                 bool ident= false)
{
  String from_user(user->user.str, user->user.length, system_charset_info);
  String from_plugin(user->plugin.str, user->plugin.length, system_charset_info);
  String from_auth(user->auth.str, user->auth.length, system_charset_info);
  String from_host(user->host.str, user->host.length, system_charset_info);

  if (comma)
    str->append(',');
  append_query_string(thd, system_charset_info, &from_user, str);
  str->append(STRING_WITH_LEN("@"));
  append_query_string(thd, system_charset_info, &from_host, str);

  if (ident)
  {
    if (user->plugin.str && (user->plugin.length > 0) &&
        memcmp(user->plugin.str, native_password_plugin_name.str,
               user->plugin.length))
    {
      /** 
          The plugin identifier is allowed to be specified,
          both with and without quote marks. We log it with
          quotes always.
        */
      str->append(STRING_WITH_LEN(" IDENTIFIED WITH "));
      append_query_string(thd, system_charset_info, &from_plugin, str);

      if (user->auth.str && (user->auth.length > 0))
      {
        str->append(STRING_WITH_LEN(" AS "));
        append_query_string(thd, system_charset_info, &from_auth, str);
      }
    }
    else if (user->auth.str)
    {
      str->append(STRING_WITH_LEN(" IDENTIFIED BY PASSWORD '"));
      if (user->uses_identified_by_password_clause)
      {
        str->append(user->auth.str, user->auth.length);
        str->append("'");
      }
      else
      {
        /*
          Password algorithm is chosen based on old_passwords variable or
          TODO the new password_algorithm variable.
          It is assumed that the variable hasn't changed since parsing.
        */
        if (thd->variables.old_passwords == 0)
        {
          /*
            my_make_scrambled_password_sha1() requires a target buffer size of
            SCRAMBLED_PASSWORD_CHAR_LENGTH + 1.
            The extra character is for the probably originate from either '\0'
            or the initial '*' character.
          */
          char tmp[SCRAMBLED_PASSWORD_CHAR_LENGTH + 1];
          my_make_scrambled_password_sha1(tmp, user->auth.str,
                                          user->auth.length);
          str->append(tmp);
        }
        else
        {
          /*
            With old_passwords == 2 the scrambled password will be binary.
          */
          DBUG_ASSERT(thd->variables.old_passwords = 2);
          str->append("<secret>");
        }
        str->append("'");
      }
    }
  }
}

void append_user_new(THD *thd, String *str, LEX_USER *user, bool comma= true)
{
  String from_user(user->user.str, user->user.length, system_charset_info);
  String from_plugin(user->plugin.str, user->plugin.length, system_charset_info);
  String default_plugin(default_auth_plugin_name.str,
                        default_auth_plugin_name.length, system_charset_info);
  String from_auth(user->auth.str, user->auth.length, system_charset_info);
  String from_host(user->host.str, user->host.length, system_charset_info);

  if (comma)
    str->append(',');
  append_query_string(thd, system_charset_info, &from_user, str);
  str->append(STRING_WITH_LEN("@"));
  append_query_string(thd, system_charset_info, &from_host, str);

  /* CREATE USER is always rewritten with IDENTIFIED WITH .. AS */
  if (thd->lex->sql_command == SQLCOM_CREATE_USER)
  {
    str->append(STRING_WITH_LEN(" IDENTIFIED WITH "));
    if (user->plugin.length > 0)
      append_query_string(thd, system_charset_info, &from_plugin, str);
    else
      append_query_string(thd, system_charset_info, &default_plugin, str);
    if (user->auth.length > 0)
    {
      str->append(STRING_WITH_LEN(" AS "));
      if (thd->lex->contains_plaintext_password)
      {
        str->append("'");
        str->append(STRING_WITH_LEN("<secret>"));
        str->append("'");
      }
      else
        append_query_string(thd, system_charset_info, &from_auth, str);
    }
  }
  else
  {
    if (user->uses_identified_by_clause ||
        user->uses_identified_with_clause ||
        user->uses_identified_by_password_clause)
    {
      str->append(STRING_WITH_LEN(" IDENTIFIED WITH "));
      if (user->plugin.length > 0)
        append_query_string(thd, system_charset_info, &from_plugin, str);
      else
        append_query_string(thd, system_charset_info, &default_plugin, str);
      if (user->auth.length > 0)
      {
        str->append(STRING_WITH_LEN(" AS "));
        if (thd->lex->contains_plaintext_password)
        {
          str->append("'");
          str->append(STRING_WITH_LEN("<secret>"));
          str->append("'");
        }
        else
          append_query_string(thd, system_charset_info, &from_auth, str);
      }
    }
  }
}

#ifndef NO_EMBEDDED_ACCESS_CHECKS

/*
 Enumeration of various ACL's and Hashes used in handle_grant_struct()
*/
enum enum_acl_lists
{
  USER_ACL= 0,
  DB_ACL,
  COLUMN_PRIVILEGES_HASH,
  PROC_PRIVILEGES_HASH,
  FUNC_PRIVILEGES_HASH,
  PROXY_USERS_ACL
};

int check_change_password(THD *thd, const char *host, const char *user,
                          const char *new_password, size_t new_password_len)
{
  if (!initialized)
  {
    my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0), "--skip-grant-tables");
    return(1);
  }
  if (!thd->slave_thread &&
      (strcmp(thd->security_context()->user().str, user) ||
       my_strcasecmp(system_charset_info, host,
                     thd->security_context()->priv_host().str)))
  {
    if (check_access(thd, UPDATE_ACL, "mysql", NULL, NULL, 1, 0))
      return(1);
  }
  if (!thd->slave_thread &&
      !strcmp(thd->security_context()->priv_user().str,""))
  {
    my_message(ER_PASSWORD_ANONYMOUS_USER, ER(ER_PASSWORD_ANONYMOUS_USER),
               MYF(0));
    return(1);
  }

  return(0);
}

/**
  Auxiliary function for constructing CREATE USER sql for a given user.

  @param thd          Thread context
  @param user_name    user for which the sql should be constructed.

  @retval
    0         OK.
    1         Error.
 */

bool mysql_show_create_user(THD *thd, LEX_USER *user_name)
{
  int error= 0;
  ACL_USER *acl_user;
  LEX *lex= thd->lex;
  Protocol *protocol= thd->protocol;
  USER_RESOURCES tmp_user_resource;
  enum SSL_type ssl_type;
  char *ssl_cipher, *x509_issuer, *x509_subject, *ssl_info, *conn_attr;
  char buff[256];
  Item_string *field= NULL;
  List<Item> field_list;
  String sql_text(buff,sizeof(buff),system_charset_info);
  TABLE_LIST tables;
  TABLE *table;
  uchar user_key[MAX_KEY_LENGTH];
  LEX_ALTER alter_info;

  DBUG_ENTER("mysql_show_create_user");

  tables.init_one_table("mysql", 5, "user", 4, "user", TL_READ);
  if (!(table= open_ltable(thd, &tables, TL_READ, MYSQL_LOCK_IGNORE_TIMEOUT)))
    DBUG_RETURN(1);

  if (!table->key_info)
  {
    my_error(ER_TABLE_CORRUPT, MYF(0), table->s->db.str,
             table->s->table_name.str);
    DBUG_RETURN(1);
  }
  table->use_all_columns();
  table->field[MYSQL_USER_FIELD_HOST]->store(user_name->host.str,
                                             user_name->host.length,
                                             system_charset_info);
  table->field[MYSQL_USER_FIELD_USER]->store(user_name->user.str,
                                             user_name->user.length,
                                             system_charset_info);
  key_copy(user_key, table->record[0], table->key_info,
           table->key_info->key_length);

  if (table->file->ha_index_read_idx_map(table->record[0], 0, user_key,
                                         HA_WHOLE_KEY,
                                         HA_READ_KEY_EXACT))
  {
    String wrong_users;
    append_user(thd, &wrong_users, user_name, wrong_users.length() > 0, false);
    /* if user does not exists report error */
    my_error(ER_CANNOT_USER, MYF(0), "SHOW CREATE USER",
             wrong_users.c_ptr_safe());
    close_acl_tables(thd);
    DBUG_RETURN(1);
  }

  store_record(table,record[1]);
  mysql_mutex_lock(&acl_cache->lock);
  if (!(acl_user= find_acl_user(user_name->host.str, user_name->user.str, TRUE)))
  {
    mysql_mutex_unlock(&acl_cache->lock);
    String wrong_users;
    append_user(thd, &wrong_users, user_name, wrong_users.length() > 0, false);
    my_error(ER_CANNOT_USER, MYF(0), "SHOW CREATE USER",
             wrong_users.c_ptr_safe());
    close_acl_tables(thd);
    DBUG_RETURN(1);
  }
  /* fill in plugin, auth_str from acl_user */
  user_name->auth.str= acl_user->auth_string.str;
  user_name->auth.length= acl_user->auth_string.length;
  user_name->plugin= acl_user->plugin;
  user_name->uses_identified_by_clause= true;
  user_name->uses_identified_with_clause= false;
  user_name->uses_identified_by_password_clause= false;
  user_name->uses_authentication_string_clause= false;

  /* read user resources, ssl attributes from table */
  ssl_info= get_field(thd->mem_root, table->field[MYSQL_USER_FIELD_SSL_TYPE]);
  if (!ssl_info)
    acl_user->ssl_type=SSL_TYPE_NONE;
  else if (!strcmp(ssl_info, "ANY"))
    acl_user->ssl_type=SSL_TYPE_ANY;
  else if (!strcmp(ssl_info, "X509"))
    acl_user->ssl_type=SSL_TYPE_X509;
  else
    acl_user->ssl_type=SSL_TYPE_SPECIFIED;

  if (table->s->fields > MYSQL_USER_FIELD_SSL_CIPHER)
    acl_user->ssl_cipher= get_field(&global_acl_memory,
                                table->field[MYSQL_USER_FIELD_SSL_CIPHER]);
  if (table->s->fields > MYSQL_USER_FIELD_X509_ISSUER)
    acl_user->x509_issuer= get_field(&global_acl_memory,
                               table->field[MYSQL_USER_FIELD_X509_ISSUER]);
  if (table->s->fields > MYSQL_USER_FIELD_X509_SUBJECT)
    acl_user->x509_subject= get_field(&global_acl_memory,
                               table->field[MYSQL_USER_FIELD_X509_SUBJECT]);

  if (table->s->fields > MYSQL_USER_FIELD_MAX_USER_CONNECTIONS)
  {
     conn_attr= get_field(thd->mem_root,
                          table->field[MYSQL_USER_FIELD_MAX_USER_CONNECTIONS]);
     acl_user->user_resource.user_conn= conn_attr ? atoi(conn_attr) : 0;
     if (acl_user->user_resource.user_conn)
       acl_user->user_resource.specified_limits|= USER_RESOURCES::USER_CONNECTIONS;
  }
  if (table->s->fields > MYSQL_USER_FIELD_MAX_QUESTIONS)
  {
     conn_attr= get_field(thd->mem_root,
                          table->field[MYSQL_USER_FIELD_MAX_QUESTIONS]);
     acl_user->user_resource.questions= conn_attr ? atoi(conn_attr) : 0;
     if (acl_user->user_resource.questions)
       acl_user->user_resource.specified_limits|= USER_RESOURCES::QUERIES_PER_HOUR;
  }
  if (table->s->fields > MYSQL_USER_FIELD_MAX_UPDATES)
  {
     conn_attr= get_field(thd->mem_root,
                          table->field[MYSQL_USER_FIELD_MAX_UPDATES]);
     acl_user->user_resource.updates= conn_attr ? atoi(conn_attr) : 0;
     if (acl_user->user_resource.updates)
       acl_user->user_resource.specified_limits|= USER_RESOURCES::UPDATES_PER_HOUR;
  }
  if (table->s->fields > MYSQL_USER_FIELD_MAX_CONNECTIONS)
  {
     conn_attr= get_field(thd->mem_root,
                          table->field[MYSQL_USER_FIELD_MAX_CONNECTIONS]);
     acl_user->user_resource.conn_per_hour= conn_attr ? atoi(conn_attr) : 0;
     if (acl_user->user_resource.conn_per_hour)
       acl_user->user_resource.specified_limits|= USER_RESOURCES::CONNECTIONS_PER_HOUR;
  }

  /* make a copy of user resources, ssl and password expire attributes */
  tmp_user_resource= lex->mqh;
  lex->mqh= acl_user->user_resource;

  ssl_type= lex->ssl_type;
  ssl_cipher= lex->ssl_cipher;
  x509_issuer= lex->x509_issuer;
  x509_subject= lex->x509_subject;

  lex->ssl_type= acl_user->ssl_type;
  lex->ssl_cipher= const_cast<char*>(acl_user->ssl_cipher);
  lex->x509_issuer= const_cast<char*>(acl_user->x509_issuer);
  lex->x509_subject= const_cast<char*>(acl_user->x509_subject);

  alter_info= lex->alter_password;

  lex->alter_password.update_password_expired_column= acl_user->password_expired;
  lex->alter_password.use_default_password_lifetime= acl_user->use_default_password_lifetime;
  lex->alter_password.expire_after_days= acl_user->password_lifetime;
  lex->alter_password.account_locked = acl_user->account_locked;

  /* send the metadata to client */
  field=new Item_string("",0,&my_charset_latin1);
  field->max_length=256;
  strxmov(buff,"CREATE USER for ",user_name->user.str,"@",
          user_name->host.str,NullS);
  field->item_name.set(buff);
  field_list.push_back(field);
  if (protocol->send_result_set_metadata(&field_list, Protocol::SEND_NUM_ROWS))
  {
    error= 1;
    goto err;
  }
  sql_text.length(0);
  lex->users_list.push_back(user_name);
  mysql_rewrite_create_alter_user(thd, &sql_text);
  /* send the result row to client */
  protocol->prepare_for_resend();
  protocol->store(sql_text.ptr(),sql_text.length(),sql_text.charset());
  if (protocol->write())
  {
    error= 1;
    goto err;
  }

err:
  /* restore user resources, ssl and password expire attributes */
  lex->mqh= tmp_user_resource;
  lex->ssl_type= ssl_type;
  lex->ssl_cipher= ssl_cipher;
  lex->x509_issuer= x509_issuer;
  lex->x509_subject= x509_subject;

  lex->alter_password= alter_info;

  mysql_mutex_unlock(&acl_cache->lock);
  close_acl_tables(thd);
  my_eof(thd);
  DBUG_RETURN(error);
}

/**
  Update user.auth.str based on the plugin.

  @param thd        Thread context
  @param lex_user   user for which auth str needs to be generated

  @retval 0 ok
  @retval 1 ERROR;
*/

bool update_auth_str(THD *thd, LEX_USER *Str)
{
  plugin_ref plugin= NULL;
  char outbuf[MAX_FIELD_WIDTH]= {0};
  unsigned int buflen= MAX_FIELD_WIDTH, inbuflen;
  const char *inbuf;
  char *password= NULL;

  plugin= my_plugin_lock_by_name(0, Str->plugin,
                                 MYSQL_AUTHENTICATION_PLUGIN);

  /* for authentication_string generate hash out of it */
  if (plugin)
  {
    st_mysql_auth *auth= (st_mysql_auth *) plugin_decl(plugin)->info;
    {
      inbuf= Str->auth.str;
      inbuflen= Str->auth.length;
      if (auth->generate_authentication_string(outbuf,
                                               &buflen,
                                               inbuf,
                                               inbuflen))
      {
        plugin_unlock(0, plugin);
        return(1);
      }
      if (buflen)
      {
        password= (char *) thd->alloc(buflen);
        memcpy(password, outbuf, buflen);
      }
      else
        password= const_cast<char*>("");
      /* erase in memory copy of plain text password */
      memset((char*)(Str->auth.str), 0, Str->auth.length);
      /* Use the authentication_string field as password */
      Str->auth.str= password;
      Str->auth.length= buflen;
      thd->lex->contains_plaintext_password= false;
    }
    plugin_unlock(0, plugin);
  }
  else
  {
     my_error(ER_PLUGIN_IS_NOT_LOADED, MYF(0), Str->plugin.str);
     return(1);
  }
  return(0);
}

/**
   This function does following:
   1. Convert plain text password to hash and update the same in
      user definition.
   2. Validate hash string if specified in user definition.
   3. Identify what all fields needs to be updated in mysql.user
      table based on user definition.

  @param thd          Thread context
  @param Str          user on which attributes has to be applied
  @param what_to_set  User attributes

  @retval 0 ok
  @retval 1 ERROR;
*/

bool set_and_validate_user_attributes(THD *thd, LEX_USER *Str, ulong &what_to_set)
{
  bool user_exists= false;
  ACL_USER *acl_user;

  what_to_set= 0;
  /* update plugin,auth str attributes */
  if (Str->uses_identified_by_clause ||
      Str->uses_identified_by_password_clause ||
      Str->uses_identified_with_clause ||
      Str->uses_authentication_string_clause)
    what_to_set|= PLUGIN_ATTR;
  else
    what_to_set|= DEFAULT_AUTH_ATTR;

  /* update ssl attributes */
  if (thd->lex->ssl_type != SSL_TYPE_NOT_SPECIFIED)
    what_to_set|= SSL_ATTR;
  /* update connection attributes */
  if (thd->lex->mqh.specified_limits)
    what_to_set|= RESOURCE_ATTR;

  if ((acl_user= find_acl_user(Str->host.str, Str->user.str, TRUE)))
    user_exists= true;

  /* copy password expire attributes to individual user */
  Str->alter_status= thd->lex->alter_password;

  /* update password expire attributes */
  if (Str->alter_status.update_password_expired_column ||
      !Str->alter_status.use_default_password_lifetime ||
      Str->alter_status.expire_after_days)
    what_to_set|= PASSWORD_EXPIRE_ATTR;

  /* update account lock attribute */
  if (Str->alter_status.update_account_locked_column)
    what_to_set|= ACCOUNT_LOCK_ATTR;

  if (user_exists)
  {
    if (thd->lex->sql_command == SQLCOM_ALTER_USER)
    {
      if (Str->uses_identified_by_clause)
      {
        /* If no plugin is given, get existing plugin */
        if (!Str->uses_identified_with_clause)
          Str->plugin= acl_user->plugin;
      }
      /*
        always check for password expire/interval attributes as there is no
        way to differentiate NEVER EXPIRE and EXPIRE DEFAULT scenario
      */
      what_to_set|= PASSWORD_EXPIRE_ATTR;
    }
    else
    {
      /* if IDENTIFIED WITH is not specified set plugin from cache */
      if (!Str->uses_identified_with_clause)
      {
        Str->plugin= acl_user->plugin;
        /* set auth str from cache when not specified for existing user */
        if (!(Str->uses_identified_by_clause ||
            Str->uses_identified_by_password_clause ||
            Str->uses_authentication_string_clause))
        {
          Str->auth.str= acl_user->auth_string.str;
          Str->auth.length= acl_user->auth_string.length;
        }
      }
    }
    /*
      if there is a plugin specified with no auth string, then set
      the account as expired.
    */
    if (Str->uses_identified_with_clause &&
        !(Str->uses_identified_by_clause ||
        Str->uses_authentication_string_clause))
    {
      Str->alter_status.update_password_expired_column= true;
      what_to_set|= PASSWORD_EXPIRE_ATTR;
    }
  }
  else
  {
    /* set default plugin for new users if not specified */
    if (!Str->uses_identified_with_clause)
      Str->plugin= default_auth_plugin_name;
  }
  /*
    If auth string is specified, change it to hash.
    Validate empty credentials for new user ex: CREATE USER u1;
  */
  if (Str->uses_identified_by_clause ||
      (Str->auth.length == 0 && !user_exists))
    if (update_auth_str(thd, Str))
      return(1);
  /* Validate hash string */
  if(Str->uses_identified_by_password_clause ||
     Str->uses_authentication_string_clause)
  {
    plugin_ref plugin= my_plugin_lock_by_name(0, Str->plugin,
                                              MYSQL_AUTHENTICATION_PLUGIN);
    if (plugin)
    {
      st_mysql_auth *auth= (st_mysql_auth *) plugin_decl(plugin)->info;
      /*
        Validate hash string in following cases:
          1. IDENTIFIED BY PASSWORD.
          2. IDENTIFIED WITH .. AS 'auth_str' for ALTER USER statement
             and its a replication slave thread
      */
      if (Str->uses_identified_by_password_clause ||
          (Str->uses_authentication_string_clause &&
          thd->lex->sql_command == SQLCOM_ALTER_USER &&
          thd->slave_thread))
      {
        if (auth->validate_authentication_string((char*)Str->auth.str,
                                                 Str->auth.length))
        {
          my_error(ER_PASSWORD_FORMAT, MYF(0));
          plugin_unlock(0, plugin);
          return(1);
        }
      }
      plugin_unlock(0, plugin);
    }
    else
    {
      my_error(ER_PLUGIN_IS_NOT_LOADED, MYF(0), Str->plugin.str);
      return(1);
    }
  }
  return(0);
}

/**
  Change a password hash for a user.

  @param thd Thread handle
  @param host Hostname
  @param user User name
  @param new_password New password hash for host@user
 
  Note : it will also reset the change_password flag.
  This is safe to do unconditionally since the simple userless form
  SET PASSWORD = 'text' will be the only allowed form when
  this flag is on. So we don't need to check user names here.


  @see set_var_password::update(THD *thd)

  @return Error code
   @retval 0 ok
   @retval 1 ERROR; In this case the error is sent to the client.
*/

bool change_password(THD *thd, const char *host, const char *user,
                     char *new_password)
{
  TABLE_LIST tables;
  TABLE *table;
  LEX_USER *combo= NULL;
  /* Buffer should be extended when password length is extended. */
  char buff[512];
  /* buffer to store the hash string */
  char hash_str[MAX_FIELD_WIDTH]= {0};
  ulong query_length= 0;
  ulong what_to_set= 0;
  bool save_binlog_row_based;
  size_t new_password_len= strlen(new_password);
  bool result= 1;

  DBUG_ENTER("change_password");
  DBUG_PRINT("enter",("host: '%s'  user: '%s'  new_password: '%s'",
                      host,user,new_password));
  DBUG_ASSERT(host != 0);                        // Ensured by parent

  if (check_change_password(thd, host, user, new_password, new_password_len))
    DBUG_RETURN(1);

  tables.init_one_table("mysql", 5, "user", 4, "user", TL_WRITE);

#ifdef HAVE_REPLICATION
  /*
    GRANT and REVOKE are applied the slave in/exclusion rules as they are
    some kind of updates to the mysql.% tables.
  */
  if (thd->slave_thread && rpl_filter->is_on())
  {
    /*
      The tables must be marked "updating" so that tables_ok() takes them into
      account in tests.  It's ok to leave 'updating' set after tables_ok.
    */
    tables.updating= 1;
    /* Thanks to memset, tables.next==0 */
    if (!(thd->sp_runtime_ctx || rpl_filter->tables_ok(0, &tables)))
      DBUG_RETURN(0);
  }
#endif
  if (!(table= open_ltable(thd, &tables, TL_WRITE, MYSQL_LOCK_IGNORE_TIMEOUT)))
    DBUG_RETURN(1);

  if (!table->key_info)
  {
    my_error(ER_TABLE_CORRUPT, MYF(0), table->s->db.str,
             table->s->table_name.str);
    DBUG_RETURN(1);
  }

  /*
    This statement will be replicated as a statement, even when using
    row-based replication.  The flag will be reset at the end of the
    statement.
  */
  if ((save_binlog_row_based= thd->is_current_stmt_binlog_format_row()))
    thd->clear_current_stmt_binlog_format_row();

  mysql_mutex_lock(&acl_cache->lock);
  ACL_USER *acl_user;
  if (!(acl_user= find_acl_user(host, user, TRUE)))
  {
    mysql_mutex_unlock(&acl_cache->lock);
    my_message(ER_PASSWORD_NO_MATCH, ER(ER_PASSWORD_NO_MATCH), MYF(0));
    goto end;
  }

  DBUG_ASSERT(acl_user->plugin.length != 0);
  
  if (!(combo=(LEX_USER*) thd->alloc(sizeof(st_lex_user))))
    DBUG_RETURN(1);

  combo->user.str= user;
  combo->host.str= host;
  combo->user.length= strlen(user);
  combo->host.length= strlen(host);

  thd->make_lex_string(&combo->user,
                       combo->user.str, strlen(combo->user.str), 0);
  thd->make_lex_string(&combo->host,
                       combo->host.str, strlen(combo->host.str), 0);

  combo->plugin= EMPTY_CSTR;
  combo->auth.str= new_password;
  combo->auth.length= new_password_len;
  combo->uses_identified_by_clause= true;
  combo->uses_identified_with_clause= false;
  combo->uses_identified_by_password_clause= false;
  combo->uses_authentication_string_clause= false;
  /* set default values */
  thd->lex->ssl_type= SSL_TYPE_NOT_SPECIFIED;
  memset(&(thd->lex->mqh), 0, sizeof(thd->lex->mqh));
  thd->lex->alter_password.update_password_expired_column= false;
  thd->lex->alter_password.use_default_password_lifetime= true;
  thd->lex->alter_password.expire_after_days= 0;
  thd->lex->alter_password.update_account_locked_column= false;
  thd->lex->alter_password.account_locked= false;

  /*
    When @@log-backward-compatible-user-definitions variable is ON
    and its a slave thread, then the password is already hashed. So
    do not generate another hash.
  */
  if (opt_log_backward_compatible_user_definitions &&
      thd->slave_thread)
    combo->uses_identified_by_clause= false;
    
  if (set_and_validate_user_attributes(thd, combo, what_to_set))
  {
    result= 1;
    mysql_mutex_unlock(&acl_cache->lock);
    goto end;
  }
  if (replace_user_table(thd, table, combo, 0, 0, 1, what_to_set))
  {
    result= 1;
    mysql_mutex_unlock(&acl_cache->lock);
    goto end;
  }
  if (!update_sctx_cache(thd->security_context(), acl_user, false) &&
       thd->security_context()->password_expired())
  {
    /* the current user is not the same as the user we operate on */
    my_error(ER_MUST_CHANGE_PASSWORD, MYF(0));
    result= 1;
    mysql_mutex_unlock(&acl_cache->lock);
    goto end;
  }

  mysql_mutex_unlock(&acl_cache->lock);
  result= 0;
  /*
    Based on @@log-backward-compatible-user-definitions variable
    rewrite SET PASSWORD
  */
  if (opt_log_backward_compatible_user_definitions)
  {
    memcpy(hash_str, combo->auth.str, combo->auth.length);
    query_length= sprintf(buff, "SET PASSWORD FOR '%-.120s'@'%-.120s'='%s'",
                          acl_user->user ? acl_user->user : "",
                          acl_user->host.get_host() ? acl_user->host.get_host() : "",
                          hash_str);
  }
  else
    query_length= sprintf(buff, "ALTER USER '%-.120s'@'%-.120s' IDENTIFIED WITH '%-.120s' AS '%s'",
                          acl_user->user ? acl_user->user : "",
                          acl_user->host.get_host() ? acl_user->host.get_host() : "",
                          acl_user->plugin.str,
                          acl_user->auth_string.str);
  result= write_bin_log(thd, true, buff, query_length,
                        table->file->has_transactions());
end:
  result|= acl_trans_commit_and_close_tables(thd);

  if (!result)
    acl_notify_htons(thd, buff, query_length);

  /* Restore the state of binlog format */
  DBUG_ASSERT(!thd->is_current_stmt_binlog_format_row());
  if (save_binlog_row_based)
    thd->set_current_stmt_binlog_format_row();

  DBUG_RETURN(result);
}

/**
  Handle an in-memory privilege structure.

  @param struct_no  The number of the structure to handle (0..5).
  @param drop       If user_from is to be dropped.
  @param user_from  The the user to be searched/dropped/renamed.
  @param user_to    The new name for the user if to be renamed, NULL otherwise.

  @note
    Scan through all elements in an in-memory grant structure and apply
    the requested operation.
    Delete from grant structure if drop is true.
    Update in grant structure if drop is false and user_to is not NULL.
    Search in grant structure if drop is false and user_to is NULL.
    Structures are enumerated as follows:
    0 ACL_USER
    1 ACL_DB
    2 COLUMN_PRIVILIGES_HASH
    3 PROC_PRIVILEGES_HASH
    4 FUNC_PRIVILEGES_HASH
    5 ACL_PROXY_USERS

  @retval > 0  At least one element matched.
  @retval 0    OK, but no element matched.
  @retval -1   Wrong arguments to function or Out of Memory.
*/

static int handle_grant_struct(enum enum_acl_lists struct_no, bool drop,
                               LEX_USER *user_from, LEX_USER *user_to)
{
  int result= 0;
  size_t idx;
  size_t elements;
  const char *user= NULL;
  const char *host= NULL;
  ACL_USER *acl_user= NULL;
  ACL_DB *acl_db= NULL;
  ACL_PROXY_USER *acl_proxy_user= NULL;
  GRANT_NAME *grant_name= NULL;
  /*
    Dynamic array acl_grant_name used to store pointers to all
    GRANT_NAME objects
  */
  Prealloced_array<GRANT_NAME *, 16> acl_grant_name(PSI_INSTRUMENT_ME);
  HASH *grant_name_hash= NULL;
  DBUG_ENTER("handle_grant_struct");
  DBUG_PRINT("info",("scan struct: %u  search: '%s'@'%s'",
                     struct_no, user_from->user.str, user_from->host.str));

  mysql_mutex_assert_owner(&acl_cache->lock);
  /* will be locked if PROXY_USERS_ACL */
  Write_lock proxy_users_wlk(&proxy_users_rwlock, DEFER);

  /* Get the number of elements in the in-memory structure. */
  switch (struct_no) {
  case USER_ACL:
    elements= acl_users->size();
    break;
  case DB_ACL:
    elements= acl_dbs->size();
    break;
  case COLUMN_PRIVILEGES_HASH:
    elements= column_priv_hash.records;
    grant_name_hash= &column_priv_hash;
    break;
  case PROC_PRIVILEGES_HASH:
    elements= proc_priv_hash.records;
    grant_name_hash= &proc_priv_hash;
    break;
  case FUNC_PRIVILEGES_HASH:
    elements= func_priv_hash.records;
    grant_name_hash= &func_priv_hash;
    break;
  case PROXY_USERS_ACL:
    proxy_users_wlk.lock();
    elements= acl_proxy_users->size();
    break;
  default:
    DBUG_RETURN(-1);
  }

#ifdef EXTRA_DEBUG
    DBUG_PRINT("loop",("scan struct: %u  search    user: '%s'  host: '%s'",
                       struct_no, user_from->user.str, user_from->host.str));
#endif
  /* Loop over all elements. */
  for (idx= 0; idx < elements; idx++)
  {
    /*
      Get a pointer to the element.
    */
    switch (struct_no) {
    case USER_ACL:
      acl_user= &acl_users->at(idx);
      user= acl_user->user;
      host= acl_user->host.get_host();
    break;

    case DB_ACL:
      acl_db= &acl_dbs->at(idx);
      user= acl_db->user;
      host= acl_db->host.get_host();
      break;

    case COLUMN_PRIVILEGES_HASH:
    case PROC_PRIVILEGES_HASH:
    case FUNC_PRIVILEGES_HASH:
      grant_name= (GRANT_NAME*) my_hash_element(grant_name_hash, idx);
      user= grant_name->user;
      host= grant_name->host.get_host();
      break;

    case PROXY_USERS_ACL:
      acl_proxy_user= &acl_proxy_users->at(idx);
      user= acl_proxy_user->get_user();
      host= acl_proxy_user->host.get_host();
      break;

    default:
      MY_ASSERT_UNREACHABLE();
    }
    if (! user)
      user= "";
    if (! host)
      host= "";

#ifdef EXTRA_DEBUG
    DBUG_PRINT("loop",("scan struct: %u  index: %zu  user: '%s'  host: '%s'",
                       struct_no, idx, user, host));
#endif
    if (strcmp(user_from->user.str, user) ||
        my_strcasecmp(system_charset_info, user_from->host.str, host))
      continue;

    result= 1; /* At least one element found. */
    if ( drop )
    {
      switch ( struct_no ) {
      case USER_ACL:
        acl_users->erase(idx);
        elements--;
        /*
        - If we are iterating through an array then we just have moved all
          elements after the current element one position closer to its head.
          This means that we have to take another look at the element at
          current position as it is a new element from the array's tail.
        - This is valid for case USER_ACL, DB_ACL and PROXY_USERS_ACL.
        */
        idx--;
        break;

      case DB_ACL:
        acl_dbs->erase(idx);
        elements--;
        idx--;
        break;

      case COLUMN_PRIVILEGES_HASH:
      case PROC_PRIVILEGES_HASH:
      case FUNC_PRIVILEGES_HASH:
        /*
          Deleting while traversing a hash table is not valid procedure and
          hence we save pointers to GRANT_NAME objects for later processing.
        */
        if (acl_grant_name.push_back(grant_name))
          DBUG_RETURN(-1);
        break;

      case PROXY_USERS_ACL:
        acl_proxy_users->erase(idx);
        elements--;
        idx--;
        break;
      }
    }
    else if ( user_to )
    {
      switch ( struct_no ) {
      case USER_ACL:
        acl_user->user= strdup_root(&global_acl_memory, user_to->user.str);
        acl_user->host.update_hostname(strdup_root(&global_acl_memory, user_to->host.str));
        break;

      case DB_ACL:
        acl_db->user= strdup_root(&global_acl_memory, user_to->user.str);
        acl_db->host.update_hostname(strdup_root(&global_acl_memory, user_to->host.str));
        break;

      case COLUMN_PRIVILEGES_HASH:
      case PROC_PRIVILEGES_HASH:
      case FUNC_PRIVILEGES_HASH:
        /*
          Updating while traversing a hash table is not valid procedure and
          hence we save pointers to GRANT_NAME objects for later processing.
        */
        if (acl_grant_name.push_back(grant_name))
          DBUG_RETURN(-1);
        break;

      case PROXY_USERS_ACL:
        acl_proxy_user->set_user(&global_acl_memory, user_to->user.str);
        acl_proxy_user->host.update_hostname((user_to->host.str && *user_to->host.str) ? 
                                             strdup_root(&global_acl_memory, user_to->host.str) : NULL);
        break;
      }
    }
    else
    {
      /* If search is requested, we do not need to search further. */
      break;
    }
  }

  proxy_users_wlk.unlock();

  if (drop || user_to)
  {
    /*
      Traversing the elements stored in acl_grant_name dynamic array
      to either delete or update them.
    */
    for (GRANT_NAME **iter= acl_grant_name.begin();
         iter != acl_grant_name.end(); ++iter)
    {
      grant_name= *iter;

      if (drop)
      {
        my_hash_delete(grant_name_hash, (uchar *) grant_name);
      }
      else
      {
        /*
          Save old hash key and its length to be able properly update
          element position in hash.
        */
        char *old_key= grant_name->hash_key;
        size_t old_key_length= grant_name->key_length;

        /*
          Update the grant structure with the new user name and host name.
        */
        grant_name->set_user_details(user_to->host.str, grant_name->db,
                                     user_to->user.str, grant_name->tname,
                                     TRUE);

        /*
          Since username is part of the hash key, when the user name
          is renamed, the hash key is changed. Update the hash to
          ensure that the position matches the new hash key value
        */
        my_hash_update(grant_name_hash, (uchar*) grant_name, (uchar*) old_key,
                       old_key_length);
      }
    }
  }

#ifdef EXTRA_DEBUG
  DBUG_PRINT("loop",("scan struct: %u  result %d", struct_no, result));
#endif

  DBUG_RETURN(result);
}


/*
  Handle all privilege tables and in-memory privilege structures.

  SYNOPSIS
    handle_grant_data()
    tables                      The array with the four open tables.
    drop                        If user_from is to be dropped.
    user_from                   The the user to be searched/dropped/renamed.
    user_to                     The new name for the user if to be renamed,
                                NULL otherwise.

  DESCRIPTION
    Go through all grant tables and in-memory grant structures and apply
    the requested operation.
    Delete from grant data if drop is true.
    Update in grant data if drop is false and user_to is not NULL.
    Search in grant data if drop is false and user_to is NULL.

  RETURN
    > 0         At least one element matched.
    0           OK, but no element matched.
    < 0         Error.
*/

static int handle_grant_data(TABLE_LIST *tables, bool drop,
                             LEX_USER *user_from, LEX_USER *user_to)
{
  int result= 0;
  int found;
  int ret;
  DBUG_ENTER("handle_grant_data");

  /* Handle user table. */
  if ((found= handle_grant_table(tables, 0, drop, user_from, user_to)) < 0)
  {
    /* Handle of table failed, don't touch the in-memory array. */
    result= -1;
  }
  else
  {
    /* Handle user array. */
    if (((ret= handle_grant_struct(USER_ACL, drop, user_from, user_to) > 0) &&
         ! result) || found)
    {
      result= 1; /* At least one record/element found. */
      /* If search is requested, we do not need to search further. */
      if (! drop && ! user_to)
        goto end;
    }
    else if (ret < 0)
    {
      result= -1;
      goto end;
    }
  }

  /* Handle db table. */
  if ((found= handle_grant_table(tables, 1, drop, user_from, user_to)) < 0)
  {
    /* Handle of table failed, don't touch the in-memory array. */
    result= -1;
  }
  else
  {
    /* Handle db array. */
    if ((((ret= handle_grant_struct(DB_ACL, drop, user_from, user_to) > 0) &&
          ! result) || found) && ! result)
    {
      result= 1; /* At least one record/element found. */
      /* If search is requested, we do not need to search further. */
      if (! drop && ! user_to)
        goto end;
    }
    else if (ret < 0)
    {
      result= -1;
      goto end;
    }
  }

  /* Handle stored routines table. */
  if ((found= handle_grant_table(tables, 4, drop, user_from, user_to)) < 0)
  {
    /* Handle of table failed, don't touch in-memory array. */
    result= -1;
  }
  else
  {
    /* Handle procs array. */
    if ((((ret= handle_grant_struct(PROC_PRIVILEGES_HASH, drop, user_from,
                                    user_to) > 0) && ! result) || found) &&
        ! result)
    {
      result= 1; /* At least one record/element found. */
      /* If search is requested, we do not need to search further. */
      if (! drop && ! user_to)
        goto end;
    }
    else if (ret < 0)
    {
      result= -1;
      goto end;
    }
    /* Handle funcs array. */
    if ((((ret= handle_grant_struct(FUNC_PRIVILEGES_HASH, drop, user_from,
                                    user_to) > 0) && ! result) || found) &&
        ! result)
    {
      result= 1; /* At least one record/element found. */
      /* If search is requested, we do not need to search further. */
      if (! drop && ! user_to)
        goto end;
    }
    else if (ret < 0)
    {
      result= -1;
      goto end;
    }
  }

  /* Handle tables table. */
  if ((found= handle_grant_table(tables, 2, drop, user_from, user_to)) < 0)
  {
    /* Handle of table failed, don't touch columns and in-memory array. */
    result= -1;
  }
  else
  {
    if (found && ! result)
    {
      result= 1; /* At least one record found. */
      /* If search is requested, we do not need to search further. */
      if (! drop && ! user_to)
        goto end;
    }

    /* Handle columns table. */
    if ((found= handle_grant_table(tables, 3, drop, user_from, user_to)) < 0)
    {
      /* Handle of table failed, don't touch the in-memory array. */
      result= -1;
    }
    else
    {
      /* Handle columns hash. */
      if ((((ret= handle_grant_struct(COLUMN_PRIVILEGES_HASH, drop, user_from,
                                      user_to) > 0) && ! result) || found) &&
          ! result)
        result= 1; /* At least one record/element found. */
      else if (ret < 0)
        result= -1;
    }
  }

  /* Handle proxies_priv table. */
  if (tables[5].table)
  {
    if ((found= handle_grant_table(tables, 5, drop, user_from, user_to)) < 0)
    {
      /* Handle of table failed, don't touch the in-memory array. */
      result= -1;
    }
    else
    {
      /* Handle proxies_priv array. */
      if (((ret= handle_grant_struct(PROXY_USERS_ACL, drop, user_from, user_to) > 0)
           && !result) || found)
        result= 1; /* At least one record/element found. */
      else if (ret < 0)
        result= -1;
    }
  }
 end:
  DBUG_RETURN(result);
}


/*
  Create a list of users.

  SYNOPSIS
    mysql_create_user()
    thd                         The current thread.
    list                        The users to create.

  RETURN
    FALSE       OK.
    TRUE        Error.
*/

bool mysql_create_user(THD *thd, List <LEX_USER> &list)
{
  int result;
  String wrong_users;
  LEX_USER *user_name, *tmp_user_name;
  List_iterator <LEX_USER> user_list(list);
  TABLE_LIST tables[GRANT_TABLES];
  bool some_users_created= FALSE;
  bool save_binlog_row_based;
  bool transactional_tables;
  ulong what_to_update= 0;
  bool is_anonymous_user= false;

  DBUG_ENTER("mysql_create_user");

  /*
    This statement will be replicated as a statement, even when using
    row-based replication.  The flag will be reset at the end of the
    statement.
  */
  if ((save_binlog_row_based= thd->is_current_stmt_binlog_format_row()))
    thd->clear_current_stmt_binlog_format_row();

  /* CREATE USER may be skipped on replication client. */
  if ((result= open_grant_tables(thd, tables, &transactional_tables)))
  {
    /* Restore the state of binlog format */
    DBUG_ASSERT(!thd->is_current_stmt_binlog_format_row());
    if (save_binlog_row_based)
      thd->set_current_stmt_binlog_format_row();
    DBUG_RETURN(result != 1);
  }

  mysql_rwlock_wrlock(&LOCK_grant);
  mysql_mutex_lock(&acl_cache->lock);

  while ((tmp_user_name= user_list++))
  {
    /*
      If tmp_user_name.user.str is == NULL then
      user_name := tmp_user_name.
      Else user_name.user := sctx->user
      TODO and all else is turned to NULL !! Why?
    */
    if (!(user_name= get_current_user(thd, tmp_user_name)))
    {
      result= TRUE;
      continue;
    }
    if (set_and_validate_user_attributes(thd, user_name, what_to_update))
    {
      result= TRUE;
      continue;
    }
    if (!strcmp(user_name->user.str,"") &&
        (what_to_update & PASSWORD_EXPIRE_ATTR))
    {
      is_anonymous_user= true;
      result= true;
      continue;
    }

    /*
      Search all in-memory structures and grant tables
      for a mention of the new user name.
    */
    if (handle_grant_data(tables, 0, user_name, NULL))
    {
      append_user(thd, &wrong_users, user_name, wrong_users.length() > 0,
                  false);
      result= TRUE;
      continue;
    }

    if (replace_user_table(thd, tables[0].table, user_name, 0, 0, 1, what_to_update))
    {
      append_user(thd, &wrong_users, user_name, wrong_users.length() > 0,
                  false);
      result= TRUE;
      continue;
    }

    some_users_created= TRUE;
  } // END while tmp_user_name= user_lists++

  mysql_mutex_unlock(&acl_cache->lock);

  if (result)
  {
    if (is_anonymous_user)
      my_error(ER_CANNOT_USER, MYF(0), "CREATE USER", "anonymous user");
    else
      my_error(ER_CANNOT_USER, MYF(0), "CREATE USER", wrong_users.c_ptr_safe());
  }

  if (some_users_created)
  {
    String *rlb= &thd->rewritten_query;
    rlb->mem_free();
    mysql_rewrite_create_alter_user(thd, rlb);

    if (!thd->rewritten_query.length())
      result|= write_bin_log(thd, false, thd->query().str, thd->query().length,
                             transactional_tables);
    else
      result|= write_bin_log(thd, false,
                             thd->rewritten_query.c_ptr_safe(),
                             thd->rewritten_query.length(),
                             transactional_tables);
  }

  mysql_rwlock_unlock(&LOCK_grant);

  result|= acl_trans_commit_and_close_tables(thd);

  if (some_users_created && !result)
    acl_notify_htons(thd, thd->query().str, thd->query().length);

  /* Restore the state of binlog format */
  DBUG_ASSERT(!thd->is_current_stmt_binlog_format_row());
  if (save_binlog_row_based)
    thd->set_current_stmt_binlog_format_row();
  DBUG_RETURN(result);
}


/*
  Drop a list of users and all their privileges.

  SYNOPSIS
    mysql_drop_user()
    thd                         The current thread.
    list                        The users to drop.

  RETURN
    FALSE       OK.
    TRUE        Error.
*/

bool mysql_drop_user(THD *thd, List <LEX_USER> &list)
{
  int result;
  String wrong_users;
  LEX_USER *user_name, *tmp_user_name;
  List_iterator <LEX_USER> user_list(list);
  TABLE_LIST tables[GRANT_TABLES];
  bool some_users_deleted= FALSE;
  sql_mode_t old_sql_mode= thd->variables.sql_mode;
  bool save_binlog_row_based;
  bool transactional_tables;
  DBUG_ENTER("mysql_drop_user");

  /*
    This statement will be replicated as a statement, even when using
    row-based replication.  The flag will be reset at the end of the
    statement.
  */
  if ((save_binlog_row_based= thd->is_current_stmt_binlog_format_row()))
    thd->clear_current_stmt_binlog_format_row();

  /* DROP USER may be skipped on replication client. */
  if ((result= open_grant_tables(thd, tables, &transactional_tables)))
  {
    /* Restore the state of binlog format */
    DBUG_ASSERT(!thd->is_current_stmt_binlog_format_row());
    if (save_binlog_row_based)
      thd->set_current_stmt_binlog_format_row();
    DBUG_RETURN(result != 1);
  }

  thd->variables.sql_mode&= ~MODE_PAD_CHAR_TO_FULL_LENGTH;

  mysql_rwlock_wrlock(&LOCK_grant);
  mysql_mutex_lock(&acl_cache->lock);

  while ((tmp_user_name= user_list++))
  {
    if (!(user_name= get_current_user(thd, tmp_user_name)))
    {
      result= TRUE;
      continue;
    }  
    if (handle_grant_data(tables, 1, user_name, NULL) <= 0)
    {
      append_user(thd, &wrong_users, user_name, wrong_users.length() > 0, FALSE);
      result= TRUE;
      continue;
    }
    some_users_deleted= TRUE;
  }

  /* Rebuild 'acl_check_hosts' since 'acl_users' has been modified */
  rebuild_check_host();

  mysql_mutex_unlock(&acl_cache->lock);

  if (result)
    my_error(ER_CANNOT_USER, MYF(0), "DROP USER", wrong_users.c_ptr_safe());

  if (some_users_deleted)
    result |= write_bin_log(thd, FALSE, thd->query().str, thd->query().length,
                            transactional_tables);

  mysql_rwlock_unlock(&LOCK_grant);

  result|= acl_trans_commit_and_close_tables(thd);

  if (some_users_deleted && !result)
    acl_notify_htons(thd, thd->query().str, thd->query().length);

  thd->variables.sql_mode= old_sql_mode;
  /* Restore the state of binlog format */
  DBUG_ASSERT(!thd->is_current_stmt_binlog_format_row());
  if (save_binlog_row_based)
    thd->set_current_stmt_binlog_format_row();
  DBUG_RETURN(result);
}


/*
  Rename a user.

  SYNOPSIS
    mysql_rename_user()
    thd                         The current thread.
    list                        The user name pairs: (from, to).

  RETURN
    FALSE       OK.
    TRUE        Error.
*/

bool mysql_rename_user(THD *thd, List <LEX_USER> &list)
{
  int result;
  String wrong_users;
  LEX_USER *user_from, *tmp_user_from;
  LEX_USER *user_to, *tmp_user_to;
  List_iterator <LEX_USER> user_list(list);
  TABLE_LIST tables[GRANT_TABLES];
  bool some_users_renamed= FALSE;
  bool save_binlog_row_based;
  bool transactional_tables;
  DBUG_ENTER("mysql_rename_user");

  /*
    This statement will be replicated as a statement, even when using
    row-based replication.  The flag will be reset at the end of the
    statement.
  */
  if ((save_binlog_row_based= thd->is_current_stmt_binlog_format_row()))
    thd->clear_current_stmt_binlog_format_row();

  /* RENAME USER may be skipped on replication client. */
  if ((result= open_grant_tables(thd, tables, &transactional_tables)))
  {
    /* Restore the state of binlog format */
    DBUG_ASSERT(!thd->is_current_stmt_binlog_format_row());
    if (save_binlog_row_based)
      thd->set_current_stmt_binlog_format_row();
    DBUG_RETURN(result != 1);
  }

  mysql_rwlock_wrlock(&LOCK_grant);
  mysql_mutex_lock(&acl_cache->lock);

  while ((tmp_user_from= user_list++))
  {
    if (!(user_from= get_current_user(thd, tmp_user_from)))
    {
      result= TRUE;
      continue;
    }  
    tmp_user_to= user_list++;
    if (!(user_to= get_current_user(thd, tmp_user_to)))
    {
      result= TRUE;
      continue;
    }  
    DBUG_ASSERT(user_to != 0); /* Syntax enforces pairs of users. */

    /*
      Search all in-memory structures and grant tables
      for a mention of the new user name.
    */
    if (handle_grant_data(tables, 0, user_to, NULL) ||
        handle_grant_data(tables, 0, user_from, user_to) <= 0)
    {
      append_user(thd, &wrong_users, user_from, wrong_users.length() > 0, FALSE);
      result= TRUE;
      continue;
    }
    some_users_renamed= TRUE;
  }
  
  /* Rebuild 'acl_check_hosts' since 'acl_users' has been modified */
  rebuild_check_host();

  mysql_mutex_unlock(&acl_cache->lock);

  if (result)
    my_error(ER_CANNOT_USER, MYF(0), "RENAME USER", wrong_users.c_ptr_safe());
  
  if (some_users_renamed)
    result |= write_bin_log(thd, FALSE, thd->query().str, thd->query().length,
                            transactional_tables);

  mysql_rwlock_unlock(&LOCK_grant);

  result|= acl_trans_commit_and_close_tables(thd);

  if (some_users_renamed && !result)
    acl_notify_htons(thd, thd->query().str, thd->query().length);

  /* Restore the state of binlog format */
  DBUG_ASSERT(!thd->is_current_stmt_binlog_format_row());
  if (save_binlog_row_based)
    thd->set_current_stmt_binlog_format_row();
  DBUG_RETURN(result);
}


/*
  Alter user list.

  SYNOPSIS
    mysql_alter_user()
    thd                         The current thread.
    list                        The user names.

  RETURN
    FALSE       OK.
    TRUE        Error.
*/

bool mysql_alter_user(THD *thd, List <LEX_USER> &list)
{
  bool result= false;
  bool is_anonymous_user= false;
  String wrong_users;
  LEX_USER *user_from, *tmp_user_from;
  List_iterator <LEX_USER> user_list(list);
  TABLE_LIST tables;
  TABLE *table;
  bool some_user_altered= false;
  bool save_binlog_row_based;

  DBUG_ENTER("mysql_alter_user");

  if (!initialized)
  {
    my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0), "--skip-grant-tables");
    DBUG_RETURN(true);
  }
  tables.init_one_table("mysql", 5, "user", 4, "user", TL_WRITE);

#ifdef HAVE_REPLICATION
  /*
    GRANT and REVOKE are applied the slave in/exclusion rules as they are
    some kind of updates to the mysql.% tables.
  */
  if (thd->slave_thread && rpl_filter->is_on())
  {
    /*
      The tables must be marked "updating" so that tables_ok() takes them into
      account in tests.  It's ok to leave 'updating' set after tables_ok.
    */
    tables.updating= 1;
    /* Thanks to memset, tables.next==0 */
    if (!(thd->sp_runtime_ctx || rpl_filter->tables_ok(0, &tables)))
      DBUG_RETURN(false);
  }
#endif
  if (!(table= open_ltable(thd, &tables, TL_WRITE, MYSQL_LOCK_IGNORE_TIMEOUT)))
    DBUG_RETURN(true);

  if (!table->key_info)
  {
    my_error(ER_TABLE_CORRUPT, MYF(0), table->s->db.str,
             table->s->table_name.str);
    DBUG_RETURN(true);
  }

  /*
    This statement will be replicated as a statement, even when using
    row-based replication.  The flag will be reset at the end of the
    statement.
  */
  if ((save_binlog_row_based= thd->is_current_stmt_binlog_format_row()))
    thd->clear_current_stmt_binlog_format_row();

  mysql_rwlock_wrlock(&LOCK_grant);
  mysql_mutex_lock(&acl_cache->lock);

  while ((tmp_user_from= user_list++))
  {
    ACL_USER *acl_user;
    ulong what_to_alter= 0;

    /* add the defaults where needed */
    if (!(user_from= get_current_user(thd, tmp_user_from)))
    {
      result= true;
      append_user(thd, &wrong_users, tmp_user_from, wrong_users.length() > 0,
                  false);
      continue;
    }

    /* look up the user */
    if (!(acl_user= find_acl_user(user_from->host.str,
                                   user_from->user.str, TRUE)))
    {
      result= true;
      append_user(thd, &wrong_users, user_from, wrong_users.length() > 0,
                  false);
      continue;
    }

    /* copy password expire attributes to individual lex user */
    user_from->alter_status= thd->lex->alter_password;

    /*
      Check if the user's authentication method supports expiration only
      if PASSWORD EXPIRE attribute is specified
    */
    if (user_from->alter_status.update_password_expired_column &&
        !auth_plugin_supports_expiration(acl_user->plugin.str))
    {
      result= true;
      append_user(thd, &wrong_users, user_from, wrong_users.length() > 0,
                  false);
      continue;
    }
    if (set_and_validate_user_attributes(thd, user_from, what_to_alter))
    {
      result= true;
      continue;
    }

    if (!acl_user->user && 
        (what_to_alter & PASSWORD_EXPIRE_ATTR) &&
        user_from->alter_status.update_password_expired_column)
    {
      result = true;
      is_anonymous_user = true;
      append_user(thd, &wrong_users, user_from, wrong_users.length() > 0,
        false);
      continue;
    }

    /* update the mysql.user table */
    if (replace_user_table(thd, table, user_from, 0, 0, 1,
                           what_to_alter))
    {
      append_user(thd, &wrong_users, user_from, wrong_users.length() > 0,
                  false);
      result= TRUE;
      continue;
    }
    some_user_altered= true;
    update_sctx_cache(thd->security_context(), acl_user,
                      user_from->alter_status.update_password_expired_column);
  }

  acl_cache->clear(1);                          // Clear locked hostname cache
  mysql_mutex_unlock(&acl_cache->lock);

  if (result)
  {
    if (is_anonymous_user)
      my_error(ER_PASSWORD_EXPIRE_ANONYMOUS_USER, MYF(0));
    else
      my_error(ER_CANNOT_USER, MYF(0), "ALTER USER", wrong_users.c_ptr_safe());
  }

  if (!result && some_user_altered)
  {
    /* do query rewrite for ALTER USER */
    String *rlb= &thd->rewritten_query;
    rlb->mem_free();
    mysql_rewrite_create_alter_user(thd, rlb);

    result= (write_bin_log(thd, false,
                           thd->rewritten_query.c_ptr_safe(),
                           thd->rewritten_query.length(),
                           table->file->has_transactions()) != 0);
  }

  mysql_rwlock_unlock(&LOCK_grant);

  result|= acl_trans_commit_and_close_tables(thd);

  if (some_user_altered && !result)
    acl_notify_htons(thd, thd->query().str, thd->query().length);

  /* Restore the state of binlog format */
  DBUG_ASSERT(!thd->is_current_stmt_binlog_format_row());
  if (save_binlog_row_based)
    thd->set_current_stmt_binlog_format_row();
  DBUG_RETURN(result);
}


#endif


