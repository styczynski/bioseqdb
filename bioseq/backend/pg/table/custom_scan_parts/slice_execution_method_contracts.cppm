/**
 * @file
 * @brief Declares the stable SliceProject executor method table.
 */
module;
struct CustomExecMethods;

export module bioseq.postgres.table:custom_scan_slice_execution_method_contracts;

/** @brief Returns the executor method table for slice projection wrappers. */
export [[nodiscard]] const CustomExecMethods& bioseq_slice_project_exec_methods() noexcept;
