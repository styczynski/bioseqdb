include_guard(GLOBAL)

function(_bioseq_require_postgres_error_safety_codegen)
    if(NOT TARGET bioseq_codegen_project)
        message(FATAL_ERROR
            "PostgreSQL safety requires the project-owned "
            "bioseq_codegen_project target")
    endif()
    if(NOT DEFINED BIOSEQ_CODEGEN_EXECUTABLE
       OR "${BIOSEQ_CODEGEN_EXECUTABLE}" STREQUAL "")
        message(FATAL_ERROR
            "PostgreSQL safety requires BIOSEQ_CODEGEN_EXECUTABLE "
            "from bioseq_codegen.cmake")
    endif()
    foreach(required_tool
            BIOSEQ_CODEGEN_CLANG_BINARY
            BIOSEQ_CODEGEN_CLANG_SHA256
            BIOSEQ_CODEGEN_CLANG_SCAN_DEPS
            BIOSEQ_CODEGEN_CLANG_SCAN_DEPS_SHA256
            BIOSEQ_CODEGEN_LLVM_CONFIG
            BIOSEQ_CODEGEN_LLVM_CONFIG_SHA256
            BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON
            BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_SHA256)
        if(NOT DEFINED ${required_tool}
           OR "${${required_tool}}" STREQUAL "")
            message(FATAL_ERROR
                "PostgreSQL safety requires ${required_tool} from "
                "the selected Clang toolchain")
        endif()
    endforeach()
endfunction()

