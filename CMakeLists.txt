cmake_minimum_required(VERSION 3.24)
project(tandem-fm VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Test)
qt_standard_project_setup()

# Core engine: filesystem access, jobs, watching. QtCore only, no UI.
add_library(tandem_core STATIC
  src/core/FileEntry.h
  src/core/Util.h src/core/Util.cpp
  src/core/Sorting.h src/core/Sorting.cpp
  src/core/DirLister.h src/core/DirLister.cpp
  src/core/DirWatcher.h src/core/DirWatcher.cpp
  src/core/Mounts.h src/core/Mounts.cpp
  src/core/Job.h src/core/Job.cpp
  src/core/FileOps.h src/core/FileOps.cpp
)
target_include_directories(tandem_core PUBLIC src)
target_link_libraries(tandem_core PUBLIC Qt6::Core)
target_compile_options(tandem_core PRIVATE -Wall -Wextra)

# Widgets UI, as a library so tests can drive the real window.
add_library(tandem_ui STATIC
  src/ui/Theme.h src/ui/Theme.cpp
  src/ui/IconCache.h src/ui/IconCache.cpp
  src/ui/FileModel.h src/ui/FileModel.cpp
  src/ui/FileView.h src/ui/FileView.cpp
  src/ui/DirTab.h src/ui/DirTab.cpp
  src/ui/Panel.h src/ui/Panel.cpp
  src/ui/Dialogs.h src/ui/Dialogs.cpp
  src/ui/JobManager.h src/ui/JobManager.cpp
  src/ui/JobsPanel.h src/ui/JobsPanel.cpp
  src/ui/MainWindow.h src/ui/MainWindow.cpp
)
target_link_libraries(tandem_ui PUBLIC tandem_core Qt6::Widgets)
target_compile_options(tandem_ui PRIVATE -Wall -Wextra)

add_executable(tandem-fm src/main.cpp)
target_link_libraries(tandem-fm PRIVATE tandem_ui)

add_executable(tandem-bench bench/bench_list.cpp)
target_link_libraries(tandem-bench PRIVATE tandem_core)

enable_testing()
add_executable(test_core tests/test_core.cpp)
target_link_libraries(test_core PRIVATE tandem_core Qt6::Test)
add_test(NAME test_core COMMAND test_core)
add_executable(test_ui tests/test_ui.cpp)
target_link_libraries(test_ui PRIVATE tandem_ui Qt6::Test)
add_test(NAME test_ui COMMAND test_ui)
set_tests_properties(test_ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

install(TARGETS tandem-fm RUNTIME DESTINATION bin)
install(FILES tandem-fm.desktop DESTINATION share/applications)
