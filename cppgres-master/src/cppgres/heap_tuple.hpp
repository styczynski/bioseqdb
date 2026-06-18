#pragma once

#include "imports.h"
#include "xact.hpp"

namespace cppgres {

/**
 * @brief Heap tuple convenience wrapper
 */
struct heap_tuple {

  heap_tuple(::HeapTuple tuple) : tuple_(tuple) {}

  operator ::HeapTuple() const { return tuple_; }

  transaction_id xmin(bool raw = false) const {
    return raw ? HeapTupleHeaderGetRawXmin(tuple_->t_data) : HeapTupleHeaderGetXmin(tuple_->t_data);
  }

  transaction_id xmax() const { return HeapTupleHeaderGetRawXmax(tuple_->t_data); }

  transaction_id update_xid() const { return HeapTupleHeaderGetUpdateXid(tuple_->t_data); }

  command_id cmin() const { return HeapTupleHeaderGetCmin(tuple_->t_data); }
  command_id cmax() const { return HeapTupleHeaderGetCmax(tuple_->t_data); }

private:
  HeapTuple tuple_;
};

static_assert(sizeof(heap_tuple) == sizeof(::HeapTuple));

} // namespace cppgres
