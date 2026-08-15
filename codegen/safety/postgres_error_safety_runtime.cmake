include_guard(GLOBAL)

# Build the PostgreSQL-side runtime control helper as its own DSO.  The helper
# owns the SQL control functions and fmgr hook; it must never be linked into the
# Bioseq extension whose generated export closure is being attested.
function(bioseq_add_postgres_error_safety_runtime_control)
    cmake_parse_arguments(
        PARSE_ARGV 0
        ARG
        ""
        "TARGET;SOURCE;OUTPUT_NAME"
        "INCLUDE_DIRS;LINKS;DEPENDS"
    )
    foreach(required TARGET SOURCE)
        if(NOT ARG_${required})
            message(FATAL_ERROR
                "bioseq_add_postgres_error_safety_runtime_control "
                "requires ${required}")
        endif()
    endforeach()
    if(TARGET "${ARG_TARGET}")
        message(FATAL_ERROR
            "PostgreSQL runtime control target already exists: "
            "${ARG_TARGET}")
    endif()
    if(NOT EXISTS "${ARG_SOURCE}")
        message(FATAL_ERROR
            "PostgreSQL runtime control source is missing: "
            "${ARG_SOURCE}")
    endif()
    if(NOT ARG_OUTPUT_NAME)
        set(ARG_OUTPUT_NAME "bioseq_safety_runtime_control")
    endif()

    add_library("${ARG_TARGET}" MODULE EXCLUDE_FROM_ALL)
    target_sources("${ARG_TARGET}"
        PRIVATE
            FILE_SET runtime_control_module TYPE CXX_MODULES
            BASE_DIRS "${CMAKE_SOURCE_DIR}"
            FILES "${ARG_SOURCE}")
    set_target_properties("${ARG_TARGET}" PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS ON
        POSITION_INDEPENDENT_CODE ON
        PREFIX ""
        OUTPUT_NAME "${ARG_OUTPUT_NAME}")
    if(ARG_INCLUDE_DIRS)
        target_include_directories(
            "${ARG_TARGET}" PRIVATE ${ARG_INCLUDE_DIRS})
    endif()
    if(ARG_LINKS)
        target_link_libraries("${ARG_TARGET}" PRIVATE ${ARG_LINKS})
    endif()
    if(APPLE)
        target_link_options(
            "${ARG_TARGET}" PRIVATE
            "-Wl,-undefined,dynamic_lookup")
    endif()
    if(ARG_DEPENDS)
        add_dependencies("${ARG_TARGET}" ${ARG_DEPENDS})
    endif()

    set("${ARG_TARGET}_DSO"
        "$<TARGET_FILE:${ARG_TARGET}>"
        PARENT_SCOPE)
endfunction()