function(bioseq_add_postgres_error_safety_facts_profile)
    cmake_parse_arguments(
        PARSE_ARGV 0
        ARG
        ""
        "TARGET;PROFILE;FINAL_TARGET;SOURCE_LIST;RAW_COMPILE_DATABASE;SQL_MANIFEST;PROVENANCE;OUTPUT_COMPILE_DATABASE;OUTPUT_PCM_ROOT;OUTPUT_PCM_MANIFEST;OUTPUT_PREPARE_DEPFILE;OUTPUT_PREPARE_STAMP;OUTPUT_COMPILE_GRAPH;OUTPUT_FACTS;OUTPUT_FACTS_STAMP;OUTPUT_PROVENANCE_CANDIDATES"
        "SOURCES;DEPENDS"
    )
    foreach(required
            TARGET
            PROFILE
            FINAL_TARGET
            SOURCE_LIST
            RAW_COMPILE_DATABASE
            SQL_MANIFEST
            PROVENANCE
            OUTPUT_COMPILE_DATABASE
            OUTPUT_PCM_ROOT
            OUTPUT_PCM_MANIFEST
            OUTPUT_PREPARE_DEPFILE
            OUTPUT_PREPARE_STAMP
            OUTPUT_COMPILE_GRAPH
            OUTPUT_FACTS
            OUTPUT_FACTS_STAMP)
        if(NOT ARG_${required})
            message(FATAL_ERROR
                "bioseq_add_postgres_error_safety_facts_profile "
                "requires ${required}")
        endif()
    endforeach()
    if(NOT ARG_PROFILE STREQUAL "production"
       AND NOT ARG_PROFILE STREQUAL "test_indexes")
        message(FATAL_ERROR
            "PostgreSQL safety facts PROFILE must be production "
            "or test_indexes")
    endif()
    set(_expected_final_target
        "bioseq_sql_final_${ARG_PROFILE}")
    if(NOT "${ARG_FINAL_TARGET}"
           STREQUAL "${_expected_final_target}")
        message(FATAL_ERROR
            "PostgreSQL safety facts must follow "
            "${_expected_final_target}; depending on a DSO target "
            "would create a safety/link cycle")
    endif()
    if(NOT TARGET "${ARG_FINAL_TARGET}")
        message(FATAL_ERROR
            "PostgreSQL safety FINAL_TARGET does not exist: "
            "${ARG_FINAL_TARGET}")
    endif()
    if(TARGET "${ARG_TARGET}")
        message(FATAL_ERROR
            "PostgreSQL safety facts target already exists: "
            "${ARG_TARGET}")
    endif()
    if("${ARG_OUTPUT_COMPILE_DATABASE}"
            STREQUAL "${ARG_RAW_COMPILE_DATABASE}"
       OR "${ARG_OUTPUT_PCM_MANIFEST}"
            STREQUAL "${ARG_SQL_MANIFEST}")
        message(FATAL_ERROR
            "PostgreSQL safety must use distinct owned compile "
            "database and PCM-manifest outputs")
    endif()
    string(TOUPPER "${ARG_PROFILE}" _profile_key)
    set(_final_pcm_root_variable
        "BIOSEQ_SQL_${_profile_key}_PCM_ROOT")
    set(_final_compile_database_variable
        "BIOSEQ_SQL_${_profile_key}_FINAL_COMPILE_DATABASE")
    set(_final_pcm_manifest_variable
        "BIOSEQ_SQL_${_profile_key}_FINAL_PCM_MANIFEST")
    if((DEFINED ${_final_pcm_root_variable}
        AND "${ARG_OUTPUT_PCM_ROOT}"
            STREQUAL "${${_final_pcm_root_variable}}")
       OR (DEFINED ${_final_compile_database_variable}
        AND "${ARG_OUTPUT_COMPILE_DATABASE}"
            STREQUAL
                "${${_final_compile_database_variable}}")
       OR (DEFINED ${_final_pcm_manifest_variable}
        AND "${ARG_OUTPUT_PCM_MANIFEST}"
            STREQUAL "${${_final_pcm_manifest_variable}}"))
        message(FATAL_ERROR
            "PostgreSQL safety requires a post-FINAL compile "
            "database, PCM root, and PCM manifest distinct from "
            "SQL FINAL")
    endif()
    _bioseq_require_postgres_error_safety_codegen()
    if(NOT ARG_OUTPUT_PROVENANCE_CANDIDATES)
        set(ARG_OUTPUT_PROVENANCE_CANDIDATES
            "${ARG_OUTPUT_FACTS}.provenance-candidates.json")
    endif()

    get_filename_component(
        _compile_database_directory
        "${ARG_OUTPUT_COMPILE_DATABASE}"
        DIRECTORY)
    get_filename_component(
        _pcm_manifest_directory
        "${ARG_OUTPUT_PCM_MANIFEST}"
        DIRECTORY)
    get_filename_component(
        _facts_directory
        "${ARG_OUTPUT_FACTS}"
        DIRECTORY)
    get_filename_component(
        _compile_graph_directory
        "${ARG_OUTPUT_COMPILE_GRAPH}"
        DIRECTORY)

    add_custom_command(
        OUTPUT "${ARG_OUTPUT_PREPARE_STAMP}"
        BYPRODUCTS
            "${ARG_OUTPUT_COMPILE_DATABASE}"
            "${ARG_OUTPUT_PCM_MANIFEST}"
        COMMAND
            "${CMAKE_COMMAND}" -E make_directory
            "${ARG_OUTPUT_PCM_ROOT}"
            "${_compile_database_directory}"
            "${_pcm_manifest_directory}"
        COMMAND
            "${BIOSEQ_CODEGEN_EXECUTABLE}"
            sql
            prepare
            --authority safety
            --profile "${ARG_PROFILE}"
            --source-list "${ARG_SOURCE_LIST}"
            --raw-compile-database
                "${ARG_RAW_COMPILE_DATABASE}"
            --clang-binary "${BIOSEQ_CODEGEN_CLANG_BINARY}"
            --clang-sha256
                "${BIOSEQ_CODEGEN_CLANG_SHA256}"
            --clang-scan-deps
                "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
            --clang-scan-deps-sha256
                "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS_SHA256}"
            --llvm-config "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
            --llvm-config-sha256
                "${BIOSEQ_CODEGEN_LLVM_CONFIG_SHA256}"
            --compiler-module-inventory
                "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
            --compiler-module-inventory-sha256
                "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_SHA256}"
            --compile-database-output
                "${ARG_OUTPUT_COMPILE_DATABASE}"
            --pcm-root "${ARG_OUTPUT_PCM_ROOT}"
            --pcm-manifest "${ARG_OUTPUT_PCM_MANIFEST}"
            --depfile "${ARG_OUTPUT_PREPARE_DEPFILE}"
            --stamp "${ARG_OUTPUT_PREPARE_STAMP}"
        DEPENDS
            bioseq_codegen_project
            "${BIOSEQ_CODEGEN_EXECUTABLE}"
            "${ARG_FINAL_TARGET}"
            "${ARG_SOURCE_LIST}"
            "${ARG_RAW_COMPILE_DATABASE}"
            "${ARG_SQL_MANIFEST}"
            "${BIOSEQ_CODEGEN_CLANG_BINARY}"
            "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
            "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
            "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
            ${ARG_SOURCES}
            ${ARG_DEPENDS}
        DEPFILE "${ARG_OUTPUT_PREPARE_DEPFILE}"
        JOB_POOL sql_codegen_ast_proof
        VERBATIM
        COMMENT "Preparing post-FINAL ${ARG_PROFILE} PostgreSQL safety Clang closure")

    add_custom_command(
        OUTPUT
            "${ARG_OUTPUT_COMPILE_GRAPH}"
            "${ARG_OUTPUT_FACTS}"
            "${ARG_OUTPUT_FACTS_STAMP}"
        BYPRODUCTS
            "${ARG_OUTPUT_PROVENANCE_CANDIDATES}"
        COMMAND
            "${CMAKE_COMMAND}" -E make_directory
            "${_facts_directory}"
            "${_compile_graph_directory}"
        COMMAND
            "${BIOSEQ_CODEGEN_EXECUTABLE}"
            safety
            emit-facts
            --schema-version 2
            --profile "${ARG_PROFILE}"
            --source-list "${ARG_SOURCE_LIST}"
            --compile-database
                "${ARG_OUTPUT_COMPILE_DATABASE}"
            --pcm-root "${ARG_OUTPUT_PCM_ROOT}"
            --pcm-manifest "${ARG_OUTPUT_PCM_MANIFEST}"
            --sql-manifest "${ARG_SQL_MANIFEST}"
            --provenance "${ARG_PROVENANCE}"
            --provenance-candidates-output
                "${ARG_OUTPUT_PROVENANCE_CANDIDATES}"
            --producer-executable
                "${BIOSEQ_CODEGEN_EXECUTABLE}"
            --compile-graph-output
                "${ARG_OUTPUT_COMPILE_GRAPH}"
            --output "${ARG_OUTPUT_FACTS}"
            --stamp "${ARG_OUTPUT_FACTS_STAMP}"
        DEPENDS
            bioseq_codegen_project
            "${BIOSEQ_CODEGEN_EXECUTABLE}"
            "${ARG_OUTPUT_PREPARE_STAMP}"
            "${ARG_OUTPUT_COMPILE_DATABASE}"
            "${ARG_OUTPUT_PCM_MANIFEST}"
            "${ARG_SOURCE_LIST}"
            "${ARG_SQL_MANIFEST}"
            "${ARG_PROVENANCE}"
            ${ARG_SOURCES}
        JOB_POOL sql_codegen_ast_proof
        VERBATIM
        COMMENT "Emitting current ${ARG_PROFILE} PostgreSQL safety facts")
    add_custom_target(
        "${ARG_TARGET}"
        DEPENDS
            "${ARG_OUTPUT_COMPILE_GRAPH}"
            "${ARG_OUTPUT_FACTS}"
            "${ARG_OUTPUT_FACTS_STAMP}")

    set("${ARG_TARGET}_COMPILE_DATABASE"
        "${ARG_OUTPUT_COMPILE_DATABASE}"
        PARENT_SCOPE)
    set("${ARG_TARGET}_PCM_MANIFEST"
        "${ARG_OUTPUT_PCM_MANIFEST}"
        PARENT_SCOPE)
    set("${ARG_TARGET}_FACTS"
        "${ARG_OUTPUT_FACTS}"
        PARENT_SCOPE)
    set("${ARG_TARGET}_COMPILE_GRAPH"
        "${ARG_OUTPUT_COMPILE_GRAPH}"
        PARENT_SCOPE)
    set("${ARG_TARGET}_PROVENANCE_CANDIDATES"
        "${ARG_OUTPUT_PROVENANCE_CANDIDATES}"
        PARENT_SCOPE)
    set("${ARG_TARGET}_STAMP"
        "${ARG_OUTPUT_FACTS_STAMP}"
        PARENT_SCOPE)
