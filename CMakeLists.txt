cmake_minimum_required(VERSION 3.8)

if(MAKE_TESTS)
    add_subdirectory(test)
    return()
endif()

if(NOT DEFINED PLATFORM)
    set(PLATFORM "native")
    message("Compiling for native system")
elseif("${PLATFORM}" STREQUAL "WIN")
    message("Cross - compiling for Windows")
    set(CMAKE_CXX_COMPILER /usr/bin/x86_64-w64-mingw32-g++-posix)
    set(CMAKE_C_COMPILER /usr/bin/x86_64-w64-mingw32-gcc-posix)
    set(CMAKE_RC_COMPILER /usr/bin/x86_64-w64-mingw32-windres)
    set(CMAKE_SYSTEM_NAME Windows)
endif()

project(Variable-Trace)

set(VARIABLE_TRACE_VERSION 1.1.0)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS 1)

if(PRODUCTION)
    set(CMAKE_BUILD_TYPE Release)
    message("Building for production!")
else()
    set(CMAKE_BUILD_TYPE Debug)
endif()

if(WIN32 AND "${PLATFORM}" STREQUAL "native")
    set(LLVM_DIR "C:/msys64/mingw64/bin")
    set(CMAKE_PREFIX_PATH ${LLVM_DIR} ${CMAKE_PREFIX_PATH})
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=lld")
    set(CMAKE_AR "C:/msys64/mingw64/bin/llvm-ar.exe")
endif()

if(CMAKE_BUILD_TYPE STREQUAL "ReleaseWithDbg")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g1")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g1")
endif()

add_compile_options(-Wall
    -Wfatal-errors
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    -Wno-missing-field-initializers)

if(WIN32)
    add_compile_options(-Wa,-mbig-obj)
endif()

set(JLINK_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/jlink/inc/)

