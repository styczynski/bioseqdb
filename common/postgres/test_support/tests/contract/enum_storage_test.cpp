/** @file Tests that enum settings keep one storage cell and can retry failed registration. */
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <stdexcept>

import bioseq.postgres.support.enum_storage;

namespace {

enum class probe_setting : int {
    initial = -1,
    changed = 7,
};

struct registration_observer {
    int* cell = nullptr;
    std::size_t calls = 0;
};

struct observing_registrar {
    registration_observer* observer;

    void register_variable(int* cell) const {
        observer->cell = cell;
        ++observer->calls;
    }
};

struct throwing_registrar {
    std::size_t* calls;

    void register_variable(int*) const {
        ++*calls;
        throw std::runtime_error{"registration failed"};
    }
};

}  // namespace

TEST_CASE(
    "enum GUC storage exposes one stable cell to one completed registration",
    "[component:common][layer:contract][feature:postgres_harness]") {
    using bioseq::postgres::support::postgres_enum_registration_access;
    using storage_type =
        bioseq::postgres::support::postgres_enum_storage<probe_setting>;

    storage_type storage{probe_setting::initial};
    registration_observer first{};
    postgres_enum_registration_access::initialize(
        storage, observing_registrar{&first});

    REQUIRE(first.calls == 1);
    REQUIRE(first.cell != nullptr);
    REQUIRE(storage.get_value() == probe_setting::initial);

    int* const registered_cell = first.cell;
    *registered_cell = static_cast<int>(probe_setting::changed);
    REQUIRE(storage.get_value() == probe_setting::changed);
    REQUIRE(first.cell == registered_cell);

    *registered_cell = static_cast<int>(probe_setting::initial);
    REQUIRE(storage.get_value() == probe_setting::initial);
    REQUIRE(first.cell == registered_cell);

    registration_observer second{};
    postgres_enum_registration_access::initialize(
        storage, observing_registrar{&second});
    REQUIRE(second.calls == 0);
    REQUIRE(second.cell == nullptr);
}

TEST_CASE(
    "enum GUC storage records completion only after registration returns",
    "[component:common][layer:contract][feature:postgres_harness]") {
    using bioseq::postgres::support::postgres_enum_registration_access;
    using storage_type =
        bioseq::postgres::support::postgres_enum_storage<probe_setting>;

    storage_type storage{probe_setting::initial};
    std::size_t failed_calls = 0;
    REQUIRE_THROWS_AS(
        postgres_enum_registration_access::initialize(
            storage, throwing_registrar{&failed_calls}),
        std::runtime_error);
    REQUIRE(failed_calls == 1);

    registration_observer retry{};
    postgres_enum_registration_access::initialize(
        storage, observing_registrar{&retry});
    REQUIRE(retry.calls == 1);
    REQUIRE(retry.cell != nullptr);

    registration_observer after_completion{};
    postgres_enum_registration_access::initialize(
        storage, observing_registrar{&after_completion});
    REQUIRE(after_completion.calls == 0);
    REQUIRE(after_completion.cell == nullptr);
}
