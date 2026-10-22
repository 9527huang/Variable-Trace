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
	${CMAKE_SOURCE_DIR}/src/Gui
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
    ${CMAKE_SOURCE_DIR}/src/Rtos
    ${CMAKE_SOURCE_DIR}/src/Rtos/Trace
    ${CMAKE_SOURCE_DIR}/src/Variable
    ${CMAKE_SOURCE_DIR}/src/VariableHandler
    ${CMAKE_SOURCE_DIR}/src/McpServer
    ${CMAKE_SOURCE_DIR}/src/DataHandler
    ${CMAKE_SOURCE_DIR}/src/MemoryReader
    ${CMAKE_SOURCE_DIR}/src/Recorder
    ${CMAKE_SOURCE_DIR}/src/MovingAverage
    ${CMAKE_SOURCE_DIR}/src/Flashing
    ${CMAKE_SOURCE_DIR}/src/WritePlanner
    ${CMAKE_SOURCE_DIR}/src/Screenshot
    ${CMAKE_SOURCE_DIR}/src/I18n
    ${CMAKE_SOURCE_DIR}/src/FileHandler
    ${CMAKE_SOURCE_DIR}/src/CSVStreamer)

include_directories(${EXECUTABLE} SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/spdlog/inc
    ${CMAKE_SOURCE_DIR}/third_party/json
    ${CMAKE_SOURCE_DIR}/third_party/imgui
    ${CMAKE_SOURCE_DIR}/third_party/implot
    ${CMAKE_SOURCE_DIR}/third_party/http
    ${CMAKE_SOURCE_DIR}/third_party/stb
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
    # Both are free of OpenGL, so the naming rules and the PNG writer are
    # exercised here even though the capture itself needs a window.
    ${CMAKE_SOURCE_DIR}/src/Screenshot/PlotExport.cpp
    ${CMAKE_SOURCE_DIR}/src/Screenshot/PngWriter.cpp
    ${CMAKE_SOURCE_DIR}/src/CSVStreamer/CSVStreamer.cpp
    ${CMAKE_SOURCE_DIR}/src/MovingAverage/MovingAverage.cpp
    ${CMAKE_SOURCE_DIR}/src/DataHandler/ViewerDataHandler.cpp
    # The trace side is here for its state flag: whether it is running is one of
    # the conditions the project tools ask about.
    ${CMAKE_SOURCE_DIR}/src/DataHandler/TraceDataHandler.cpp
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
    # The kernel table is a data structure and four lookups, so it is built here
    # whole: what it decides about a missing symbol cannot be tested against the
    # application's own copy without dragging the application in.
    ${CMAKE_SOURCE_DIR}/src/Rtos/RtosProfileBase.cpp
    ${CMAKE_SOURCE_DIR}/src/Rtos/FreeRtosProfile.cpp
    ${CMAKE_SOURCE_DIR}/src/Rtos/RtosFactory.cpp
    ${CMAKE_SOURCE_DIR}/src/Rtos/RtosListWalker.cpp
    ${CMAKE_SOURCE_DIR}/src/Rtos/RtosTaskEnumerator.cpp
    ${CMAKE_SOURCE_DIR}/src/Rtos/RtosModel.cpp
    ${CMAKE_SOURCE_DIR}/src/Rtos/RtosSampler.cpp
    ${CMAKE_SOURCE_DIR}/src/Rtos/GdbRtosLayout.cpp
    ${CMAKE_SOURCE_DIR}/src/Rtos/RtosSyncEnumerator.cpp
    ${CMAKE_SOURCE_DIR}/src/Rtos/RtosTableModel.cpp
    ${CMAKE_SOURCE_DIR}/src/Rtos/RtosTaskRoster.cpp
    ${CMAKE_SOURCE_DIR}/src/Rtos/RtosStackScanner.cpp
    ${CMAKE_SOURCE_DIR}/src/Rtos/RtosStackSweep.cpp
    ${CMAKE_SOURCE_DIR}/src/Rtos/RtosCpuLoad.cpp
    ${CMAKE_SOURCE_DIR}/src/Rtos/RtosDiagnostics.cpp
    ${CMAKE_SOURCE_DIR}/src/Rtos/RtosWaitGraph.cpp
    ${CMAKE_SOURCE_DIR}/src/Rtos/Trace/RtosTraceTypes.cpp
    ${CMAKE_SOURCE_DIR}/src/Rtos/Trace/RtosTraceClock.cpp
    ${CMAKE_SOURCE_DIR}/src/Rtos/Trace/RtosIdentityStore.cpp
    ${CMAKE_SOURCE_DIR}/src/Rtos/Trace/RtosTracePipeline.cpp
    ${CMAKE_SOURCE_DIR}/src/Rtos/RtosEventRing.cpp
    ${CMAKE_SOURCE_DIR}/src/Rtos/RtosCsvExport.cpp
    ${CMAKE_SOURCE_DIR}/src/Rtos/RtosItmDecoder.cpp
    ${CMAKE_SOURCE_DIR}/src/Rtos/RtosEventHistory.cpp
    ${CMAKE_SOURCE_DIR}/src/Rtos/RtosEventQuery.cpp
    ${CMAKE_SOURCE_DIR}/src/Rtos/RtosTimeline.cpp
    ${CMAKE_SOURCE_DIR}/src/Rtos/RtosRecording.cpp
    ${CMAKE_SOURCE_DIR}/src/McpServer/McpTypes.cpp
    ${CMAKE_SOURCE_DIR}/src/McpServer/McpProtocol.cpp
    ${CMAKE_SOURCE_DIR}/src/McpServer/McpToolRegistry.cpp
    ${CMAKE_SOURCE_DIR}/src/McpServer/McpToolHelpers.cpp
    ${CMAKE_SOURCE_DIR}/src/McpServer/McpBridge.cpp
    # The server itself is built here as well, so the tool table and the request
    # handling can be checked without opening a socket.
    ${CMAKE_SOURCE_DIR}/src/McpServer/McpServer.cpp
    ${CMAKE_SOURCE_DIR}/src/McpServer/CommandServer.cpp
    ${CMAKE_SOURCE_DIR}/src/McpServer/ProjectTools.cpp
    ${CMAKE_SOURCE_DIR}/src/McpServer/GroupTools.cpp
    ${CMAKE_SOURCE_DIR}/src/McpServer/PlotTools.cpp
    ${CMAKE_SOURCE_DIR}/src/McpServer/VariableTools.cpp
    ${CMAKE_SOURCE_DIR}/src/McpServer/RecorderTools.cpp
    ${CMAKE_SOURCE_DIR}/src/McpServer/AcquisitionTools.cpp
    ${CMAKE_SOURCE_DIR}/src/McpServer/DataTools.cpp
    ${CMAKE_SOURCE_DIR}/src/McpServer/FlashingTools.cpp
    ${CMAKE_SOURCE_DIR}/src/McpServer/RtosTools.cpp
    ${CMAKE_SOURCE_DIR}/src/MemoryReader/SerialProtocol.cpp
    # The probe builds an operating system port in its one argument constructor,
    # and the recorder test takes the other one, so the port has to be linkable
    # even though no test opens a real one.
    ${CMAKE_SOURCE_DIR}/src/MemoryReader/SerialPort.cpp
    ${CMAKE_SOURCE_DIR}/src/MemoryReader/SerialDebugProbe.cpp
    ${CMAKE_SOURCE_DIR}/src/MemoryReader/GdbServerDebugProbe.cpp
    ${CMAKE_SOURCE_DIR}/src/Recorder/RecorderLayout.cpp
    ${CMAKE_SOURCE_DIR}/src/Recorder/RecorderHandler.cpp
    # The string table the interface draws from. Nothing in it needs a window,
    # so the whole of it is checked here.
    ${CMAKE_SOURCE_DIR}/src/I18n/I18n.cpp
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
    VariableHandlerTest.cpp
    ViewerDataHandlerTest.cpp
    ViewerDataHandlerPerformanceTest.cpp
	GuiPerformanceTest.cpp
	PerformanceDiagnosisTest.cpp
    CSVStreamerTest.cpp
    PlotTest.cpp
    PlotCopyTest.cpp
    PlotHandlerTest.cpp
    PlotGroupCopyTest.cpp
    I18nTest.cpp
    ProjectHandlerTest.cpp
    WritePlannerTest.cpp
    PlotCsvExportTest.cpp
    PlotExportTest.cpp
    PngWriterTest.cpp
    FlashingServiceTest.cpp
    FlashingToolsTest.cpp
    ProjectToolsTest.cpp
    McpServerTest.cpp
    CommandServerTest.cpp
    SerialProtocolTest.cpp
    GdbServerDebugProbeTest.cpp
    DebugProbeTypesTest.cpp
    RecorderHandlerTest.cpp
    RecorderToolsTest.cpp
    FreeRtosProfileTest.cpp
    RtosTaskEnumerationTest.cpp
    RtosModelTest.cpp
    RtosSamplerTest.cpp
    GdbRtosLayoutTest.cpp
    PlotDerivedSeriesTest.cpp
    RtosTableModelTest.cpp
    RtosSyncEnumerationTest.cpp
    RtosStackScannerTest.cpp
    RtosStackSweepTest.cpp
    RtosCpuLoadTest.cpp
    RtosDiagnosticsTest.cpp
    RtosWaitGraphTest.cpp
    RtosTraceTypesTest.cpp
    RtosTraceClockTest.cpp
    RtosIdentityStoreTest.cpp
    RtosTracePipelineTest.cpp
    RtosEventRingTest.cpp
    RtosItmDecoderTest.cpp
    RtosEventHistoryTest.cpp
    RtosEventQueryTest.cpp
    RtosCsvExportTest.cpp
    RtosTimelineTest.cpp
    RtosRecordingTest.cpp
    RtosTaskRosterTest.cpp
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
