cmake_minimum_required(VERSION 3.24)
project(tandem-fm VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets DBus Pdf Test OPTIONAL_COMPONENTS LinguistTools)
qt_standard_project_setup(I18N_TRANSLATED_LANGUAGES de)

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
  src/core/DesktopApps.h src/core/DesktopApps.cpp
  src/core/Archive.h src/core/Archive.cpp
  src/core/Vfs.cpp
  src/core/MultiRename.h src/core/MultiRename.cpp
  src/core/ExifDate.h src/core/ExifDate.cpp
  src/core/Permissions.h src/core/Permissions.cpp
  src/core/Trash.h src/core/Trash.cpp
  src/core/Keyring.h src/core/Keyring.cpp
  src/core/Duplicates.h src/core/Duplicates.cpp
  src/core/DiskUsage.h src/core/DiskUsage.cpp
  src/core/Checksums.h src/core/Checksums.cpp
  src/core/IoScheduler.h src/core/IoScheduler.cpp
  src/core/Fuzzy.h src/core/Fuzzy.cpp
  src/core/FolderHistory.h src/core/FolderHistory.cpp
  src/core/Undo.h src/core/Undo.cpp
  src/core/GitStatus.h src/core/GitStatus.cpp
  src/core/Search.h src/core/Search.cpp
  src/core/FolderCompare.h src/core/FolderCompare.cpp
  src/core/Remote.h src/core/Remote.cpp
  src/core/Backend.h src/core/Backend.cpp
  src/core/BackendCopy.cpp
  src/core/SftpBackend.cpp
  src/core/SmbBackend.cpp
)
target_include_directories(tandem_core PUBLIC src)
find_package(LibArchive 3.3 REQUIRED)
target_link_libraries(tandem_core PUBLIC Qt6::Core PRIVATE LibArchive::LibArchive)
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
  src/ui/MultiRenameDialog.h src/ui/MultiRenameDialog.cpp
  src/ui/EditNames.h src/ui/EditNames.cpp
  src/ui/FuzzyPopup.h src/ui/FuzzyPopup.cpp
  src/ui/PathBar.h src/ui/PathBar.cpp
  src/ui/KeyChordPopup.h src/ui/KeyChordPopup.cpp
  src/ui/CommandBar.h src/ui/CommandBar.cpp
  src/ui/SyncDialog.h src/ui/SyncDialog.cpp
  src/ui/PermissionsDialog.h src/ui/PermissionsDialog.cpp
  src/ui/ShortcutsDialog.h src/ui/ShortcutsDialog.cpp
  src/ui/SettingsDialog.h src/ui/SettingsDialog.cpp
  src/ui/TrashDialog.h src/ui/TrashDialog.cpp
  src/ui/DuplicatesDialog.h src/ui/DuplicatesDialog.cpp
  src/ui/DiskUsageDialog.h src/ui/DiskUsageDialog.cpp
  src/ui/ChecksumDialog.h src/ui/ChecksumDialog.cpp
  src/ui/Thumbnails.h src/ui/Thumbnails.cpp
  src/ui/ThumbGrid.h src/ui/ThumbGrid.cpp
  src/ui/Network.h src/ui/Network.cpp
  src/ui/AuthDialogs.h src/ui/AuthDialogs.cpp
  src/ui/viewer/BlockCache.h src/ui/viewer/BlockCache.cpp
  src/ui/viewer/ByteViews.h src/ui/viewer/ByteViews.cpp
  src/ui/viewer/ImageView.h src/ui/viewer/ImageView.cpp
  src/ui/viewer/ViewerWindow.h src/ui/viewer/ViewerWindow.cpp
  src/ui/viewer/Previewers.h src/ui/viewer/Previewers.cpp
  src/ui/viewer/FileInfo.h src/ui/viewer/FileInfo.cpp
)
target_link_libraries(tandem_ui PUBLIC tandem_core Qt6::Widgets Qt6::DBus Qt6::Pdf)

# Optional: network locations and drives through GIO/GVfs.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(GIO IMPORTED_TARGET gio-2.0)
endif()
if(GIO_FOUND)
  target_link_libraries(tandem_ui PRIVATE PkgConfig::GIO)
  target_compile_definitions(tandem_ui PRIVATE TANDEM_HAVE_GIO)
  message(STATUS "Network locations (GIO/GVfs): enabled")
else()
  message(STATUS "Network locations (GIO/GVfs): disabled (gio-2.0 not found)")
endif()

# Optional: native SFTP (libssh).
if(PkgConfig_FOUND)
  pkg_check_modules(LIBSSH IMPORTED_TARGET libssh>=0.11)
endif()
if(LIBSSH_FOUND)
  target_link_libraries(tandem_core PRIVATE PkgConfig::LIBSSH)
  target_compile_definitions(tandem_core PRIVATE TANDEM_HAVE_LIBSSH)
  message(STATUS "Native SFTP (libssh ${LIBSSH_VERSION}): enabled")
