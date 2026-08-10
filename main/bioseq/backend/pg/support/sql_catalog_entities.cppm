/*-------------------------------------------------------------------------
 *
 * sql_catalog_entities.cppm
 *
 * Stateless typed handles for PostgreSQL relation-like catalog entities.
 * Generated owner facets instantiate these handles from collected SQL
 * declarations; ordinary consumers use only those generated variables.
 *
 *-------------------------------------------------------------------------
 */
module;

#include <bioseq/pg_compat.h>

extern "C" {
#include "access/htup_details.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "commands/extension.h"
#include "utils/builtins.h"
#include "utils/errcodes.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
}

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

export module bioseq.postgres.support.sql_catalog_entities;

import bioseq.postgres.support;

export namespace bioseq::postgres::support {

#if defined(BIOSEQ_TEST_INDEXES)
namespace catalog_entity_test_support {

inline thread_local std::optional<std::int64_t>
    lookup_barrier_key;

/**
 * Arm a one-shot, backend-local advisory-lock barrier after relation-name
 * lookup and before the validating pg_class syscache pin.
 */
inline void arm_lookup_barrier(std::int64_t key) noexcept {
    lookup_barrier_key = key;
}

inline void disarm_lookup_barrier() noexcept {
    lookup_barrier_key.reset();
}

/**
 * Block on the armed advisory lock, then immediately release the lock acquired
 * by this backend. The controller session holds the same lock while changing
 * the catalog object and observes this backend waiting through pg_locks.
 */
inline void pass_lookup_barrier() {
    if (!lookup_barrier_key.has_value())
        return;
    const std::int64_t key = *lookup_barrier_key;
    lookup_barrier_key.reset();
    bioseq::postgres::support::invoke_pg_as_cpp([key] {
        static_cast<void>(DirectFunctionCall1(
            pg_advisory_lock_int8,
            Int64GetDatum(key)));
        static_cast<void>(DirectFunctionCall1(
            pg_advisory_unlock_int8,
            Int64GetDatum(key)));
    });
}

}  // namespace catalog_entity_test_support
#endif

/** Select whether an absent catalog entity returns an empty result or ERRORs. */
enum class catalog_missing_policy : std::uint8_t {
    error,
    return_invalid,
};

/** Select a fixed SQL schema or the installation schema of an extension. */
enum class sql_schema_binding : std::uint8_t {
    fixed,
    extension,
};

/** Structural, lifetime-owning catalog identifier usable as a template value. */
template<std::size_t Extent>
struct fixed_catalog_name final {
    std::array<char, Extent> bytes{};

    consteval fixed_catalog_name(const char (&text)[Extent]) {
        for (std::size_t index = 0; index != Extent; ++index)
            bytes[index] = text[index];
    }

    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return {bytes.data(), Extent - 1};
    }

    [[nodiscard]] consteval bool valid() const noexcept {
        return Extent > 1 && bytes[Extent - 1] == '\0';
    }
};

static_assert(std::is_trivially_copyable_v<fixed_catalog_name<2>>);
static_assert(std::is_standard_layout_v<fixed_catalog_name<2>>);

/**
 * Stateless resolver for a pg_class entity with an exact name, schema, and
 * relation kind.
 */
template<char ExpectedRelkind,
         fixed_catalog_name Leaf,
         sql_schema_binding Binding,
         fixed_catalog_name Scope>
class relation_entity_handle final {
    static_assert(Leaf.valid());
    static_assert(Scope.valid());
    static_assert(ExpectedRelkind != '\0');
    static_assert(Leaf.view().size() < NAMEDATALEN);
    static_assert(Scope.view().size() < NAMEDATALEN);
    static_assert(
        Binding == sql_schema_binding::fixed
        || Binding == sql_schema_binding::extension);

    enum class relation_lookup_state : std::uint8_t {
        found,
        disappeared,
        wrong_namespace,
        wrong_relkind,
    };

    [[noreturn]] static void raise_lookup_error(
            int sqlstate,
            std::string_view message) {
        bioseq::postgres::support::raise_sql_error(
            sqlstate,
            {.message = std::string{message}});
    }

    [[nodiscard]] static Oid missing_oid(
            catalog_missing_policy policy,
            std::string_view message) {
        if (policy == catalog_missing_policy::return_invalid)
            return InvalidOid;
        raise_lookup_error(ERRCODE_UNDEFINED_OBJECT, message);
    }

    [[nodiscard]] static Oid namespace_oid(
            catalog_missing_policy policy) {
        Oid result = InvalidOid;
        if constexpr (Binding == sql_schema_binding::fixed) {
            const std::string schema{Scope.view()};
            result = bioseq::postgres::support::invoke_pg_as_cpp(
                [schema_name = schema.c_str()] {
                    return get_namespace_oid(schema_name, true);
                });
        } else {
            const std::string extension{Scope.view()};
            const Oid extension_oid =
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [extension_name = extension.c_str()] {
                        return get_extension_oid(extension_name, true);
                    });
            if (!OidIsValid(extension_oid)) {
                return missing_oid(
                    policy, "required extension is missing");
            }
            result = bioseq::postgres::support::invoke_pg_as_cpp(
                [extension_oid] {
                    return get_extension_schema(extension_oid);
                });
        }

