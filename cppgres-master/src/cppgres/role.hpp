/**
 * \file
 */
#pragma once

#include <optional>
#include <span>
#include <string>
#include <utility>

#include "datum.hpp"
#include "syscache.hpp"
#include "type.hpp"

namespace cppgres {

struct role : oid {
  using oid::oid;
  role() : oid(::GetCurrentRoleId() != InvalidOid ? ::GetCurrentRoleId() : ::GetUserId()) {}
  role(std::string role_name) : oid(ffi_guard{::get_role_oid}(role_name.c_str(), false)) {}

  std::string_view name() const { return ffi_guard{::GetUserNameFromId}(*this, false); }

  bool is_member(role &other, bool ignore_superuser = false) {
    return ffi_guard{ignore_superuser ? ::is_member_of_role_nosuper : ::is_member_of_role}(*this,
                                                                                           other);
  }
};

template <> struct type_traits<role> {
  static bool is(const type &t) { return t.oid == OIDOID || t.oid == REGROLEOID; }
  static constexpr type type_for() { return type{.oid = REGROLEOID}; }
};

template <> struct datum_conversion<role> : default_datum_conversion<role> {
  static role from_datum(const datum &d, oid, std::optional<memory_context>) {
    return static_cast<role>(d.operator const ::Datum &());
  }

  static datum into_datum(const role &t) { return datum(static_cast<::Datum>(t)); }
};

enum security_context_flag : int {
  security_none = 0x0000,
  security_local_user_id_change = SECURITY_LOCAL_USERID_CHANGE,
  security_restricted_operation = SECURITY_RESTRICTED_OPERATION,
  security_noforce_rls = SECURITY_NOFORCE_RLS
};

constexpr security_context_flag operator|(security_context_flag a, security_context_flag b) {
  return static_cast<security_context_flag>(static_cast<int>(a) | static_cast<int>(b));
}

constexpr security_context_flag operator&(security_context_flag a, security_context_flag b) {
  return static_cast<security_context_flag>(static_cast<int>(a) & static_cast<int>(b));
}

constexpr security_context_flag &operator|=(security_context_flag &a, security_context_flag b) {
  return a = a | b;
}

struct security_context {
  security_context(role r, security_context_flag context = security_none) : role_(r) {
    ::GetUserIdAndSecContext(&user, &ctx);
    ::SetUserIdAndSecContext(role_, context);
  }
  ~security_context() { ::SetUserIdAndSecContext(user, ctx); }

  static bool in_local_user_id_change() { return ::InLocalUserIdChange(); }
  static bool in_security_restricted_operation() { return ::InSecurityRestrictedOperation(); }
  static bool in_no_force_rls_operation() { return ::InNoForceRLSOperation(); }

private:
  role role_;
  ::Oid user;
  int ctx;
};

} // namespace cppgres