endfunction()

function(bioseq_add_postgres_error_safety_profile)
    cmake_parse_arguments(
        PARSE_ARGV 0
        ARG
        ""
        "TARGET;PROFILE;SQL_MANIFEST;COMPILER_FACTS;COMPILE_GRAPH;PROVENANCE;GENERATOR_REVISION;OUTPUT_LEDGER;OUTPUT_STAMP"
        "DEPENDS"
    )

    foreach(required
            TARGET
            PROFILE
            SQL_MANIFEST
            COMPILER_FACTS
            COMPILE_GRAPH
            PROVENANCE
            GENERATOR_REVISION
            OUTPUT_LEDGER
            OUTPUT_STAMP)
        if(NOT ARG_${required})
            message(FATAL_ERROR
                "bioseq_add_postgres_error_safety_profile requires ${required}")
        endif()
    endforeach()
    if(NOT ARG_PROFILE STREQUAL "production"
       AND NOT ARG_PROFILE STREQUAL "test_indexes")
        message(FATAL_ERROR
            "PostgreSQL safety PROFILE must be production or test_indexes")
    endif()
    if(TARGET "${ARG_TARGET}")
        message(FATAL_ERROR
            "PostgreSQL safety target already exists: ${ARG_TARGET}")
    endif()
    _bioseq_require_postgres_error_safety_codegen()

    add_custom_command(
        OUTPUT
            "${ARG_OUTPUT_LEDGER}"
            "${ARG_OUTPUT_STAMP}"
        COMMAND
            "${BIOSEQ_CODEGEN_EXECUTABLE}"
            safety
            analyze-facts
            --sql-manifest "${ARG_SQL_MANIFEST}"
            --compiler-facts "${ARG_COMPILER_FACTS}"
            --compile-graph "${ARG_COMPILE_GRAPH}"
            --provenance "${ARG_PROVENANCE}"
            --producer-executable
                "${BIOSEQ_CODEGEN_EXECUTABLE}"
            --output "${ARG_OUTPUT_LEDGER}"
            --stamp "${ARG_OUTPUT_STAMP}"
            --profile "${ARG_PROFILE}"
            --generator-revision "${ARG_GENERATOR_REVISION}"
        DEPENDS
            bioseq_codegen_project
            "${BIOSEQ_CODEGEN_EXECUTABLE}"
            "${ARG_SQL_MANIFEST}"
            "${ARG_COMPILER_FACTS}"
            "${ARG_COMPILE_GRAPH}"
            "${ARG_PROVENANCE}"
            ${ARG_DEPENDS}
        VERBATIM
        COMMENT
            "Analyzing PostgreSQL ERROR safety for ${ARG_PROFILE}"
    )
    add_custom_target(
        "${ARG_TARGET}"
        DEPENDS
            "${ARG_OUTPUT_LEDGER}"
            "${ARG_OUTPUT_STAMP}"
    )

    set("${ARG_TARGET}_LEDGER"
        "${ARG_OUTPUT_LEDGER}"
        PARENT_SCOPE)
    set("${ARG_TARGET}_STAMP"
        "${ARG_OUTPUT_STAMP}"
        PARENT_SCOPE)
