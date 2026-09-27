/**
 * @file
 * @brief Declares the stable SliceProject executor method table.
 */
module;
struct CustomExecMethods;

export module bioseq.postgres.table:custom_scan_slice_execution_method_contracts;

/** @brief Returns the PostgreSQL executor callbacks for BioseqSliceProject. */
export [[nodiscard]] const CustomExecMethods& bioseq_slice_project_exec_methods() noexcept;
