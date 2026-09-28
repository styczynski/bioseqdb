/**
 * @file
 * @brief Checks SliceProject plan records, dependencies and output bindings before execution.
 */
module;
#include <bioseq/pg_compat.h>

extern "C" {
#include "catalog/namespace.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_language_d.h"
#include "catalog/pg_proc.h"
#include "commands/extension.h"
#include "optimizer/appendinfo.h"
#include "optimizer/planmain.h"
#include "storage/dsm.h"
#include "storage/shmem.h"
#include "utils/inval.h"
#include "utils/syscache.h"

#include "rewrite/rewriteManip.h"
}

export module bioseq.postgres.table:custom_scan_slice_path_codec_validation;

import std;
import bioseq.postgres.support;
import bioseq.postgres.types.compact_sequence;

import :custom_scan_slice_path_codec_contract;
import :custom_scan_slice_path_runtime_contract;

using namespace bioseq::postgres::support;

export {
    void slice_project_validate_codec(CustomScan* scan) {
        List* header = slice_project_plan_header(scan);
        List* nodes = slice_project_plan_nodes(scan);
        List* records = slice_project_plan_records(scan);
        List* output_bindings = slice_project_plan_output_bindings(scan);
        if (header == NIL || list_length(header) != k_bioseq_slice_project_header_fields ||
            !IsA(reinterpret_cast<Node*>(list_nth(header, k_slice_header_magic)), Integer) ||
            !IsA(reinterpret_cast<Node*>(list_nth(header, k_slice_header_version)), Integer) ||
            !IsA(reinterpret_cast<Node*>(list_nth(header, k_slice_header_boundary_count)),
                 Integer) ||
            !IsA(reinterpret_cast<Node*>(list_nth(header, k_slice_header_expression_count)),
                 Integer) ||
            !IsA(reinterpret_cast<Node*>(list_nth(header, k_slice_header_producer_count)),
                 Integer) ||
            !IsA(reinterpret_cast<Node*>(list_nth(header, k_slice_header_fallback_reason)),
                 String) ||
            intVal(list_nth(header, k_slice_header_magic)) != k_bioseq_slice_project_magic ||
            intVal(list_nth(header, k_slice_header_version)) !=
                k_bioseq_slice_project_codec_version) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject plan codec header is invalid"),
                     errdetail("header_fields=%d expected_fields=%d",
                               list_length(header),
                               k_bioseq_slice_project_header_fields)));
        }
        const int boundary_count = intVal(list_nth(header, k_slice_header_boundary_count));
        const int expression_count = intVal(list_nth(header, k_slice_header_expression_count));
        const int producer_count = intVal(list_nth(header, k_slice_header_producer_count));
        if (boundary_count < 0 || boundary_count != list_length(records) || expression_count < 0 ||
            expression_count != list_length(scan->custom_exprs) || producer_count < 1 ||
            producer_count != list_length(nodes)) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject plan codec cardinality is invalid"),
                     errdetail("boundary_count=%d record_count=%d "
                               "expression_count=%d custom_expression_count=%d "
                               "producer_count=%d node_count=%d",
                               boundary_count,
                               list_length(records),
                               expression_count,
                               list_length(scan->custom_exprs),
                               producer_count,
                               list_length(nodes))));
        }
        (void)slice_project_int8_value(
            reinterpret_cast<Node*>(list_nth(header, k_slice_header_piece_boundaries)));
        (void)slice_project_int8_value(
            reinterpret_cast<Node*>(list_nth(header, k_slice_header_source_pieces)));
        (void)slice_project_int8_value(
            reinterpret_cast<Node*>(list_nth(header, k_slice_header_planned_symbols)));
        (void)slice_project_int8_value(
            reinterpret_cast<Node*>(list_nth(header, k_slice_header_planned_bytes)));

        int expected_stable_id = 1;
        for (Node* raw_node : postgres_list<Node>(nodes)) {
            List* node = raw_node != nullptr && IsA(raw_node, List)
                             ? reinterpret_cast<List*>(raw_node)
                             : NIL;
            if (node == NIL || list_length(node) != k_bioseq_slice_project_node_fields) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqSliceProject IR node is invalid"),
                         errdetail("stable_id=%d node_fields=%d expected_fields=%d",
                                   expected_stable_id,
                                   list_length(node),
                                   k_bioseq_slice_project_node_fields)));
            }
            constexpr std::array<int, 9> integer_fields{k_slice_node_stable_id,
                                                        k_slice_node_opcode,
                                                        k_slice_node_expression_index,
                                                        k_slice_node_lower_expression_index,
                                                        k_slice_node_upper_expression_index,
                                                        k_slice_node_options_expression_index,
                                                        k_slice_node_transform,
                                                        k_slice_node_mechanism,
                                                        k_slice_node_reason};
            for (const int field : integer_fields) {
                Node* value = reinterpret_cast<Node*>(list_nth(node, field));
                if (value == nullptr || !IsA(value, Integer)) {
                    ereport(ERROR,
                            (errcode(ERRCODE_DATA_CORRUPTED),
                             errmsg("BioseqSliceProject IR integer field is invalid"),
                             errdetail("stable_id=%d field=%d node_tag=%d",
                                       expected_stable_id,
                                       field,
                                       value == nullptr ? 0 : static_cast<int>(nodeTag(value)))));
                }
            }
            Node* flags_node = reinterpret_cast<Node*>(list_nth(node, k_slice_node_flags));
            Node* dependencies_node =
                reinterpret_cast<Node*>(list_nth(node, k_slice_node_dependencies));
            if (flags_node == nullptr || !IsA(flags_node, Integer) ||
                (dependencies_node != nullptr && !IsA(dependencies_node, List))) {
                ereport(
                    ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject IR topology field is invalid"),
                     errdetail("stable_id=%d flags_tag=%d dependencies_tag=%d",
                               expected_stable_id,
                               flags_node == nullptr ? 0 : static_cast<int>(nodeTag(flags_node)),
                               dependencies_node == nullptr
                                   ? 0
                                   : static_cast<int>(nodeTag(dependencies_node)))));
            }
            List* dependencies =
                dependencies_node == nullptr ? NIL : reinterpret_cast<List*>(dependencies_node);
            const int stable_id = intVal(list_nth(node, k_slice_node_stable_id));
            const int opcode = intVal(list_nth(node, k_slice_node_opcode));
            const int expression_index = intVal(list_nth(node, k_slice_node_expression_index));
            const int lower_expression_index =
                intVal(list_nth(node, k_slice_node_lower_expression_index));
            const int upper_expression_index =
                intVal(list_nth(node, k_slice_node_upper_expression_index));
            const int options_expression_index =
                intVal(list_nth(node, k_slice_node_options_expression_index));
            const int transform = intVal(list_nth(node, k_slice_node_transform));
            const int mechanism = intVal(list_nth(node, k_slice_node_mechanism));
            const int reason = intVal(list_nth(node, k_slice_node_reason));
            const int flags = intVal(flags_node);
            auto valid_optional_expression_index = [expression_count](int index) {
                return index == -1 || (index >= 0 && index < expression_count);
            };
            if (stable_id != expected_stable_id ||
                opcode < static_cast<int>(slice_project_opcode::source_expr) ||
                opcode > static_cast<int>(slice_project_opcode::materialize) ||
                expression_index < 0 || expression_index >= expression_count ||
                !valid_optional_expression_index(lower_expression_index) ||
                !valid_optional_expression_index(upper_expression_index) ||
                !valid_optional_expression_index(options_expression_index) || transform < 0 ||
                transform > 3 || flags < 0 ||
                (flags & ~(k_slice_node_volatile | k_slice_node_opaque)) != 0) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqSliceProject IR node metadata is invalid"),
                         errdetail("stable_id=%d expected_id=%d opcode=%d "
                                   "expression=%d lower=%d upper=%d options=%d "
                                   "transform=%d flags=%d",
                                   stable_id,
                                   expected_stable_id,
                                   opcode,
                                   expression_index,
                                   lower_expression_index,
                                   upper_expression_index,
                                   options_expression_index,
                                   transform,
                                   flags)));
            }
            Bitmapset* seen_dependencies = nullptr;
            for (Node* dependency : postgres_list<Node>(dependencies)) {
                if (dependency == nullptr || !IsA(dependency, Integer) || intVal(dependency) <= 0 ||
                    intVal(dependency) >= stable_id ||
                    bms_is_member(intVal(dependency), seen_dependencies)) {
                    ereport(
                        ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqSliceProject IR dependency is not topological"),
                         errdetail(
                             "stable_id=%d dependency_id=%d dependency_tag=%d",
                             stable_id,
                             dependency != nullptr && IsA(dependency, Integer) ? intVal(dependency)
                                                                               : 0,
                             dependency == nullptr ? 0 : static_cast<int>(nodeTag(dependency)))));
                }
                seen_dependencies = bms_add_member(seen_dependencies, intVal(dependency));
            }
            if (seen_dependencies != nullptr)
                bms_free(seen_dependencies);
            const auto typed_opcode = static_cast<slice_project_opcode>(opcode);
            const int dependency_count = list_length(dependencies);
            const bool leaf = typed_opcode == slice_project_opcode::source_expr ||
                              typed_opcode == slice_project_opcode::literal_expr;
            const bool unary = typed_opcode == slice_project_opcode::element ||
                               typed_opcode == slice_project_opcode::slice ||
                               typed_opcode == slice_project_opcode::reverse ||
                               typed_opcode == slice_project_opcode::reverse_complement ||
                               typed_opcode == slice_project_opcode::length ||
                               typed_opcode == slice_project_opcode::cardinality ||
                               typed_opcode == slice_project_opcode::materialize;
            const bool materialize = typed_opcode == slice_project_opcode::materialize;
            const bool bounds_valid =
                (typed_opcode == slice_project_opcode::slice ||
                 typed_opcode == slice_project_opcode::element)
                    ? (typed_opcode == slice_project_opcode::element
                           ? lower_expression_index == -1 && upper_expression_index >= 0
                           : true)
                    : lower_expression_index == -1 && upper_expression_index == -1;
            const int expected_transform =
                typed_opcode == slice_project_opcode::reverse
                    ? 1
                    : (typed_opcode == slice_project_opcode::reverse_complement ? 3 : 0);
            const bool boundary_metadata_valid =
                materialize
                    ? mechanism >= static_cast<int>(materialization_mechanism::backing_copy) &&
                          mechanism <= static_cast<int>(materialization_mechanism::letter_array) &&
                          reason == static_cast<int>(materialization_reason::sql_conversion)
                    : mechanism == -1 && reason == -1;
            if ((leaf && dependency_count != 0) || (unary && dependency_count != 1) ||
                (typed_opcode == slice_project_opcode::concat && dependency_count < 1) ||
                (typed_opcode == slice_project_opcode::bridge && dependency_count < 1) ||
                !bounds_valid || transform != expected_transform || !boundary_metadata_valid ||
                options_expression_index != -1) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqSliceProject IR opcode contract is invalid"),
                         errdetail("stable_id=%d opcode=%d dependencies=%d "
                                   "lower=%d upper=%d options=%d transform=%d "
                                   "mechanism=%d reason=%d",
                                   stable_id,
                                   opcode,
                                   dependency_count,
                                   lower_expression_index,
                                   upper_expression_index,
                                   options_expression_index,
                                   transform,
                                   mechanism,
                                   reason)));
            }
            ++expected_stable_id;
        }

        std::uint32_t expected_boundary_id = 1u;
        int record_index = 0;
        for (Node* raw_record : postgres_list<Node>(records)) {
            List* record = raw_record != nullptr && IsA(raw_record, List)
                               ? reinterpret_cast<List*>(raw_record)
                               : NIL;
            if (record == NIL || list_length(record) != k_bioseq_slice_project_record_fields) {
                ereport(
                    ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject plan codec record is invalid"),
                     errdetail("record_index=%d record_node_tag=%d "
                               "record_fields=%d expected_fields=%d",
                               record_index,
                               raw_record == nullptr ? 0 : static_cast<int>(nodeTag(raw_record)),
                               list_length(record),
                               k_bioseq_slice_project_record_fields)));
            }
            Node* boundary_id_node =
                reinterpret_cast<Node*>(list_nth(record, k_slice_record_boundary_id));
            Node* producer_id_node =
                reinterpret_cast<Node*>(list_nth(record, k_slice_record_producer_id));
            Node* expression_index_node =
                reinterpret_cast<Node*>(list_nth(record, k_slice_record_expression_index));
            Node* mechanism_node =
                reinterpret_cast<Node*>(list_nth(record, k_slice_record_mechanism));
            Node* reason_node = reinterpret_cast<Node*>(list_nth(record, k_slice_record_reason));
            if (boundary_id_node == nullptr || !IsA(boundary_id_node, Integer) ||
                producer_id_node == nullptr || !IsA(producer_id_node, Integer) ||
                expression_index_node == nullptr || !IsA(expression_index_node, Integer) ||
                mechanism_node == nullptr || !IsA(mechanism_node, Integer) ||
                reason_node == nullptr || !IsA(reason_node, Integer)) {
                auto node_tag_or_zero = [](Node* node) noexcept {
                    return node == nullptr ? 0 : static_cast<int>(nodeTag(node));
                };
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqSliceProject plan codec topology is invalid"),
                         errdetail("record_index=%d integer_field_tags: "
                                   "boundary_id=%d producer_id=%d expression_index=%d "
                                   "mechanism=%d reason=%d",
                                   record_index,
                                   node_tag_or_zero(boundary_id_node),
                                   node_tag_or_zero(producer_id_node),
                                   node_tag_or_zero(expression_index_node),
                                   node_tag_or_zero(mechanism_node),
                                   node_tag_or_zero(reason_node))));
            }
            const int boundary_id = intVal(boundary_id_node);
            const int producer_id = intVal(producer_id_node);
            const int expression_index = intVal(expression_index_node);
            const int mechanism = intVal(mechanism_node);
            const int reason = intVal(reason_node);
            Node* dependency_node =
                reinterpret_cast<Node*>(list_nth(record, k_slice_record_dependencies));
            const bool dependency_shape_valid =
                dependency_node == nullptr || IsA(dependency_node, List);
            List* dependencies =
                dependency_node == nullptr
                    ? NIL
                    : (dependency_shape_valid ? reinterpret_cast<List*>(dependency_node) : NIL);
            if (boundary_id != static_cast<int>(expected_boundary_id) || producer_id <= 0 ||
                producer_id > producer_count || expression_index < 0 ||
                expression_index >= expression_count ||
                mechanism < static_cast<int>(materialization_mechanism::backing_copy) ||
                mechanism > static_cast<int>(materialization_mechanism::letter_array) ||
                reason < static_cast<int>(materialization_reason::executor_copy) ||
                reason > static_cast<int>(materialization_reason::client_output) ||
                !dependency_shape_valid) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqSliceProject plan codec topology is invalid"),
                         errdetail("record_index=%d boundary_id=%d "
                                   "expected_boundary_id=%u producer_id=%d "
                                   "producer_count=%d mechanism=%d reason=%d "
                                   "expression_index=%d expression_count=%d "
                                   "dependency_node_tag=%d",
                                   record_index,
                                   boundary_id,
                                   expected_boundary_id,
                                   producer_id,
                                   producer_count,
                                   mechanism,
                                   reason,
                                   expression_index,
                                   expression_count,
                                   dependency_node == nullptr
                                       ? 0
                                       : static_cast<int>(nodeTag(dependency_node)))));
            }
            int dependency_index = 0;
            for (Node* dependency : postgres_list<Node>(dependencies)) {
                if (dependency == nullptr || !IsA(dependency, Integer) || intVal(dependency) <= 0 ||
                    intVal(dependency) >= boundary_id) {
                    ereport(
                        ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqSliceProject dependency is not topological"),
                         errdetail(
                             "record_index=%d dependency_index=%d "
                             "boundary_id=%d dependency_id=%d dependency_node_tag=%d",
                             record_index,
                             dependency_index,
                             boundary_id,
                             dependency != nullptr && IsA(dependency, Integer) ? intVal(dependency)
                                                                               : 0,
                             dependency == nullptr ? 0 : static_cast<int>(nodeTag(dependency)))));
                }
                ++dependency_index;
            }
            (void)slice_project_int8_value(
                reinterpret_cast<Node*>(list_nth(record, k_slice_record_estimated_symbols)));
            Node* encoding = reinterpret_cast<Node*>(list_nth(record, k_slice_record_encoding));
            Node* cache_scope =
                reinterpret_cast<Node*>(list_nth(record, k_slice_record_cache_scope));
            if (encoding == nullptr || !IsA(encoding, String) || cache_scope == nullptr ||
                !IsA(cache_scope, String)) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqSliceProject boundary metadata is invalid"),
                         errdetail(
                             "record_index=%d encoding_node_tag=%d "
                             "cache_scope_node_tag=%d",
                             record_index,
                             encoding == nullptr ? 0 : static_cast<int>(nodeTag(encoding)),
                             cache_scope == nullptr ? 0 : static_cast<int>(nodeTag(cache_scope)))));
            }
            ++expected_boundary_id;
            ++record_index;
        }
        Bitmapset* seen_resnos = nullptr;
        int binding_index = 0;
        for (Node* raw_binding : postgres_list<Node>(output_bindings)) {
            List* binding = raw_binding != nullptr && IsA(raw_binding, List)
                                ? reinterpret_cast<List*>(raw_binding)
                                : NIL;
            if (list_length(binding) != 2 ||
                !IsA(reinterpret_cast<Node*>(linitial(binding)), Integer) ||
                !IsA(reinterpret_cast<Node*>(lsecond(binding)), Integer)) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqSliceProject output binding is invalid"),
                         errdetail("binding_index=%d", binding_index)));
            }
            const int resno = intVal(linitial(binding));
            const int producer_id = intVal(lsecond(binding));
            if (resno <= 0 || resno > list_length(scan->scan.plan.targetlist) || producer_id <= 0 ||
                producer_id > producer_count || bms_is_member(resno, seen_resnos)) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqSliceProject output binding is out of range"),
                         errdetail("binding_index=%d resno=%d producer_id=%d",
                                   binding_index,
                                   resno,
                                   producer_id)));
            }
            seen_resnos = bms_add_member(seen_resnos, resno);
            ++binding_index;
        }
        if (seen_resnos != nullptr)
            bms_free(seen_resnos);
    }

} // export custom-scan partition declarations
