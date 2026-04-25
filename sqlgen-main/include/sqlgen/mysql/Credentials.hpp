#ifndef SQLGEN_MYSQL_CREDENTIALS_HPP_
#define SQLGEN_MYSQL_CREDENTIALS_HPP_

#include <string>

namespace sqlgen::mysql {

struct Credentials {
  std::string host;
  std::string user;
  std::string password;
  std::string dbname = "mysql";
  int port = 3306;
  std::string unix_socket = "/var/run/mysqld/mysqld.sock";
};

}  // namespace sqlgen::mysql

#endif
