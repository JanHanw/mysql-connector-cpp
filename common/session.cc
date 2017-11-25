/*
 * Copyright (c) 2015, 2016, Oracle and/or its affiliates. All rights reserved.
 *
 * The MySQL Connector/C++ is licensed under the terms of the GPLv2
 * <http://www.gnu.org/licenses/old-licenses/gpl-2.0.html>, like most
 * MySQL Connectors. There are special exceptions to the terms and
 * conditions of the GPLv2 as it is applied to this software, see the
 * FLOSS License Exception
 * <http://www.mysql.com/about/legal/licensing/foss-exception.html>.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
 */

#include <mysql_common.h>
#include <mysql/cdk.h>
#include <uuid_gen.h>
#include <uri_parser.h>

#include "settings.h"
#include "session.h"
#include "result.h"


using namespace ::mysqlx::common;
using TCPIP_options = cdk::ds::TCPIP::Options;
using TLS_options = TCPIP_options::TLS_options;


void Settings_impl::clear()
{
  m_data = Data();
}


void Settings_impl::set_from_uri(const std::string &uri)
{
  parser::URI_parser parser(uri);
  Setter set(*this);

  parser.process(set);
  set.commit();
}


TCPIP_options::auth_method_t get_auth(unsigned m)
{
  using DevAPI_type = Settings_impl::Auth_method;
  using CDK_type = TCPIP_options::auth_method_t;

  switch (DevAPI_type(m))
  {
#define AUTH_TO_CDK(X,N) \
  case DevAPI_type::X: return CDK_type::X;

    AUTH_METHOD_LIST(AUTH_TO_CDK)

  default:
    // Note: caller should ensure that argument has correct value
    assert(false);
  }

  return CDK_type(0); // quiet compiler warnings
}

TLS_options::SSL_MODE get_ssl_mode(unsigned m)
{
  using DevAPI_type = Settings_impl::SSL_mode;
  using CDK_type = TLS_options::SSL_MODE;

  switch (DevAPI_type(m))
  {
#define AUTH_TO_CDK(X,N) \
  case DevAPI_type::X: return CDK_type::X;

    SSL_MODE_LIST(AUTH_TO_CDK)

  default:
    // Note: caller should ensure that argument has correct value
    assert(false);
  }

  return CDK_type(0); // quiet compiler warnings
}


/*
  Initialize CDK connection options based on session settings. If secure is
  true then the connection is assumed to be secure (does not require
  encryption).
*/

void prepare_options(Settings_impl &settings, bool secure, TCPIP_options &opts)
{
  using Option = Settings_impl::Option;
  using SSL_mode = Settings_impl::SSL_mode;

  if (!settings.has_option(Option::USER))
    throw_error("USER option not defined");

  opts = TCPIP_options(
    string(settings.get(Option::USER).get_string()),
    settings.has_option(Option::PWD)
      ? &settings.get(Option::PWD).get_string() : nullptr
  );

  // Set basic options

  if (settings.has_option(Option::DB))
    opts.set_database(settings.get(Option::DB).get_string());

  // Set TLS options

  /*
    By default ssl-mode is REQUIRED. If ssl-mode was not explicitly set but
    ssl-ca was, then mode defaults to VERIFY_CA.
  */

  unsigned mode = unsigned(SSL_mode::REQUIRED);

  if (settings.has_option(Option::SSL_MODE))
    mode = (unsigned)settings.get(Option::SSL_MODE).get_uint();
  else if (settings.has_option(Option::SSL_CA))
    mode = unsigned(SSL_mode::VERIFY_CA);

  if (unsigned(SSL_mode::DISABLED) == mode)
  {
#ifdef WITH_SSL
    opts.set_tls(TLS_options::SSL_MODE::DISABLED);
#endif
  }
  else
#ifdef WITH_SSL
  {
    secure = true;

    TLS_options tls_opt(get_ssl_mode(mode));
    if (settings.has_option(Option::SSL_CA))
      tls_opt.set_ca(settings.get(Option::SSL_CA).get_string());
    opts.set_tls(tls_opt);
  }
#endif

  // Set authentication options

  if (settings.has_option(Option::AUTH))
    opts.set_auth_method(get_auth(
      (unsigned)settings.get(Option::AUTH).get_uint()
    ));
  else
  {
    opts.set_auth_method(
      secure ? TCPIP_options::PLAIN : TCPIP_options::MYSQL41
    );
  }

}