endfunction()

function(bioseq_add_postgres_error_safety_inventory)
    cmake_parse_arguments(
        PARSE_ARGV 0
        ARG
        ""
        "TARGET;PROFILE;LEDGER;COMPILER_FACTS;COMPILE_GRAPH;SQL_MANIFEST;PRODUCER_EXECUTABLE;OUTPUT_INVENTORY;OUTPUT_STAMP"
        "DEPENDS"
    )
    foreach(required
            TARGET
            PROFILE
            LEDGER
            COMPILER_FACTS
            COMPILE_GRAPH
            SQL_MANIFEST
            PRODUCER_EXECUTABLE
            OUTPUT_INVENTORY
            OUTPUT_STAMP)
        if(NOT ARG_${required})
            message(FATAL_ERROR
                "bioseq_add_postgres_error_safety_inventory "
                "requires ${required}")
        endif()
    endforeach()
    if(NOT ARG_PROFILE STREQUAL "production"
       AND NOT ARG_PROFILE STREQUAL "test_indexes")
        message(FATAL_ERROR
            "PostgreSQL safety inventory PROFILE must be "
            "production or test_indexes")
    endif()
    if(TARGET "${ARG_TARGET}")
        message(FATAL_ERROR
            "PostgreSQL safety inventory target already exists: "
            "${ARG_TARGET}")
    endif()
    if("${ARG_OUTPUT_INVENTORY}" STREQUAL "${ARG_OUTPUT_STAMP}")
        message(FATAL_ERROR
            "PostgreSQL safety inventory and stamp outputs must "
            "be distinct")
    endif()
    _bioseq_require_postgres_error_safety_codegen()
    get_filename_component(
        _inventory_directory
        "${ARG_OUTPUT_INVENTORY}"
        DIRECTORY)
    get_filename_component(
        _stamp_directory
        "${ARG_OUTPUT_STAMP}"
        DIRECTORY)

    add_custom_command(
        OUTPUT
            "${ARG_OUTPUT_INVENTORY}"
            "${ARG_OUTPUT_STAMP}"
        COMMAND
            "${CMAKE_COMMAND}" -E make_directory
            "${_inventory_directory}"
            "${_stamp_directory}"
        COMMAND
            "${BIOSEQ_CODEGEN_EXECUTABLE}"
            safety
            emit-inventory
            --profile "${ARG_PROFILE}"
            --ledger "${ARG_LEDGER}"
            --compiler-facts "${ARG_COMPILER_FACTS}"
            --compile-graph "${ARG_COMPILE_GRAPH}"
            --sql-manifest "${ARG_SQL_MANIFEST}"
            --producer-executable
                "${ARG_PRODUCER_EXECUTABLE}"
            --output "${ARG_OUTPUT_INVENTORY}"
            --stamp "${ARG_OUTPUT_STAMP}"
        DEPENDS
            bioseq_codegen_project
            "${BIOSEQ_CODEGEN_EXECUTABLE}"
            "${ARG_LEDGER}"
            "${ARG_COMPILER_FACTS}"
            "${ARG_COMPILE_GRAPH}"
            "${ARG_SQL_MANIFEST}"
            "${ARG_PRODUCER_EXECUTABLE}"
            ${ARG_DEPENDS}
        VERBATIM
        COMMENT
            "Emitting ${ARG_PROFILE} PostgreSQL runtime probe inventory")
    add_custom_target(
        "${ARG_TARGET}"
        DEPENDS
            "${ARG_OUTPUT_INVENTORY}"
            "${ARG_OUTPUT_STAMP}")
    set("${ARG_TARGET}_INVENTORY"
        "${ARG_OUTPUT_INVENTORY}"
        PARENT_SCOPE)
    set("${ARG_TARGET}_STAMP"
        "${ARG_OUTPUT_STAMP}"
        PARENT_SCOPE)