# Derive one profile's cases from its exact FINAL safety ledger, seal the plan
# into a generated bindings header, compile the standalone protocol harness,
# and expose a bounded self-test target.  Generated plans are profile-specific;
# callers must invoke this function once for production and once for
# test_indexes.
function(bioseq_add_postgres_error_safety_runtime_profile)
    set(_one_value_arguments
        TARGET
        PROFILE
        LEDGER
        RUNTIME_INVENTORY
        RUNTIME_INVENTORY_PRODUCER_EXECUTABLE
        SQL_MANIFEST
        SQL_FILE
        CONTROL_FILE
        CONTROL_DSO_TARGET
        RUNTIME_POLICY
        CASE_TEMPLATE
        OUTPUT_DIRECTORY)
    set(_multi_value_arguments
        HARNESS_MODULES
        HARNESS_SOURCES
        INCLUDE_DIRS
        LINKS
        DEPENDS)
    cmake_parse_arguments(
        PARSE_ARGV 0
        ARG
        ""
        "${_one_value_arguments}"
        "${_multi_value_arguments}"
    )
    foreach(required
            TARGET
            PROFILE
            LEDGER
            RUNTIME_INVENTORY
            RUNTIME_INVENTORY_PRODUCER_EXECUTABLE
            SQL_MANIFEST
            SQL_FILE
            CONTROL_FILE
            CONTROL_DSO_TARGET
            RUNTIME_POLICY
            CASE_TEMPLATE
            OUTPUT_DIRECTORY)
        if(NOT ARG_${required})
            message(FATAL_ERROR
                "bioseq_add_postgres_error_safety_runtime_profile "
                "requires ${required}")
        endif()
    endforeach()
    foreach(required HARNESS_MODULES HARNESS_SOURCES)
        if(NOT ARG_${required})
            message(FATAL_ERROR
                "bioseq_add_postgres_error_safety_runtime_profile "
                "requires ${required}")
        endif()
    endforeach()
    if(NOT ARG_PROFILE STREQUAL "production"
       AND NOT ARG_PROFILE STREQUAL "test_indexes")
        message(FATAL_ERROR
            "PostgreSQL runtime safety PROFILE must be production "
            "or test_indexes")
    endif()
    if(TARGET "${ARG_TARGET}")
        message(FATAL_ERROR
            "PostgreSQL runtime harness target already exists: "
            "${ARG_TARGET}")
    endif()
    if(NOT TARGET "${ARG_CONTROL_DSO_TARGET}")
        message(FATAL_ERROR
            "PostgreSQL runtime control target does not exist: "
            "${ARG_CONTROL_DSO_TARGET}")
    endif()
    foreach(source_input
            RUNTIME_POLICY
            CASE_TEMPLATE)
        if(NOT EXISTS "${ARG_${source_input}}")
            message(FATAL_ERROR
                "PostgreSQL runtime safety ${source_input} is missing: "
                "${ARG_${source_input}}")
        endif()
    endforeach()
    foreach(harness_source IN LISTS ARG_HARNESS_MODULES ARG_HARNESS_SOURCES)
        if(NOT EXISTS "${harness_source}")
            message(FATAL_ERROR
                "PostgreSQL runtime safety harness source is missing: "
                "${harness_source}")
        endif()
    endforeach()
    if(NOT Python3_EXECUTABLE)
        find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    endif()

    get_filename_component(
        _output_directory "${ARG_OUTPUT_DIRECTORY}" ABSOLUTE)
    set(_plan "${_output_directory}/case-plan.json")
    set(_header "${_output_directory}/bindings.h")
    set(_plan_stamp "${_output_directory}/plan.stamp")
    set(_self_test_stamp
        "${_output_directory}/harness-self-test.stamp")
    set(_runtime_evidence_script
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/run_postgres_error_safety_tests.py")
    if(NOT EXISTS "${_runtime_evidence_script}")
        message(FATAL_ERROR
            "PostgreSQL runtime safety producer is missing: "
            "${_runtime_evidence_script}")
    endif()

    add_custom_command(
        OUTPUT
            "${_plan}"
            "${_header}"
            "${_plan_stamp}"
        COMMAND
            "${CMAKE_COMMAND}" -E make_directory
            "${_output_directory}"
        COMMAND
            "${CMAKE_COMMAND}" -E env
            "PYTHONDONTWRITEBYTECODE=1"
            "${Python3_EXECUTABLE}"
            "${_runtime_evidence_script}"
            plan
            --profile "${ARG_PROFILE}"
            --ledger "${ARG_LEDGER}"
            --runtime-inventory
                "${ARG_RUNTIME_INVENTORY}"
            --runtime-inventory-producer-executable
                "${ARG_RUNTIME_INVENTORY_PRODUCER_EXECUTABLE}"
            --sql-manifest "${ARG_SQL_MANIFEST}"
            --sql-file "${ARG_SQL_FILE}"
            --control-file "${ARG_CONTROL_FILE}"
            --control-dso
                "$<TARGET_FILE:${ARG_CONTROL_DSO_TARGET}>"
            --runtime-policy "${ARG_RUNTIME_POLICY}"
            --template "${ARG_CASE_TEMPLATE}"
            --output-plan "${_plan}"
            --output-header "${_header}"
            --stamp "${_plan_stamp}"
        DEPENDS
            "${_runtime_evidence_script}"
            "${ARG_LEDGER}"
            "${ARG_RUNTIME_INVENTORY}"
            "${ARG_RUNTIME_INVENTORY_PRODUCER_EXECUTABLE}"
            "${ARG_SQL_MANIFEST}"
            "${ARG_SQL_FILE}"
            "${ARG_CONTROL_FILE}"
            "${ARG_RUNTIME_POLICY}"
            "${ARG_CASE_TEMPLATE}"
            "${ARG_CONTROL_DSO_TARGET}"
            ${ARG_DEPENDS}
        VERBATIM
        COMMENT
            "Sealing ${ARG_PROFILE} PostgreSQL ERROR runtime plan")
    add_custom_target(
        "${ARG_TARGET}_plan"
        DEPENDS "${_plan}" "${_header}" "${_plan_stamp}")

    add_executable("${ARG_TARGET}" EXCLUDE_FROM_ALL)
    target_sources(
        "${ARG_TARGET}"
        PRIVATE
            ${ARG_HARNESS_SOURCES}
        PRIVATE FILE_SET CXX_MODULES
        BASE_DIRS "${CMAKE_SOURCE_DIR}"
        FILES
            ${ARG_HARNESS_MODULES})
    set_source_files_properties(
        "${_header}" PROPERTIES GENERATED TRUE)
    set_source_files_properties(
        ${ARG_HARNESS_MODULES}
        ${ARG_HARNESS_SOURCES}
        PROPERTIES OBJECT_DEPENDS "${_header}")
    target_sources("${ARG_TARGET}" PRIVATE "${_header}")
    set_target_properties("${ARG_TARGET}" PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS ON)
    target_compile_definitions(
        "${ARG_TARGET}" PRIVATE
        "BIOSEQ_SAFETY_RUNTIME_BINDINGS_HEADER=\"${_header}\"")
    if(ARG_INCLUDE_DIRS)
        target_include_directories(
            "${ARG_TARGET}" PRIVATE ${ARG_INCLUDE_DIRS})
    endif()
    if(ARG_LINKS)
        target_link_libraries("${ARG_TARGET}" PRIVATE ${ARG_LINKS})
    endif()
    add_dependencies("${ARG_TARGET}" "${ARG_TARGET}_plan")

    add_custom_command(
        OUTPUT "${_self_test_stamp}"
        COMMAND
            "${CMAKE_COMMAND}" -E make_directory
            "${_output_directory}"
        COMMAND
            "${CMAKE_COMMAND}" -E env
            "BIOSEQ_SAFETY_RUNTIME_PROTOCOL=2"
            "$<TARGET_FILE:${ARG_TARGET}>"
            --bioseq-safety-runtime-self-test
        COMMAND
            "${CMAKE_COMMAND}" -E touch
            "${_self_test_stamp}"
        DEPENDS
            "${ARG_TARGET}"
            "${_plan}"
            "${_header}"
            "${_plan_stamp}"
            "${ARG_CONTROL_DSO_TARGET}"
        VERBATIM
        COMMENT
            "Checking ${ARG_PROFILE} PostgreSQL ERROR runtime harness")
    add_custom_target(
        "${ARG_TARGET}_self_test"
        DEPENDS "${_self_test_stamp}")

    set("${ARG_TARGET}_CASE_PLAN" "${_plan}" PARENT_SCOPE)
    set("${ARG_TARGET}_BINDINGS_HEADER" "${_header}" PARENT_SCOPE)
    set("${ARG_TARGET}_PLAN_STAMP" "${_plan_stamp}" PARENT_SCOPE)
    set("${ARG_TARGET}_SELF_TEST_STAMP"
        "${_self_test_stamp}"
        PARENT_SCOPE)
endfunction()

# Adds an explicit, non-ALL target that runs the runtime safety
# evidence matrix.  The caller supplies two separately built profile DSOs,
# profile runtime-test executables, and either two isolated cluster
# configurations or two sequential activation configurations for one cluster.
#
# The profile test executables own PostgreSQL installation, fault injection,
# invocation, and recovery probes through the fixed PostgreSQL safety runner
# activate/describe/run protocol.  This helper never treats a same-tree DSO as
# both profiles and never manufactures runtime rows from static taxonomy.
function(bioseq_add_postgres_error_safety_runtime_evidence)
    cmake_parse_arguments(
        PARSE_ARGV 0
        ARG
        ""
        "TARGET;CLUSTER_MODE;PRODUCTION_LEDGER;TEST_INDEXES_LEDGER;PRODUCTION_RUNTIME_INVENTORY;TEST_INDEXES_RUNTIME_INVENTORY;RUNTIME_INVENTORY_PRODUCER_EXECUTABLE;PRODUCTION_SQL_MANIFEST;TEST_INDEXES_SQL_MANIFEST;PRODUCTION_CASE_PLAN;TEST_INDEXES_CASE_PLAN;RUNTIME_POLICY;PRODUCTION_DSO;TEST_INDEXES_DSO;RUNTIME_CONTROL_DSO;PRODUCTION_STAGE_MANIFEST;TEST_INDEXES_STAGE_MANIFEST;PRODUCTION_TEST_EXECUTABLE;TEST_INDEXES_TEST_EXECUTABLE;PRODUCTION_CLUSTER_CONFIG;TEST_INDEXES_CLUSTER_CONFIG;OUTPUT_DIRECTORY;OUTPUT_MATRIX;OUTPUT_STAMP;TIMEOUT_SECONDS"
        "DEPENDS"
    )
    foreach(required
            TARGET
            CLUSTER_MODE
            PRODUCTION_LEDGER
            TEST_INDEXES_LEDGER
            PRODUCTION_RUNTIME_INVENTORY
            TEST_INDEXES_RUNTIME_INVENTORY
            RUNTIME_INVENTORY_PRODUCER_EXECUTABLE
            PRODUCTION_SQL_MANIFEST
            TEST_INDEXES_SQL_MANIFEST
            PRODUCTION_CASE_PLAN
            TEST_INDEXES_CASE_PLAN
            RUNTIME_POLICY
            PRODUCTION_DSO
            TEST_INDEXES_DSO
            RUNTIME_CONTROL_DSO
            PRODUCTION_STAGE_MANIFEST
            TEST_INDEXES_STAGE_MANIFEST
            PRODUCTION_TEST_EXECUTABLE
            TEST_INDEXES_TEST_EXECUTABLE
            PRODUCTION_CLUSTER_CONFIG
            TEST_INDEXES_CLUSTER_CONFIG
            OUTPUT_DIRECTORY
            OUTPUT_MATRIX
            OUTPUT_STAMP)
        if(NOT ARG_${required})
            message(FATAL_ERROR
                "bioseq_add_postgres_error_safety_runtime_evidence "
                "requires ${required}")
        endif()
    endforeach()
    if(TARGET "${ARG_TARGET}")
        message(FATAL_ERROR
            "PostgreSQL runtime safety target already exists: "
            "${ARG_TARGET}")
    endif()
    if(NOT ARG_CLUSTER_MODE STREQUAL "isolated"
       AND NOT ARG_CLUSTER_MODE STREQUAL "sequential")
        message(FATAL_ERROR
            "PostgreSQL runtime safety CLUSTER_MODE must be isolated "
            "or sequential")
    endif()
    if("${ARG_PRODUCTION_DSO}" STREQUAL "${ARG_TEST_INDEXES_DSO}"
       OR "${ARG_PRODUCTION_TEST_EXECUTABLE}"
            STREQUAL "${ARG_TEST_INDEXES_TEST_EXECUTABLE}")
        message(FATAL_ERROR
            "PostgreSQL runtime safety requires distinct production "
            "and test-indexes DSO/test artifact paths")
    endif()
    if("${ARG_PRODUCTION_CASE_PLAN}"
       STREQUAL "${ARG_TEST_INDEXES_CASE_PLAN}")
        message(FATAL_ERROR
            "PostgreSQL runtime safety requires distinct production "
            "and test-indexes case plans")
    endif()
    if("${ARG_OUTPUT_MATRIX}" STREQUAL "${ARG_OUTPUT_STAMP}")
        message(FATAL_ERROR
            "PostgreSQL runtime safety matrix and stamp outputs "
            "must be distinct")
    endif()
    if(NOT ARG_TIMEOUT_SECONDS)
        set(ARG_TIMEOUT_SECONDS 600)
    endif()
    if(NOT ARG_TIMEOUT_SECONDS MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR
            "PostgreSQL runtime safety TIMEOUT_SECONDS must be a "
            "positive integer")
    endif()
    if(NOT Python3_EXECUTABLE)
        find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    endif()

    set(_runtime_evidence_script
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/run_postgres_error_safety_tests.py")
    if(NOT EXISTS "${_runtime_evidence_script}")
        message(FATAL_ERROR
            "PostgreSQL runtime safety producer is missing: "
            "${_runtime_evidence_script}")
    endif()
    get_filename_component(
        _matrix_directory
        "${ARG_OUTPUT_MATRIX}"
        DIRECTORY)
    get_filename_component(
        _stamp_directory
        "${ARG_OUTPUT_STAMP}"
        DIRECTORY)

    add_custom_command(
        OUTPUT
            "${ARG_OUTPUT_MATRIX}"
            "${ARG_OUTPUT_STAMP}"
        COMMAND
            "${CMAKE_COMMAND}" -E make_directory
            "${ARG_OUTPUT_DIRECTORY}"
            "${_matrix_directory}"
            "${_stamp_directory}"
        COMMAND
            "${CMAKE_COMMAND}" -E env
            "PYTHONDONTWRITEBYTECODE=1"
            "${Python3_EXECUTABLE}"
            "${_runtime_evidence_script}"
            self-test
        COMMAND
            "${CMAKE_COMMAND}" -E env
            "PYTHONDONTWRITEBYTECODE=1"
            "${Python3_EXECUTABLE}"
            "${_runtime_evidence_script}"
            run
            --production-ledger
                "${ARG_PRODUCTION_LEDGER}"
            --test-indexes-ledger
                "${ARG_TEST_INDEXES_LEDGER}"
            --production-runtime-inventory
                "${ARG_PRODUCTION_RUNTIME_INVENTORY}"
            --test-indexes-runtime-inventory
                "${ARG_TEST_INDEXES_RUNTIME_INVENTORY}"
            --runtime-inventory-producer-executable
                "${ARG_RUNTIME_INVENTORY_PRODUCER_EXECUTABLE}"
            --production-sql-manifest
                "${ARG_PRODUCTION_SQL_MANIFEST}"
            --test-indexes-sql-manifest
                "${ARG_TEST_INDEXES_SQL_MANIFEST}"
            --production-case-plan
                "${ARG_PRODUCTION_CASE_PLAN}"
            --test-indexes-case-plan
                "${ARG_TEST_INDEXES_CASE_PLAN}"
            --runtime-policy
                "${ARG_RUNTIME_POLICY}"
            --production-dso
                "${ARG_PRODUCTION_DSO}"
            --test-indexes-dso
                "${ARG_TEST_INDEXES_DSO}"
            --runtime-control-dso
                "${ARG_RUNTIME_CONTROL_DSO}"
            --production-stage-manifest
                "${ARG_PRODUCTION_STAGE_MANIFEST}"
            --test-indexes-stage-manifest
                "${ARG_TEST_INDEXES_STAGE_MANIFEST}"
            --production-test-executable
                "${ARG_PRODUCTION_TEST_EXECUTABLE}"
            --test-indexes-test-executable
                "${ARG_TEST_INDEXES_TEST_EXECUTABLE}"
            --production-cluster-config
                "${ARG_PRODUCTION_CLUSTER_CONFIG}"
            --test-indexes-cluster-config
                "${ARG_TEST_INDEXES_CLUSTER_CONFIG}"
            --cluster-mode
                "${ARG_CLUSTER_MODE}"
            --output-directory
                "${ARG_OUTPUT_DIRECTORY}"
            --output-matrix
                "${ARG_OUTPUT_MATRIX}"
            --stamp
                "${ARG_OUTPUT_STAMP}"
            --timeout-seconds
                "${ARG_TIMEOUT_SECONDS}"
        DEPENDS
            "${_runtime_evidence_script}"
            "${ARG_PRODUCTION_LEDGER}"
            "${ARG_TEST_INDEXES_LEDGER}"
            "${ARG_PRODUCTION_RUNTIME_INVENTORY}"
            "${ARG_TEST_INDEXES_RUNTIME_INVENTORY}"
            "${ARG_RUNTIME_INVENTORY_PRODUCER_EXECUTABLE}"
            "${ARG_PRODUCTION_SQL_MANIFEST}"
            "${ARG_TEST_INDEXES_SQL_MANIFEST}"
            "${ARG_PRODUCTION_CASE_PLAN}"
            "${ARG_TEST_INDEXES_CASE_PLAN}"
            "${ARG_RUNTIME_POLICY}"
            "${ARG_PRODUCTION_DSO}"
            "${ARG_TEST_INDEXES_DSO}"
            "${ARG_RUNTIME_CONTROL_DSO}"
            "${ARG_PRODUCTION_STAGE_MANIFEST}"
            "${ARG_TEST_INDEXES_STAGE_MANIFEST}"
            "${ARG_PRODUCTION_TEST_EXECUTABLE}"
            "${ARG_TEST_INDEXES_TEST_EXECUTABLE}"
            "${ARG_PRODUCTION_CLUSTER_CONFIG}"
            "${ARG_TEST_INDEXES_CLUSTER_CONFIG}"
            ${ARG_DEPENDS}
        VERBATIM
        USES_TERMINAL
        COMMENT
            "Running dual-profile PostgreSQL ERROR runtime evidence")
    add_custom_target(
        "${ARG_TARGET}"
        DEPENDS
            "${ARG_OUTPUT_MATRIX}"
            "${ARG_OUTPUT_STAMP}")

    set("${ARG_TARGET}_MATRIX"
        "${ARG_OUTPUT_MATRIX}"
        PARENT_SCOPE)
    set("${ARG_TARGET}_STAMP"
        "${ARG_OUTPUT_STAMP}"
        PARENT_SCOPE)
endfunction()