if(UNIX)
    include(FetchContent)
    FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG v1.13.0)

    FetchContent_GetProperties(spdlog)

    if(NOT spdlog_POPULATED)
        FetchContent_Populate(spdlog)
        add_subdirectory(${spdlog_SOURCE_DIR} ${spdlog_BINARY_DIR})
    endif()

    # this is to avoid warnings from spdlog
    get_target_property(SPDLOG_IID spdlog INTERFACE_INCLUDE_DIRECTORIES)
    set_target_properties(spdlog PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${SPDLOG_IID}")

    set(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake/modules)
    include(${CMAKE_MODULE_PATH}/Findlibusb.cmake)
    find_package(libusb REQUIRED)
    find_package(glfw3 REQUIRED)
    set(STLINK_LINUX ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stlink/lib/linux/libstlink.a)
    set(JLINK_LINUX ${CMAKE_CURRENT_SOURCE_DIR}/third_party/jlink/lib/linux/libjlinkarm.so.8)
    set(INSTALL_PATH /usr/local/Variable-Trace)
    set(LIB_INSTALL_PATH /usr/local/lib)
    set(DESKTOP_FILE_PATH /usr/share/applications)

    set(LIB_CONF_FILE ${CMAKE_CURRENT_SOURCE_DIR}/launch/install/Unix/Variable-Trace.conf)
    set(LIB_CONF_FILE_PATH /etc/ld.so.conf.d/)

    if(APPLE)
        find_package(OpenGL REQUIRED)
        find_package(GLUT REQUIRED)

        # TODO distinguish between ARM and x86 */
        set(STLINK_MACOS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stlink/lib/macos/libstlink.a)
    else()
        set(STLINK_LINUX ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stlink/lib/linux/libstlink.a)
    endif()
endif()

if(WIN32)
    enable_language("RC")
    set(ICON_RC "${CMAKE_CURRENT_SOURCE_DIR}/launch/install/Windows/icon.rc")
    set(GLFW3_WINDOWS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/GLFW/lib/windows/glfw3.dll)
    set(STLINK_WINDOWS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stlink/lib/windows/libstlink.a)
    set(JLINK_WINDOWS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/jlink/lib/windows/JLink_x64.dll)
    set(LIBUSB_WINDOWS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libusb/lib/windows/libusb-1.0.dll)
    set(LIBUSB_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libusb/inc/libusb-1.0)
    set(SPDLOG_WINDOWS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/spdlog/lib/windows/libspdlog.dll)
    set(GDB_WINDOWS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/GDB/windows/gdb.exe)
    set(INSTALL_PATH bin)
endif()

# ---------------------------------------------------------------- libdwarf
# The one optional dependency. The application is complete without it: the GDB
# parser answers the same questions through a child process, which is slower but
# works for every dialect. The in-process reader is linked when a library is
# found, and the code that needs it is guarded by the macro defined below, so a
# build without it still produces an application that runs and says why one of
# its three parsers is not there.
set(LIBDWARF_ROOT "" CACHE PATH "Prefix of a libdwarf installation, holding include/ and lib/")

if(WIN32 AND NOT LIBDWARF_ROOT)
    set(LIBDWARF_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libdwarf)
endif()

set(VARIABLE_TRACE_HAS_LIBDWARF FALSE)
set(LIBDWARF_RUNTIME_DLLS "")

if(WIN32)
    set(LIBDWARF_INCLUDE_DIR ${LIBDWARF_ROOT}/include/windows)
    set(LIBDWARF_LIBRARIES ${LIBDWARF_ROOT}/lib/windows/libdwarf.dll.a)
    set(LIBDWARF_RUNTIME_DLLS
        ${LIBDWARF_ROOT}/lib/windows/libdwarf-2.dll
        ${LIBDWARF_ROOT}/lib/windows/zlib1.dll
        ${LIBDWARF_ROOT}/lib/windows/libzstd.dll)

    if(EXISTS ${LIBDWARF_LIBRARIES})
        set(VARIABLE_TRACE_HAS_LIBDWARF TRUE)
    endif()
else()
    find_package(PkgConfig QUIET)

    if(PkgConfig_FOUND)
        pkg_check_modules(LIBDWARF QUIET libdwarf)

        if(LIBDWARF_FOUND)
            set(LIBDWARF_INCLUDE_DIR ${LIBDWARF_INCLUDE_DIRS})
            set(LIBDWARF_LIBRARIES ${LIBDWARF_LINK_LIBRARIES})
            set(VARIABLE_TRACE_HAS_LIBDWARF TRUE)
        endif()
    endif()
endif()

set(PROJECT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/commons.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Gui/Gui.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Gui/GuiPlots.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Gui/GuiSwoPlots.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Gui/GuiSwoControl.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Gui/GuiAbout.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Gui/GuiAcqusition.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Gui/GuiRecorder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Gui/GuiFlashing.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Gui/GuiScreenshot.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Gui/GuiHelper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MemoryReader/StlinkDebugProbe.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MemoryReader/JlinkDebugProbe.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MemoryReader/SerialProtocol.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MemoryReader/SerialPort.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MemoryReader/SerialDebugProbe.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MemoryReader/GdbServerDebugProbe.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MemoryReader/SerialDriverLayout.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Recorder/RecorderLayout.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Recorder/RecorderHandler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Plot/Plot.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Variable/Variable.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MovingAverage/MovingAverage.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ConfigHandler/ConfigHandler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ProjectHandler/ProjectHandler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ProjectHandler/GlobalConfig.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Flashing/FlashingService.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/WritePlanner/WritePlanner.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Screenshot/PlotExport.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Screenshot/PngWriter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/FileHandler/NFDFileHandler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ImguiPlugins/ImguiPlugins.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stlink/inc/spdlogWrapper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/PlotHandler/PlotHandler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/TraceReader/TraceReader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/TraceReader/StlinkTraceProbe.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/TraceReader/JlinkTraceProbe.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ElfParser/IElfParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ElfParser/GdbParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ElfParser/ElfParserFactory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ElfParser/ElfTarget.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ElfParser/DwarfDecode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ElfParser/LibDwarfParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ElfParser/OfdDwarfXml.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ElfParser/TiOfdParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Rtos/RtosProfileBase.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Rtos/FreeRtosProfile.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Rtos/RtosFactory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Rtos/RtosListWalker.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Rtos/RtosTaskEnumerator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Rtos/RtosModel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Rtos/RtosSampler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Rtos/GdbRtosLayout.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Rtos/RtosSyncEnumerator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Rtos/RtosTableModel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/CSVStreamer/CSVStreamer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/I18n/I18n.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VariableHandler/VariableHandler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/DataHandler/ViewerDataHandler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/DataHandler/TraceDataHandler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/McpServer/McpTypes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/McpServer/McpProtocol.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/McpServer/McpToolRegistry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/McpServer/McpBridge.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/McpServer/McpServer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/McpServer/CommandServer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/McpServer/McpToolHelpers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/McpServer/ProjectTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/McpServer/GroupTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/McpServer/PlotTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/McpServer/VariableTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/McpServer/RecorderTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/McpServer/AcquisitionTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/McpServer/DataTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/McpServer/FlashingTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/McpServer/RtosTools.cpp)

set(IMGUI_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui_demo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui_draw.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui_tables.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui_widgets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/backends/imgui_impl_glfw.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/backends/imgui_impl_opengl3.cpp)

set(IMPLOT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/implot/implot.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/implot/implot_demo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/implot/implot_items.cpp)

set_source_files_properties(
    ${IMPLOT_SOURCES}
    PROPERTIES
    COMPILE_FLAGS "-w"
)

# A headless CPU benchmark for the exact ImGui/ImPlot plotting path. It is
# excluded from ordinary builds and can be requested explicitly when recording
# the P3 baseline; no OpenGL backend or probe is involved.
add_executable(P3-Gui-Benchmark EXCLUDE_FROM_ALL
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/performance/p3_gui_benchmark.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui_draw.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui_tables.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui_widgets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/implot/implot.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/implot/implot_items.cpp)
target_include_directories(P3-Gui-Benchmark PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Plot
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/implot)

# Measures only CSV producer and writer CPU/file paths; it has no probe or GUI
# dependency and is excluded from ordinary builds.
add_executable(P4-Csv-Benchmark EXCLUDE_FROM_ALL
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/performance/p4_csv_benchmark.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/CSVStreamer/CSVStreamer.cpp)
target_include_directories(P4-Csv-Benchmark PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/CSVStreamer
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/spdlog/inc)

# Exercises deterministic slow-disk and file-failure behavior without a probe.
add_executable(P42-Csv-Fault-Benchmark EXCLUDE_FROM_ALL
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/performance/p42_csv_fault_benchmark.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/CSVStreamer/CSVStreamer.cpp)
target_include_directories(P42-Csv-Fault-Benchmark PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/CSVStreamer
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/spdlog/inc)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/third_party/nfd/)