endfunction()

function(bioseq_add_postgres_error_safety_pair)
    cmake_parse_arguments(
        PARSE_ARGV 0
        ARG
        ""
        "TARGET;PRODUCTION_TARGET;TEST_INDEXES_TARGET;PRODUCTION_LEDGER;TEST_INDEXES_LEDGER;PROVENANCE;OUTPUT;OUTPUT_STAMP"
        ""
    )
    foreach(required TARGET PRODUCTION_TARGET TEST_INDEXES_TARGET)
        if(NOT ARG_${required})
            message(FATAL_ERROR
                "bioseq_add_postgres_error_safety_pair requires ${required}")
        endif()
    endforeach()
    if(TARGET "${ARG_TARGET}")
        message(FATAL_ERROR
            "PostgreSQL safety pair target already exists: ${ARG_TARGET}")
    endif()
    if(NOT TARGET "${ARG_PRODUCTION_TARGET}"
       OR NOT TARGET "${ARG_TEST_INDEXES_TARGET}")
        message(FATAL_ERROR
            "PostgreSQL safety pair requires both profile targets")
    endif()
    if(NOT ARG_PRODUCTION_LEDGER)
        set(_production_ledger_variable
            "${ARG_PRODUCTION_TARGET}_LEDGER")
        if(DEFINED ${_production_ledger_variable})
            set(ARG_PRODUCTION_LEDGER
                "${${_production_ledger_variable}}")
        endif()
    endif()
    if(NOT ARG_TEST_INDEXES_LEDGER)
        set(_test_indexes_ledger_variable
            "${ARG_TEST_INDEXES_TARGET}_LEDGER")
        if(DEFINED ${_test_indexes_ledger_variable})
            set(ARG_TEST_INDEXES_LEDGER
                "${${_test_indexes_ledger_variable}}")
        endif()
    endif()
    if(NOT ARG_PROVENANCE
       AND DEFINED BIOSEQ_SQL_SAFETY_PROVENANCE)
        set(ARG_PROVENANCE
            "${BIOSEQ_SQL_SAFETY_PROVENANCE}")
    endif()
    if(NOT ARG_OUTPUT)
        set(ARG_OUTPUT
            "${CMAKE_CURRENT_BINARY_DIR}/${ARG_TARGET}.json")
    endif()
    if(NOT ARG_OUTPUT_STAMP)
        set(ARG_OUTPUT_STAMP
            "${CMAKE_CURRENT_BINARY_DIR}/${ARG_TARGET}.stamp")
    endif()
    foreach(required
            PRODUCTION_LEDGER
            TEST_INDEXES_LEDGER
            PROVENANCE
            OUTPUT
            OUTPUT_STAMP)
        if(NOT ARG_${required})
            message(FATAL_ERROR
                "bioseq_add_postgres_error_safety_pair requires "
                "${required} or its profile-target-derived value")
        endif()
    endforeach()
    _bioseq_require_postgres_error_safety_codegen()
    get_filename_component(
        _pair_output_directory "${ARG_OUTPUT}" DIRECTORY)
    get_filename_component(
        _pair_stamp_directory "${ARG_OUTPUT_STAMP}" DIRECTORY)
    add_custom_command(
        OUTPUT
            "${ARG_OUTPUT}"
            "${ARG_OUTPUT_STAMP}"
        COMMAND
            "${CMAKE_COMMAND}" -E make_directory
            "${_pair_output_directory}"
            "${_pair_stamp_directory}"
        COMMAND
            "${BIOSEQ_CODEGEN_EXECUTABLE}"
            safety
            validate-pair
            --production-ledger "${ARG_PRODUCTION_LEDGER}"
            --test-indexes-ledger
                "${ARG_TEST_INDEXES_LEDGER}"
            --provenance "${ARG_PROVENANCE}"
            --producer-executable
                "${BIOSEQ_CODEGEN_EXECUTABLE}"
            --output "${ARG_OUTPUT}"
            --stamp "${ARG_OUTPUT_STAMP}"
        DEPENDS
            "${ARG_PRODUCTION_TARGET}"
            "${ARG_TEST_INDEXES_TARGET}"
            "${ARG_PRODUCTION_LEDGER}"
            "${ARG_TEST_INDEXES_LEDGER}"
            "${ARG_PROVENANCE}"
            "${BIOSEQ_CODEGEN_EXECUTABLE}"
        VERBATIM
        COMMENT
            "Validating production/test-indexes PostgreSQL safety pair"
    )
    add_custom_target(
        "${ARG_TARGET}"
        DEPENDS
            "${ARG_OUTPUT}"
            "${ARG_OUTPUT_STAMP}"
    )
    set("${ARG_TARGET}_OUTPUT" "${ARG_OUTPUT}" PARENT_SCOPE)
    set("${ARG_TARGET}_STAMP" "${ARG_OUTPUT_STAMP}" PARENT_SCOPE)
endfunction()
