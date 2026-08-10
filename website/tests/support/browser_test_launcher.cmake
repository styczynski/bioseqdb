# Runs the pinned Playwright browser test (§17.3 step 10). Browser acceptance is a
# publication contract when BIOSEQ_WEBSITE_BROWSER_TESTS is enabled, so an absent
# Playwright/Chromium installation (exit 4) is a failure rather than a silent pass.
# The website CMake target provisions the pinned browser before invoking this file.
#
# Required -D arguments: PYTHON BROWSER_TEST SITE BASE_PATH VERSION MODEL
# DESIGN_SOURCE ARTIFACT_DIR STAMP BROWSERS_PATH

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
          "PLAYWRIGHT_BROWSERS_PATH=${BROWSERS_PATH}"
          "PYTHONDONTWRITEBYTECODE=1"
          "${PYTHON}" "${BROWSER_TEST}"
          --site "${SITE}" --base-path "${BASE_PATH}" --version "${VERSION}"
          --model "${MODEL}" --design-source "${DESIGN_SOURCE}"
          --artifact-dir "${ARTIFACT_DIR}"
          --stamp "${STAMP}"
  RESULT_VARIABLE _rc)
if(_rc EQUAL 0)
  message(STATUS "browser test: passed")
else()
  message(FATAL_ERROR "browser test: failed (exit ${_rc})")
endif()
