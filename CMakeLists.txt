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

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Network)
qt_standard_project_setup()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
pkg_check_modules(MYSQLCLIENT IMPORTED_TARGET mysqlclient)

if(NOT MYSQLCLIENT_FOUND)
  message(WARNING "libmysqlclient not found -- MySQL support will be disabled.")
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

if(MYSQLCLIENT_FOUND)
  target_sources(dscore PRIVATE src/core/mysql/MySqlDriver.cpp)
  target_link_libraries(dscore PUBLIC PkgConfig::MYSQLCLIENT)
  target_compile_definitions(dscore PUBLIC DS_HAVE_MYSQL=1)
else()
  target_compile_definitions(dscore PUBLIC DS_HAVE_MYSQL=0)
endif()

# ---------------------------------------------------------------- app
qt_add_executable(data-studio
  src/main.cpp
  src/ai/NlSqlClient.cpp
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
target_link_libraries(data-studio PRIVATE dscore Qt6::Widgets Qt6::Network)
set_target_properties(data-studio PROPERTIES
  MACOSX_BUNDLE_BUNDLE_NAME "Data Studio"
  WIN32_EXECUTABLE ON
)
