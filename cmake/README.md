# Build support

CMake uses this folder to build the PostgreSQL extension and the test executables, including the steps that produce C++ sources during the build C++ modules are shared between targets. That lets the  extension and test executables reuse compiled modules, while separate source lists keep test support out of the production module library.

SQL definitions are generated too. CMake runs the generators and records their outputs as build dependencies, so a jarget that imports a generated module waits for that module to be ready. PostgreSQL is found through `pg_config`. Install rules use its reported directories for the extension library and for the control file and SQL script that PostgreSQL needs to create the extension.

For database tests, the helpers prepare private PostgreSQL installations with the current extension files. Python starts temporary clusters for SQL checks and tests that, deliberately  trigger errors. The build has its own tests. These check source selection and the rules that limit which functions the extension exports.
