#ifndef SQLGEN_DYNAMIC_STATEMENT_HPP_
#define SQLGEN_DYNAMIC_STATEMENT_HPP_

#include <rfl.hpp>

#include "CreateAs.hpp"
#include "CreateIndex.hpp"
#include "CreateTable.hpp"
#include "DeleteFrom.hpp"
#include "Drop.hpp"
#include "Insert.hpp"
#include "SelectFrom.hpp"
#include "Union.hpp"
#include "Update.hpp"
#include "Write.hpp"

namespace sqlgen::dynamic {

using Statement =
    rfl::TaggedUnion<"stmt", CreateAs, CreateIndex, CreateTable, DeleteFrom,
                     Drop, Insert, SelectFrom, Union, Update, Write>;

}  // namespace sqlgen::dynamic

#endif