/*
  Initialize CDK data source based on collected settings.
*/

void Settings_impl::get_data_source(cdk::ds::Multi_source &src)
{
  cdk::ds::TCPIP::Options opts;

  /*
    A single-host connection over Unix domain socket is considered secure.
    Otherwise SSL connection will be configured by default.
  */
  bool secure = m_data.m_sock && (1 == m_data.m_host_cnt);

  prepare_options(*this, secure, opts);

  // Build the list of hosts based on current settings.

  src.clear();

  // if priorities were not set explicitly, assign decreasing starting from 100
  int prio = m_data.m_user_priorities ? -1 : 100;

  auto add_host = [this, &src, &opts](iterator &it, int prio) {

    string host("localhost");
    unsigned short  port = DEFAULT_MYSQLX_PORT;

    if (Option::PORT == it->first)
    {
      assert(0 == m_data.m_host_cnt);
    }
    else
    {
      assert(Option::HOST == it->first);
      host = it->second.get_string();
      ++it;
    }

    // Look for PORT

    if (it != end() && Option::PORT == it->first)
    {
      port = (unsigned short)it->second.get_uint();
      ++it;
    }

    // Look for priority

    if (0 > prio)
    {
      if (it == end() || Option::PRIORITY != it->first)
        throw_error("No priority specified for host ...");
      // note: value of PRIORITY option is checked for validity
      prio = (int)it->second.get_uint();
      ++it;
    }

    assert(0 <= prio && prio <= 100);

    /*
      If there are more options, there should be no PRIORITY option
      at this point.
    */
    assert(it == end() || Option::PRIORITY != it->first);

#ifdef WITH_SSL

    /*
      Set expected CN if ssl mode is VERIFY_IDENTITY. We expect CN to be
      the host name given by user when creating the session.
    */

    if (TLS_options::SSL_MODE::VERIFY_IDENTITY == opts.get_tls().ssl_mode())
    {
      TLS_options tls = opts.get_tls();
      tls.set_cn(host);
      opts.set_tls(tls);
    }
#endif

    src.add(cdk::ds::TCPIP(host, port), opts, (unsigned short)prio);
  };



  auto add_socket = [this, &src, &opts](iterator it, int prio) {
    // TODO
    assert(false);
  };


  for (auto it = begin(); it != end();)
  {
    switch (it->first)
    {
    case Option::HOST:
      add_host(it, prio--); break;

    case Option::SOCKET:
      add_socket(it, prio--); break;

    /*
      Note: if m_host_cnt > 0 then a HOST setting must be before PORT setting,
      so the case above should cover that HOST/PORT pair.
    */
    case Option::PORT:
      assert(0 == m_data.m_host_cnt);
      add_host(it, prio--);
      break;

    default:
      ++it;
    }
  }

  assert(0 < src.size());
}


// ---------------------------------------------------------------------------


void Session_impl::prepare_for_cmd()
{
  if (m_current_result)
    m_current_result->store();
  m_current_result = nullptr;
}


// ---------------------------------------------------------------------------

void mysqlx::common::GUID::generate()
{
  using namespace uuid;

  static const char *hex_digit = "0123456789ABCDEF";
  uuid_type uuid;

  generate_uuid(uuid);

  for (unsigned i = 0; i < sizeof(uuid) && 2*i < sizeof(m_data); ++i)
  {
    m_data[2*i] = hex_digit[uuid[i] >> 4];
    m_data[2 * i + 1] = hex_digit[uuid[i] % 16];
  }
}

