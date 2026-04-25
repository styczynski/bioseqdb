#ifndef SQLGEN_POSTGRES_CREDENTIALS_HPP_
#define SQLGEN_POSTGRES_CREDENTIALS_HPP_

#include <functional>
#include <string>

namespace sqlgen::postgres {

struct Credentials {
  std::string user;
  std::string password;
  std::string host;
  std::string dbname;
  int port = 5432;
  std::function<void(const char*)> notice_handler;

  std::string to_str() const {
    return "postgresql://" + user + ":" + password + "@" + host + ":" +
           std::to_string(port) + "/" + dbname;
  }
};

}  // namespace sqlgen::postgres

#endif