else()
  message(STATUS "Native SFTP: disabled (libssh >= 0.11 not found; GVfs is used instead)")
endif()

# Optional: POSIX ACLs (extra users and groups in the permissions editor).
if(PkgConfig_FOUND)
  pkg_check_modules(LIBACL IMPORTED_TARGET libacl)
endif()
if(LIBACL_FOUND)
  target_link_libraries(tandem_core PRIVATE PkgConfig::LIBACL)
  target_compile_definitions(tandem_core PRIVATE TANDEM_HAVE_ACL)
  message(STATUS "ACLs (libacl ${LIBACL_VERSION}): enabled")
else()
  message(STATUS "ACLs: disabled (libacl not found; permissions editor shows mode bits only)")
endif()

# Optional: the desktop keyring (Secret Service) for server passwords.
if(PkgConfig_FOUND)
  pkg_check_modules(LIBSECRET IMPORTED_TARGET libsecret-1)
endif()
if(LIBSECRET_FOUND)
  target_link_libraries(tandem_core PRIVATE PkgConfig::LIBSECRET)
  target_compile_definitions(tandem_core PRIVATE TANDEM_HAVE_LIBSECRET)
  message(STATUS "Keyring (libsecret ${LIBSECRET_VERSION}): enabled")
else()
  message(STATUS "Keyring: disabled (libsecret-1 not found; passwords last for the session)")
endif()

# Optional: native SMB (Samba's libsmbclient).
if(PkgConfig_FOUND)
  pkg_check_modules(SMBCLIENT IMPORTED_TARGET smbclient)
endif()
if(SMBCLIENT_FOUND)
  target_link_libraries(tandem_core PRIVATE PkgConfig::SMBCLIENT)
  target_compile_definitions(tandem_core PRIVATE TANDEM_HAVE_SMBCLIENT)
  message(STATUS "Native SMB (libsmbclient): enabled")
else()
  message(STATUS "Native SMB: disabled (libsmbclient not found; GVfs is used instead)")
endif()

# Optional: syntax highlighting in the viewer (KDE's KSyntaxHighlighting).
find_package(KF6SyntaxHighlighting QUIET)
if(KF6SyntaxHighlighting_FOUND)
  target_link_libraries(tandem_ui PRIVATE KF6::SyntaxHighlighting)
  target_compile_definitions(tandem_ui PRIVATE TANDEM_HAVE_KSYNTAXHIGHLIGHTING)
  message(STATUS "Viewer syntax highlighting: enabled")
else()
  message(STATUS "Viewer syntax highlighting: disabled (KF6SyntaxHighlighting not found)")
endif()
target_compile_options(tandem_ui PRIVATE -Wall -Wextra)

add_executable(tandem-fm src/main.cpp)
target_link_libraries(tandem-fm PRIVATE tandem_ui)
# The app icon, inside the binary too (for running from the build folder).
qt_add_resources(tandem-fm "icons" PREFIX "/icons" BASE data FILES data/tandem-fm.svg)

# Translations (needs qt6-tools): `cmake --build build --target update_translations`
# refreshes translations/*.ts from the sources; the .qm files are compiled into
# the binary under :/i18n.
if(Qt6LinguistTools_FOUND)
  qt_add_translations(TARGETS tandem-fm
    SOURCE_TARGETS tandem_core tandem_ui tandem-fm
    TS_FILE_BASE tandem-fm
    TS_FILE_DIR translations
    RESOURCE_PREFIX /i18n
    LUPDATE_OPTIONS -no-obsolete -locations none)
  message(STATUS "Translations: enabled")
else()
  message(STATUS "Translations: disabled (install qt6-tools)")
endif()

add_executable(tandem-bench bench/bench_list.cpp)
target_link_libraries(tandem-bench PRIVATE tandem_core)

enable_testing()
add_executable(test_core tests/test_core.cpp)
target_link_libraries(test_core PRIVATE tandem_core Qt6::Test)
add_test(NAME test_core COMMAND test_core)
add_executable(test_ui tests/test_ui.cpp)
target_link_libraries(test_ui PRIVATE tandem_ui Qt6::Test)
target_compile_definitions(test_ui PRIVATE TANDEM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME test_ui COMMAND test_ui)
set_tests_properties(test_ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_executable(test_viewer tests/test_viewer.cpp)
target_link_libraries(test_viewer PRIVATE tandem_ui Qt6::Test)
add_test(NAME test_viewer COMMAND test_viewer)
set_tests_properties(test_viewer PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

install(TARGETS tandem-fm RUNTIME DESTINATION bin)
install(FILES tandem-fm.desktop DESTINATION share/applications)
install(FILES data/tandem-fm.svg DESTINATION share/icons/hicolor/scalable/apps)
foreach(size 32 48 256)
  install(FILES data/icons/${size}x${size}/tandem-fm.png DESTINATION share/icons/hicolor/${size}x${size}/apps)
endforeach()
install(FILES LICENSE DESTINATION share/licenses/tandem-fm)