        if (!OidIsValid(result))
            return missing_oid(policy, "required schema is missing");
        return result;
    }

    [[nodiscard]] static std::optional<std::string>
    copy_namespace_name(
            Oid namespace_id,
            catalog_missing_policy policy) {
        char* raw_name = bioseq::postgres::support::invoke_pg_as_cpp(
            [namespace_id] {
                return get_namespace_name(namespace_id);
            });
        if (raw_name == nullptr) {
            if (policy == catalog_missing_policy::error) {
                raise_lookup_error(
                    ERRCODE_UNDEFINED_SCHEMA,
                    "required schema disappeared");
            }
            return std::nullopt;
        }

        try {
            std::string copied{raw_name};
            bioseq::postgres::support::invoke_pg_as_cpp(
                [raw_name] { pfree(raw_name); });
            raw_name = nullptr;
            return copied;
        } catch (...) {
            const std::exception_ptr original =
                std::current_exception();
            try {
                if (raw_name != nullptr) {
                    bioseq::postgres::support::invoke_pg_as_cpp(
                        [raw_name] { pfree(raw_name); });
                }
            } catch (...) {
                std::terminate();
            }
            std::rethrow_exception(original);
        }
    }

public:
    constexpr relation_entity_handle() noexcept = default;

    [[nodiscard]] static constexpr std::string_view
    leaf_name() noexcept {
        return Leaf.view();
    }

    [[nodiscard]] static constexpr sql_schema_binding
    schema_binding() noexcept {
        return Binding;
    }

    [[nodiscard]] static constexpr std::string_view
    stable_schema_name() noexcept {
        if constexpr (Binding == sql_schema_binding::fixed)
            return Scope.view();
        return "$extension";
    }

    [[nodiscard]] static std::string stable_qualified_name() {
        std::string result{stable_schema_name()};
        result.push_back('.');
        result.append(leaf_name());
        return result;
    }

    [[nodiscard]] static std::optional<std::string>
    installed_schema_name(catalog_missing_policy policy) {
        const Oid namespace_id = namespace_oid(policy);
        if (!OidIsValid(namespace_id))
            return std::nullopt;
        return copy_namespace_name(namespace_id, policy);
    }

    [[nodiscard]] static std::optional<std::string>
    installed_qualified_name(catalog_missing_policy policy) {
        auto schema = installed_schema_name(policy);
        if (!schema)
            return std::nullopt;

        const std::string leaf{leaf_name()};
        char* raw_name = bioseq::postgres::support::invoke_pg_as_cpp(
            [schema_name = schema->c_str(),
             leaf_name = leaf.c_str()] {
                return quote_qualified_identifier(
                    schema_name, leaf_name);
            });
        try {
            std::string copied{raw_name};
            bioseq::postgres::support::invoke_pg_as_cpp(
                [raw_name] { pfree(raw_name); });
            raw_name = nullptr;
            return copied;
        } catch (...) {
            const std::exception_ptr original =
                std::current_exception();
            try {
                if (raw_name != nullptr) {
                    bioseq::postgres::support::invoke_pg_as_cpp(
                        [raw_name] { pfree(raw_name); });
                }
            } catch (...) {
                std::terminate();
            }
            std::rethrow_exception(original);
        }
    }

    [[nodiscard]] static Oid resolve_oid(
            catalog_missing_policy policy) {
        const Oid namespace_id = namespace_oid(policy);
        if (!OidIsValid(namespace_id))
            return InvalidOid;

        const std::string leaf{leaf_name()};
        const Oid relation_id =
            bioseq::postgres::support::invoke_pg_as_cpp(
                [namespace_id, leaf_name = leaf.c_str()] {
                    return get_relname_relid(
                        leaf_name, namespace_id);
                });
        if (!OidIsValid(relation_id))
            return missing_oid(policy, "required relation is missing");

#if defined(BIOSEQ_TEST_INDEXES)
        catalog_entity_test_support::pass_lookup_barrier();
#endif

        const relation_lookup_state state =
            bioseq::postgres::support::invoke_pg_as_cpp(
                [relation_id, namespace_id] {
                    HeapTuple tuple = SearchSysCache1(
                        RELOID, ObjectIdGetDatum(relation_id));
                    if (!HeapTupleIsValid(tuple))
                        return relation_lookup_state::disappeared;

                    const Form_pg_class relation =
                        reinterpret_cast<Form_pg_class>(
                            GETSTRUCT(tuple));
                    relation_lookup_state result =
                        relation_lookup_state::found;
                    if (relation->relnamespace != namespace_id) {
                        result =
                            relation_lookup_state::wrong_namespace;
                    } else if (std::strncmp(
                                   NameStr(relation->relname),
                                   Leaf.bytes.data(),
                                   NAMEDATALEN) != 0) {
                        result =
                            relation_lookup_state::disappeared;
                    } else if (relation->relkind != ExpectedRelkind) {
                        result = relation_lookup_state::wrong_relkind;
                    }
                    ReleaseSysCache(tuple);
                    return result;
                });

        if (state == relation_lookup_state::disappeared) {
            return missing_oid(
                policy, "required relation disappeared");
        }
        if (state != relation_lookup_state::found) {
            raise_lookup_error(
                ERRCODE_WRONG_OBJECT_TYPE,
                "catalog object has the wrong namespace or relation kind");
        }
        return relation_id;
    }
};

} // namespace bioseq::postgres::support
