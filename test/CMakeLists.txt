# googletest release-1.11.0 declares a cmake_minimum_required below 3.5,
# which CMake 4.x refuses outright, so the policy floor has to be lowered here
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

include(FetchContent)

project(Variable-Trace_test)

set(EXECUTABLE ${CMAKE_PROJECT_NAME})

set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_CXX_STANDARD 20)

enable_testing()

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG release-1.11.0
)
FetchContent_MakeAvailable(googletest)

add_library(GTest::GTest INTERFACE IMPORTED)

include_directories(${EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/src/
    ${CMAKE_SOURCE_DIR}/src/Plot
    ${CMAKE_SOURCE_DIR}/src/PlotHandler
    ${CMAKE_SOURCE_DIR}/src/PlotGroupHandler
    ${CMAKE_SOURCE_DIR}/src/ProjectHandler
    ${CMAKE_SOURCE_DIR}/src/ElfReader
    ${CMAKE_SOURCE_DIR}/src/Variable
    ${CMAKE_SOURCE_DIR}/src/ScrollingBuffer
    ${CMAKE_SOURCE_DIR}/src/RingBuffer
    ${CMAKE_SOURCE_DIR}/src/TraceReader
    ${CMAKE_SOURCE_DIR}/src/Statistics
    ${CMAKE_SOURCE_DIR}/src/ElfParser
    ${CMAKE_SOURCE_DIR}/src/Variable
    ${CMAKE_SOURCE_DIR}/src/VariableHandler
    ${CMAKE_SOURCE_DIR}/src/McpServer
    ${CMAKE_SOURCE_DIR}/src/DataHandler
    ${CMAKE_SOURCE_DIR}/src/MemoryReader
    ${CMAKE_SOURCE_DIR}/src/Recorder
    ${CMAKE_SOURCE_DIR}/src/MovingAverage
    ${CMAKE_SOURCE_DIR}/src/Flashing
    ${CMAKE_SOURCE_DIR}/src/WritePlanner
    ${CMAKE_SOURCE_DIR}/src/CSVStreamer)

include_directories(${EXECUTABLE} SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/spdlog/inc
    ${CMAKE_SOURCE_DIR}/third_party/json
    ${CMAKE_SOURCE_DIR}/third_party/imgui
    ${CMAKE_SOURCE_DIR}/third_party/implot
    ${CMAKE_SOURCE_DIR}/third_party/http
    # The acquisition data handler includes its probe headers, so the vendor
    # headers have to be reachable even though no probe is built here.
    ${CMAKE_SOURCE_DIR}/third_party/jlink/inc
    ${CMAKE_SOURCE_DIR}/third_party/stlink/inc
    ${CMAKE_SOURCE_DIR}/third_party/libusb/inc/libusb-1.0)

set(SOURCES
    ${CMAKE_SOURCE_DIR}/src/commons.cpp
    ${CMAKE_SOURCE_DIR}/src/TraceReader/TraceReader.cpp
    ${CMAKE_SOURCE_DIR}/src/Variable/Variable.cpp
    ${CMAKE_SOURCE_DIR}/src/VariableHandler/VariableHandler.cpp
    ${CMAKE_SOURCE_DIR}/src/Plot/Plot.cpp
    ${CMAKE_SOURCE_DIR}/src/PlotHandler/PlotHandler.cpp
    ${CMAKE_SOURCE_DIR}/src/ProjectHandler/ProjectHandler.cpp
    ${CMAKE_SOURCE_DIR}/src/ProjectHandler/GlobalConfig.cpp
    # The programmer runs a real child process, so the test binary exercises the
    # same process handling the application uses.
    ${CMAKE_SOURCE_DIR}/src/Flashing/FlashingService.cpp
    ${CMAKE_SOURCE_DIR}/src/WritePlanner/WritePlanner.cpp
    ${CMAKE_SOURCE_DIR}/src/CSVStreamer/CSVStreamer.cpp
    ${CMAKE_SOURCE_DIR}/src/MovingAverage/MovingAverage.cpp
    ${CMAKE_SOURCE_DIR}/src/DataHandler/ViewerDataHandler.cpp
    # The symbol readers. The DWARF one is built here as well, with the library
    # itself only reached inside the guard, so that the mapping from an encoding
    # to a type is exercised by the same code the application runs.
    ${CMAKE_SOURCE_DIR}/src/ElfParser/IElfParser.cpp
    ${CMAKE_SOURCE_DIR}/src/ElfParser/GdbParser.cpp
    ${CMAKE_SOURCE_DIR}/src/ElfParser/ElfParserFactory.cpp
    ${CMAKE_SOURCE_DIR}/src/ElfParser/ElfTarget.cpp
    ${CMAKE_SOURCE_DIR}/src/ElfParser/DwarfDecode.cpp
    ${CMAKE_SOURCE_DIR}/src/ElfParser/LibDwarfParser.cpp
    ${CMAKE_SOURCE_DIR}/src/ElfParser/OfdDwarfXml.cpp
    ${CMAKE_SOURCE_DIR}/src/ElfParser/TiOfdParser.cpp
    ${CMAKE_SOURCE_DIR}/src/McpServer/McpTypes.cpp
    ${CMAKE_SOURCE_DIR}/src/McpServer/McpProtocol.cpp
    ${CMAKE_SOURCE_DIR}/src/McpServer/McpToolRegistry.cpp
    ${CMAKE_SOURCE_DIR}/src/McpServer/McpToolHelpers.cpp
    ${CMAKE_SOURCE_DIR}/src/McpServer/McpBridge.cpp
    # The server itself is built here as well, so the tool table and the request
    # handling can be checked without opening a socket.
    ${CMAKE_SOURCE_DIR}/src/McpServer/McpServer.cpp
    ${CMAKE_SOURCE_DIR}/src/McpServer/ProjectTools.cpp
    ${CMAKE_SOURCE_DIR}/src/McpServer/GroupTools.cpp
    ${CMAKE_SOURCE_DIR}/src/McpServer/PlotTools.cpp
    ${CMAKE_SOURCE_DIR}/src/McpServer/VariableTools.cpp
    ${CMAKE_SOURCE_DIR}/src/McpServer/RecorderTools.cpp
    ${CMAKE_SOURCE_DIR}/src/McpServer/AcquisitionTools.cpp
    ${CMAKE_SOURCE_DIR}/src/McpServer/DataTools.cpp
    ${CMAKE_SOURCE_DIR}/src/McpServer/FlashingTools.cpp
    ${CMAKE_SOURCE_DIR}/src/MemoryReader/SerialProtocol.cpp
    # The probe builds an operating system port in its one argument constructor,
    # and the recorder test takes the other one, so the port has to be linkable
    # even though no test opens a real one.
    ${CMAKE_SOURCE_DIR}/src/MemoryReader/SerialPort.cpp
    ${CMAKE_SOURCE_DIR}/src/MemoryReader/SerialDebugProbe.cpp
    ${CMAKE_SOURCE_DIR}/src/Recorder/RecorderLayout.cpp
    ${CMAKE_SOURCE_DIR}/src/Recorder/RecorderHandler.cpp
    ${CMAKE_SOURCE_DIR}/src/MemoryReader/SerialDriverLayout.cpp)

target_link_libraries(GTest::GTest INTERFACE gtest_main gmock gmock_main)

add_executable(${EXECUTABLE} main.cpp
    ScrollingBufferTest.cpp
    RingBufferTest.cpp
    TraceReaderTest.cpp
    StatisticsTest.cpp
    GdbParserTest.cpp
    ElfTargetTest.cpp
    DwarfDecodeTest.cpp
    OfdDwarfXmlTest.cpp
    TiOfdParserTest.cpp
    SerialDriverLayoutTest.cpp
    VariableTest.cpp
    ProjectHandlerTest.cpp
    WritePlannerTest.cpp
    FlashingServiceTest.cpp
    FlashingToolsTest.cpp
    McpServerTest.cpp
    SerialProtocolTest.cpp
    RecorderHandlerTest.cpp
    ${SOURCES})

add_compile_options(-Wall -Wextra -Wpedantic)

target_link_libraries(${EXECUTABLE}
    PRIVATE
    GTest::GTest)

# The HTTP library is header only, but it does use the socket calls.
if(WIN32)
    target_link_libraries(${EXECUTABLE} PRIVATE ws2_32)
endif()

# Copy test elf file to binary directory
set(SOURCE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/testFiles/Variable-Trace_test.elf")
set(DESTINATION_DIR "${CMAKE_CURRENT_BINARY_DIR}")

add_custom_command(
    TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SOURCE_FILE}"
        "${DESTINATION_DIR}"
    COMMENT "Copying file to executable directory"
)

add_custom_target(copy_file ALL
    DEPENDS ${SOURCE_FILE}
)

add_dependencies(${EXECUTABLE} copy_file)