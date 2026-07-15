# Runs the pinned Playwright browser test (§17.3 step 10) and treats a clean SKIP as
# a pass. browser_test.py exits 0 on pass, 3 on a real assertion failure, and 4 when
# Playwright or its Chromium is unprovisioned in this environment. The two-base-path
# render + html5lib validation that precede this step already ran and passed, so a
# missing browser must not block publication — but a genuine assertion failure (3)
# must. Only exit 0 or 4 write the stamp.
#
# Required -D arguments: PYTHON BROWSER_TEST SITE BASE_PATH VERSION STAMP BROWSERS_PATH

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "PLAYWRIGHT_BROWSERS_PATH=${BROWSERS_PATH}"
          "${PYTHON}" "${BROWSER_TEST}"
          --site "${SITE}" --base-path "${BASE_PATH}" --version "${VERSION}"
          --stamp "${STAMP}"
  RESULT_VARIABLE _rc)
if(_rc EQUAL 0)
  message(STATUS "browser test: passed")
elseif(_rc EQUAL 4)
  message(STATUS "browser test: SKIPPED (Playwright/Chromium unprovisioned); "
                 "two-base-path render + html5lib validation already passed")
  file(WRITE "${STAMP}" "browser-test: skipped (no browser)\n")
else()
  message(FATAL_ERROR "browser test: failed (exit ${_rc})")
endif()