set(EXECUTABLE ${CMAKE_PROJECT_NAME})

add_executable(${EXECUTABLE} ${PROJECT_SOURCES} ${IMGUI_SOURCES} ${IMPLOT_SOURCES} ${ICON_RC})

# Runtime assets are copied by POST_BUILD commands below. Make their changes
# invalidate the link step as well; otherwise an incremental build can leave an
# old user guide, layout, or ST-Link target database beside a current binary.
file(GLOB STLINK_CHIP_DATABASE CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stlink/chips/*.chip)
set_property(TARGET ${EXECUTABLE} APPEND PROPERTY LINK_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui.ini
    ${CMAKE_CURRENT_SOURCE_DIR}/docs/Variable-Trace-User-Guide.html
    ${STLINK_CHIP_DATABASE})

set(GIT_VERSION_HPP ${CMAKE_CURRENT_SOURCE_DIR}/src/gitversion.hpp)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# Run on every build because .git/HEAD does not change when a commit advances
# the branch it names. The generator only rewrites the header when its content
# changes, so ordinary incremental builds still avoid recompilation.
add_custom_target(addGitVersion ALL
    COMMAND ${CMAKE_COMMAND} -E env PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR} ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/launch/addGitVersion.py
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    BYPRODUCTS ${GIT_VERSION_HPP}
    COMMENT "Generating ${GIT_VERSION_HPP}..."
    VERBATIM
)

add_dependencies(${EXECUTABLE} addGitVersion)

string(REGEX MATCH "([0-9]+)\\.([0-9]+)\\.([0-9]+)" _ "${VARIABLE_TRACE_VERSION}")
target_compile_definitions(${EXECUTABLE}
    PRIVATE VARIABLE_TRACE_VERSION_MAJOR=${CMAKE_MATCH_1}
    PRIVATE VARIABLE_TRACE_VERSION_MINOR=${CMAKE_MATCH_2}
    PRIVATE VARIABLE_TRACE_VERSION_REVISION=${CMAKE_MATCH_3}
)

target_include_directories(${EXECUTABLE} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Gui
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MemoryReader
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Plot
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ScrollingBuffer
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MovingAverage
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Variable
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ConfigHandler
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ProjectHandler
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ImguiPlugins
    ${CMAKE_CURRENT_SOURCE_DIR}/src/FileHandler
    ${CMAKE_CURRENT_SOURCE_DIR}/src/PlotHandler
    ${CMAKE_CURRENT_SOURCE_DIR}/src/TraceReader
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RingBuffer
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Statistics
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ElfParser
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Rtos
    ${CMAKE_CURRENT_SOURCE_DIR}/src/PlotGroupHandler
    ${CMAKE_CURRENT_SOURCE_DIR}/src/CSVStreamer
    ${CMAKE_CURRENT_SOURCE_DIR}/src/I18n
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VariableHandler
    ${CMAKE_CURRENT_SOURCE_DIR}/src/DataHandler
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Recorder
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Flashing
    ${CMAKE_CURRENT_SOURCE_DIR}/src/WritePlanner
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Screenshot
    ${CMAKE_CURRENT_SOURCE_DIR}/src/McpServer)

target_include_directories(${EXECUTABLE} SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb/
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/http/
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stlink/inc/
    ${JLINK_INCLUDE_DIR}
    ${LIBUSB_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/backends/
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/GLFW/inc/
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/GLFW/inc/GLFW/
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/implot/
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mINI/src/mini/
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nfd/src/include/
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/spdlog/inc/
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/CLI11/inc/)

if(VARIABLE_TRACE_HAS_LIBDWARF)
    target_include_directories(${EXECUTABLE} SYSTEM PRIVATE ${LIBDWARF_INCLUDE_DIR})
    target_link_libraries(${EXECUTABLE} ${LIBDWARF_LIBRARIES})
    target_compile_definitions(${EXECUTABLE} PRIVATE VARIABLE_TRACE_HAS_LIBDWARF=1)
    message(STATUS "Variable-Trace: the in-process DWARF reader is included")
else()
    message(STATUS "Variable-Trace: built without libdwarf, the DWARF parser will report itself unavailable")
endif()

if(APPLE)
    include_directories(${OPENGL_INCLUDE_DIRS} ${GLUT_INCLUDE_DIRS} ${STLINK_INCLUDE_DIRS})
    target_link_libraries(${EXECUTABLE} ${STLINK_MACOS} ${LIBUSB_LIBRARY} ${OPENGL_LIBRARIES} ${GLUT_LIBRARY} spdlog::spdlog pthread dl glfw nfd)
elseif(UNIX)
    target_link_libraries(${EXECUTABLE} ${STLINK_LINUX} ${LIBUSB_LIBRARY} ${JLINK_LINUX} spdlog::spdlog pthread dl GL glfw nfd)
elseif(WIN32)
    target_link_libraries(${EXECUTABLE} ${GLFW3_WINDOWS} ${STLINK_WINDOWS} ${LIBUSB_WINDOWS} ${SPDLOG_WINDOWS} ${JLINK_WINDOWS} -static ssp opengl32 ws2_32 nfd -static-libstdc++ -static-libgcc)
endif()

if(WIN32)
    add_custom_command(TARGET ${EXECUTABLE} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
        ${GLFW3_WINDOWS}
        ${CMAKE_BINARY_DIR}/glfw3.dll)

    add_custom_command(TARGET ${EXECUTABLE} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
        ${LIBUSB_WINDOWS}
        ${CMAKE_BINARY_DIR}/libusb-1.0.dll)

    add_custom_command(TARGET ${EXECUTABLE} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
        ${GDB_WINDOWS}
        ${CMAKE_BINARY_DIR}/gdb.exe)

    add_custom_command(TARGET ${EXECUTABLE} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
        ${JLINK_WINDOWS}
        ${CMAKE_BINARY_DIR})

    # The DWARF library and the two compression libraries it imports have to sit
    # next to the executable: the loader resolves them by name at start up, and
    # the failure when it cannot is a dialog rather than a message here.
    foreach(DWARF_DLL ${LIBDWARF_RUNTIME_DLLS})
        if(EXISTS ${DWARF_DLL})
            add_custom_command(TARGET ${EXECUTABLE} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy
                ${DWARF_DLL}
                ${CMAKE_BINARY_DIR})
        endif()
    endforeach()
endif()

add_custom_command(TARGET ${EXECUTABLE} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui.ini
    ${CMAKE_BINARY_DIR}/imgui.ini)

# The user guide is opened from the Help menu, so it has to sit beside the
# executable. The name is kept in ASCII on purpose: the path reaches the shell
# through an ANSI call, and a name outside that code page comes out mangled.
add_custom_command(TARGET ${EXECUTABLE} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
    ${CMAKE_CURRENT_SOURCE_DIR}/docs/Variable-Trace-User-Guide.html
    ${CMAKE_BINARY_DIR}/Variable-Trace-User-Guide.html)

# libstlink resolves the target chip database relative to the process working
# directory. Keep it beside the executable in developer and packaged builds,
# otherwise USB enumeration succeeds but target identification fails.
add_custom_command(TARGET ${EXECUTABLE} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stlink/chips
    ${CMAKE_BINARY_DIR}/chips)

install(TARGETS ${EXECUTABLE} RUNTIME DESTINATION ${INSTALL_PATH} COMPONENT applications)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/imgui.ini
              ${CMAKE_CURRENT_SOURCE_DIR}/docs/Variable-Trace-User-Guide.html
        DESTINATION ${INSTALL_PATH})
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stlink/chips DESTINATION ${INSTALL_PATH})

if(WIN32)
    if(PRODUCTION)
        set_target_properties(${EXECUTABLE} PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()

    install(FILES ${LIBUSB_WINDOWS} ${GLFW3_WINDOWS} ${GDB_WINDOWS} ${JLINK_WINDOWS} ${LIBDWARF_RUNTIME_DLLS} DESTINATION ${INSTALL_PATH})
    set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS "
    SetOutPath \\\"$INSTDIR\\\\bin\\\"
    CreateShortCut \\\"$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\${EXECUTABLE}.lnk\\\" \\\"$INSTDIR\\\\bin\\\\${EXECUTABLE}.exe\\\"
    CreateShortCut \\\"$DESKTOP\\\\${EXECUTABLE}.lnk\\\" \\\"$INSTDIR\\\\bin\\\\${EXECUTABLE}.exe\\\"")
    set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS "
    Delete \\\"$SMPROGRAMS\\\\$START_MENU\\\\${EXECUTABLE}.lnk\\\"
    Delete \\\"$DESKTOP\\\\${EXECUTABLE}.lnk\\\"")
endif()

if(UNIX)
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/launch/install/Unix/icon.png DESTINATION ${INSTALL_PATH})
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/launch/install/Unix/Variable-Trace.desktop
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
        DESTINATION ${DESKTOP_FILE_PATH})
    install(FILES ${JLINK_LINUX}
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
        DESTINATION ${LIB_INSTALL_PATH})
    install(FILES ${LIB_CONF_FILE}
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
        DESTINATION ${LIB_CONF_FILE_PATH})
    set(CPACK_GENERATOR "DEB;RPM")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libglfw3 | libglfw3-wayland, libgl1, libglib2.0-0, libgtk-3-0, libstdc++6, libusb-1.0-0, gdb")
endif()

set(CPACK_PACKAGE_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${VARIABLE_TRACE_VERSION})
set(CPACK_PACKAGE_VERSION ${CPACK_PROJECT_VERSION})
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE)
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Piotr Wasilewski <piwasilewski@interia.pl>")
set(CPACK_PACKAGE_VENDOR "klonyyy")
set(CPACK_PACKAGE_DESCRIPTION "STM32 no-overhead realtime variable viewer.")
set(CPACK_NSIS_CREATE_UNINSTALLER ON)
set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
set(CPACK_NSIS_MODIFY_PATH ON)
set(CPACK_ALL_INSTALL_TYPES Full Developer)
set(CPACK_COMPONENT_APPLICATIONS_INSTALL_TYPES Full)
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "${PROJECT_SOURCE_DIR}/launch/install/Unix/postinst")
include(CPack)
