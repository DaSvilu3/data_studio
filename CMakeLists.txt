cmake_minimum_required(VERSION 3.21)
project(DataStudio VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(APPLE AND NOT DEFINED CMAKE_PREFIX_PATH)
  execute_process(COMMAND brew --prefix qt OUTPUT_VARIABLE BREW_QT
                  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(BREW_QT)
    list(APPEND CMAKE_PREFIX_PATH "${BREW_QT}")
  endif()
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Network Concurrent)
qt_standard_project_setup()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
# MySQL/MariaDB client library. MariaDB Connector/C is preferred: it speaks
# both servers' protocols and still ships mysql_native_password, which Oracle's
# client removed in 9.x -- without it, connecting to any MariaDB server fails
# with "Authentication plugin 'mysql_native_password' cannot be loaded".
# Both expose the same mysql.h API, so the driver compiles against either.
if(APPLE)
  execute_process(COMMAND brew --prefix mariadb-connector-c
                  OUTPUT_VARIABLE BREW_MARIADB
                  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(BREW_MARIADB AND EXISTS "${BREW_MARIADB}/lib/pkgconfig")
    # Keg-only, so pkg-config needs pointing at it.
    set(ENV{PKG_CONFIG_PATH} "${BREW_MARIADB}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
  endif()
endif()

pkg_check_modules(MARIADB IMPORTED_TARGET libmariadb)
if(NOT MARIADB_FOUND)
  pkg_check_modules(MYSQLCLIENT IMPORTED_TARGET mysqlclient)
endif()

# ---------------------------------------------------------------- dscore
# Pure C++ (no Qt): drivers, schema model, SQL analysis, query generation.
add_library(dscore STATIC
  src/core/Value.cpp
  src/core/Schema.cpp
  src/core/DriverRegistry.cpp
  src/core/sqlite/SqliteDriver.cpp
  src/query/SqlLexer.cpp
  src/query/SqlContext.cpp
  src/query/JoinGraph.cpp
  src/query/QueryBuilder.cpp
  src/query/Analyzer.cpp
)
target_include_directories(dscore PUBLIC src)
target_link_libraries(dscore PUBLIC PkgConfig::SQLITE3)

if(MARIADB_FOUND OR MYSQLCLIENT_FOUND)
  target_sources(dscore PRIVATE src/core/mysql/MySqlDriver.cpp)
  target_compile_definitions(dscore PUBLIC DS_HAVE_MYSQL=1)
  if(MARIADB_FOUND)
    target_link_libraries(dscore PUBLIC PkgConfig::MARIADB)
  else()
    target_link_libraries(dscore PUBLIC PkgConfig::MYSQLCLIENT)
  endif()
else()
  target_compile_definitions(dscore PUBLIC DS_HAVE_MYSQL=0)
endif()

# ---------------------------------------------------------------- dsui
# Qt widgets + the Claude client. A library so tests can link it too.
qt_add_library(dsui STATIC
  src/ai/SqlGenerator.cpp
  src/ai/SchemaPrompt.cpp
  src/ai/AiSettings.cpp
  src/ai/LocalSqlGenerator.cpp
  src/ai/ClaudeSqlGenerator.cpp
  src/ai/AiSettingsDialog.cpp
  src/ui/MainWindow.cpp
  src/ui/ConnectionDialog.cpp
  src/ui/ConnectionStore.cpp
  src/ui/SchemaTree.cpp
  src/ui/SqlEditor.cpp
  src/ui/SqlHighlighter.cpp
  src/ui/ResultsModel.cpp
  src/ui/ResultsView.cpp
  src/ui/QueryExecutor.cpp
  src/ui/AnalyzerPanel.cpp
  src/ui/QueryBuilderPanel.cpp
  src/ui/NlPromptBar.cpp
)
target_link_libraries(dsui PUBLIC dscore Qt6::Widgets Qt6::Network Qt6::Concurrent)

# ---------------------------------------------------------------- app
qt_add_executable(data-studio MACOSX_BUNDLE src/main.cpp)
target_link_libraries(data-studio PRIVATE dsui)
set_target_properties(data-studio PROPERTIES
  MACOSX_BUNDLE ON
  MACOSX_BUNDLE_BUNDLE_NAME "Data Studio"
  MACOSX_BUNDLE_GUI_IDENTIFIER "dev.datastudio.app"
  MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
  MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
  WIN32_EXECUTABLE ON
)

# ---------------------------------------------------------------- tests
# ctest -V. The UI tests run headless via the offscreen platform plugin.
include(CTest)
if(BUILD_TESTING)
  add_executable(ds-core-tests tests/core_tests.cpp)
  target_link_libraries(ds-core-tests PRIVATE dscore)
  target_include_directories(ds-core-tests PRIVATE ${CMAKE_SOURCE_DIR})
  add_test(NAME core COMMAND ds-core-tests)

  qt_add_executable(ds-ui-tests tests/ui_tests.cpp)
  target_link_libraries(ds-ui-tests PRIVATE dsui)
  target_include_directories(ds-ui-tests PRIVATE ${CMAKE_SOURCE_DIR})
  add_test(NAME ui COMMAND ds-ui-tests)
  set_tests_properties(ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

