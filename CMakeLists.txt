cmake_minimum_required(VERSION 3.16)
project(KarabasFlasher VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 QUIET COMPONENTS Core Gui Widgets Network SerialPort)
if(NOT Qt6_FOUND)
    message(FATAL_ERROR
        "Qt 6 (Core, Gui, Widgets, Network, SerialPort) was not found.\n"
        "  Ubuntu/Debian: sudo apt install qt6-base-dev qt6-serialport-dev libgl1-mesa-dev\n"
        "  Fedora:        sudo dnf install qt6-qtbase-devel qt6-qtserialport-devel\n"
        "  Arch/Manjaro:  sudo pacman -S qt6-base qt6-serialport\n"
        "If Qt is installed in a non-standard place, pass -DCMAKE_PREFIX_PATH=<path to Qt>.")
endif()

# minizip-ng: распаковка extra.zip (zlib-подобная лицензия, есть во всех дистрибутивах).
# Сначала штатный поиск через pkg-config (minizip-ng ставит .pc-файл), затем
# запасной путь через find_path/find_library для систем без pkg-config.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(MinizipNG QUIET minizip-ng)
endif()
if(NOT MinizipNG_FOUND)
    find_path(MinizipNG_INCLUDE_DIR NAMES mz.h PATH_SUFFIXES minizip-ng)
    find_library(MinizipNG_LIBRARY NAMES minizip-ng minizip)
    if(MinizipNG_INCLUDE_DIR AND MinizipNG_LIBRARY)
        set(MinizipNG_FOUND TRUE)
        set(MinizipNG_INCLUDE_DIRS "${MinizipNG_INCLUDE_DIR}")
        set(MinizipNG_LIBRARIES "${MinizipNG_LIBRARY}")
    endif()
endif()
if(NOT MinizipNG_FOUND)
    message(FATAL_ERROR
        "minizip-ng was not found.\n"
        "  Ubuntu/Debian: sudo apt install libminizip-ng-dev pkg-config\n"
        "  Fedora:        sudo dnf install minizip-ng-devel pkgconf\n"
        "  Arch/Manjaro:  sudo pacman -S minizip-ng pkgconf\n"
        "  macOS:         brew install minizip-ng pkg-config\n"
        "  Windows:       vcpkg install minizip-ng:x64-windows")
endif()

# Версия: номер из project(... VERSION) + описание из git (тег, число коммитов, хэш, -dirty).
# Описание считается при конфигурации; если git нет (сборка из zip), будет "no-git".
find_package(Git QUIET)
set(APP_GIT_DESCRIBE "")
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
                    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                    OUTPUT_VARIABLE APP_GIT_DESCRIBE
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
endif()
if(NOT APP_GIT_DESCRIBE)
    set(APP_GIT_DESCRIBE "no-git")
endif()
configure_file(src/Version.h.in ${CMAKE_BINARY_DIR}/generated/Version.h @ONLY)

# Платформенный слой (работа с дисками: поиск, монтирование, извлечение).
if(WIN32)
    set(PLATFORM_SRC src/platform_windows.cpp)
elseif(APPLE)
    set(PLATFORM_SRC src/platform_macos.cpp)
elseif(UNIX)
    set(PLATFORM_SRC src/platform_linux.cpp)
else()
    message(FATAL_ERROR "Unsupported platform. See src/Platform.h for the interface to implement.")
endif()


qt_add_executable(KarabasFlasher WIN32 MACOSX_BUNDLE
    resources/resources.qrc
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
    src/Worker.cpp
    src/Worker.h
    src/Models.h
    src/Uf2.cpp
    src/Uf2.h
    src/Util.cpp
    src/Util.h
    src/Platform.h
    ${PLATFORM_SRC}
)

target_include_directories(KarabasFlasher PRIVATE ${CMAKE_BINARY_DIR}/generated)


target_include_directories(KarabasFlasher PRIVATE ${MinizipNG_INCLUDE_DIRS})
target_link_libraries(KarabasFlasher PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::SerialPort
    ${MinizipNG_LIBRARIES})

# Иконка приложения для exe (Windows) и .app-бандла (macOS).
# Linux использует resources/krb.png через install() ниже.
# Файлы иконок подключаются только если они есть в репозитории.
if(WIN32 AND EXISTS "${CMAKE_SOURCE_DIR}/resources/icon.rc"
        AND EXISTS "${CMAKE_SOURCE_DIR}/resources/icon.ico")
    target_sources(KarabasFlasher PRIVATE resources/icon.rc)
elseif(APPLE AND EXISTS "${CMAKE_SOURCE_DIR}/resources/icon.icns")
    set_source_files_properties(resources/icon.icns PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources")
    target_sources(KarabasFlasher PRIVATE resources/icon.icns)
endif()

if(APPLE)
    set_target_properties(KarabasFlasher PROPERTIES
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.karabas.flasher"
        MACOSX_BUNDLE_BUNDLE_NAME "Karabas Flasher"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
    if(EXISTS "${CMAKE_SOURCE_DIR}/resources/icon.icns")
        set_target_properties(KarabasFlasher PROPERTIES
            MACOSX_BUNDLE_ICON_FILE "icon.icns")
    endif()
endif()

install(TARGETS KarabasFlasher RUNTIME DESTINATION bin BUNDLE DESTINATION .)

# Linux: правило udev выдаёт доступ к порту Карабаса без группы dialout/uucp
if(UNIX AND NOT APPLE)
    install(FILES packaging/70-karabas.rules DESTINATION lib/udev/rules.d)
    install(FILES packaging/karabas-flasher.desktop DESTINATION share/applications)
    install(FILES packaging/karabas-flasher.svg DESTINATION share/icons/hicolor/scalable/apps)
    install(FILES resources/krb.png DESTINATION share/icons/hicolor/256x256/apps RENAME karabas-flasher.png)
    install(FILES LICENSE DESTINATION share/doc/karabas-flasher RENAME copyright)
    install(FILES THIRD_PARTY.md DESTINATION share/doc/karabas-flasher)

    # ---- Пакет .deb (Ubuntu/Debian): cd build && cpack -G DEB ----
    # Зависимости определяются автоматически по бинарнику (dpkg-shlibdeps) и дополняются вручную,
    # поэтому `sudo apt install ./karabas-flasher_*.deb` сам доставит Qt, udisks2 и остальное.
    set(CPACK_GENERATOR "DEB")
    set(CPACK_PACKAGE_NAME "karabas-flasher")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Firmware and core flasher for Karabas Go boards")
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Karabas Flasher <noreply@users.noreply.github.com>")
    set(CPACK_DEBIAN_PACKAGE_SECTION "electronics")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "udisks2, libglib2.0-bin, ca-certificates")
    set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "qt6-qpa-plugins, qt6-wayland, pkexec | policykit-1, eject")

    # postinst: перечитать правила udev. Права 755 выставляются при конфигурации,
    # чтобы не зависеть от прав файла в zip или git.
    file(COPY packaging/debian/postinst DESTINATION ${CMAKE_BINARY_DIR}/debian
         FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "${CMAKE_BINARY_DIR}/debian/postinst")

    include(CPack)
endif()
