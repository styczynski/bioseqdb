/**
 * \file
 */
#pragma once

#include <stack>

extern "C" {
#include <access/xact.h>
}

namespace cppgres {

using command_id = ::CommandId;

struct transaction_id {

  transaction_id() : id_(InvalidTransactionId) {}
  transaction_id(::TransactionId id) : id_(id) {}
  transaction_id(const transaction_id &id) : id_(id.id_) {}

  static transaction_id current(bool acquire = true) {
    return transaction_id(
        ffi_guard{acquire ? ::GetCurrentTransactionId : ::GetCurrentTransactionIdIfAny}());
  }

  bool is_valid() const { return TransactionIdIsValid(id_); }

  bool operator==(const transaction_id &other) const { return TransactionIdEquals(id_, other.id_); }
  bool operator>(const transaction_id &other) const { return TransactionIdFollows(id_, other.id_); }
  bool operator>=(const transaction_id &other) const {
    return TransactionIdFollowsOrEquals(id_, other.id_);
  }
  bool operator<(const transaction_id &other) const {
    return TransactionIdPrecedes(id_, other.id_);
  }
  bool operator<=(const transaction_id &other) const {
    return TransactionIdPrecedesOrEquals(id_, other.id_);
  }

  bool did_abort() const { return is_valid() && TransactionIdDidAbort(id_); }
  bool did_commit() const { return is_valid() && TransactionIdDidCommit(id_); }

private:
  ::TransactionId id_;
};

static_assert(sizeof(transaction_id) == sizeof(::TransactionId));

struct internal_subtransaction {
  internal_subtransaction(bool commit = true)
      : owner(::CurrentResourceOwner), commit(commit), name("") {
    if (txns.empty()) {
      ffi_guard{::BeginInternalSubTransaction}(nullptr);
      txns.push(this);
    } else {
      throw std::runtime_error("internal subtransaction already started");
    }
  }

  internal_subtransaction(std::string_view name, bool commit = true)
      : owner(::CurrentResourceOwner), commit(commit), name(name) {
    if (txns.empty()) {
      ffi_guard{::BeginInternalSubTransaction}(this->name.c_str());
      txns.push(this);
    } else {
      throw std::runtime_error("internal subtransaction already started");
    }
  }

  ~internal_subtransaction() {
    txns.pop();
    if (commit) {
      ffi_guard{::ReleaseCurrentSubTransaction}();
    } else {
      ffi_guard{::RollbackAndReleaseCurrentSubTransaction}();
    }
    ::CurrentResourceOwner = owner;
  }

private:
  ::ResourceOwner owner;
  bool commit;
  std::string name;
  static inline std::stack<internal_subtransaction *> txns;
};

struct transaction {
  transaction(bool commit = true) : should_commit(commit), released(false) {
    ffi_guard([]() {
      if (!::IsTransactionState()) {
        ::SetCurrentStatementStartTimestamp();
        ::StartTransactionCommand();
        ::PushActiveSnapshot(::GetTransactionSnapshot());
      }
    })();
  }

  ~transaction() {
    if (!released) {
      ffi_guard([this]() {
        ::PopActiveSnapshot();
        if (should_commit) {
          ::CommitTransactionCommand();
        } else {
          ::AbortCurrentTransaction();
        }
      })();
    }
  }

  void commit() {
    ffi_guard([]() {
      ::PopActiveSnapshot();
      ::CommitTransactionCommand();
    })();
    released = true;
  }

  void rollback() {
    ffi_guard([]() {
      ::PopActiveSnapshot();
      ::AbortCurrentTransaction();
    })();
    released = true;
  }

private:
  bool should_commit;
  bool released;
};

} // namespace cppgres
