cmake_minimum_required(VERSION 3.20)
project(GPTAssembly VERSION 2.0.0 LANGUAGES C CXX)

if(NOT MINGW OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "This build targets Windows x64 with MinGW-w64.")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

add_library(assembly STATIC src/language.cpp src/format.cpp src/pe_reader.cpp)
target_compile_options(assembly PRIVATE -Wall -Wextra -Wpedantic -finput-charset=UTF-8 -fexec-charset=UTF-8)

add_library(gptrt SHARED src/runtime.cpp)
set_target_properties(gptrt PROPERTIES PREFIX "")
target_link_libraries(gptrt PRIVATE assembly psapi)
target_link_options(gptrt PRIVATE -static)

set(lua_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua54")
file(GLOB lua_sources CONFIGURE_DEPENDS "${lua_dir}/*.c")
list(REMOVE_ITEM lua_sources "${lua_dir}/lua.c" "${lua_dir}/luac.c")
add_library(lua_compiler STATIC ${lua_sources})
target_include_directories(lua_compiler PUBLIC "${lua_dir}")
add_library(lua54 SHARED ${lua_sources} src/lua_literals.c)
target_include_directories(lua54 PUBLIC "${lua_dir}")
target_compile_definitions(lua54 PUBLIC LUA_BUILD_AS_DLL)
target_link_options(lua54 PRIVATE -static-libgcc)
set_target_properties(lua54 PROPERTIES PREFIX "")

add_library(lua_prose STATIC src/lua_prose.cpp src/lua_package.cpp src/feedback.cpp)
target_include_directories(lua_prose PRIVATE "${lua_dir}")
target_link_libraries(lua_prose PUBLIC assembly)
target_compile_options(lua_prose PRIVATE -Wall -Wextra -Wpedantic -fexec-charset=UTF-8)
add_library(gptcompat SHARED src/lua_runtime.cpp)
set_target_properties(gptcompat PROPERTIES PREFIX "")
target_link_libraries(gptcompat PRIVATE lua_prose lua54 shell32 psapi)
target_link_options(gptcompat PRIVATE -static)
add_library(gptlua54 SHARED src/prose_host.cpp src/prose_vm.cpp src/prose_library.cpp)
target_include_directories(gptlua54 PRIVATE "${lua_dir}")
set_target_properties(gptlua54 PROPERTIES PREFIX "")
target_link_libraries(gptlua54 PRIVATE lua_prose shell32 psapi)
target_link_options(gptlua54 PRIVATE -static)
add_executable(lua-host "${lua_dir}/lua.c")
target_link_libraries(lua-host PRIVATE lua54)
target_link_options(lua-host PRIVATE -static-libgcc)

add_executable(gptc src/cli.cpp src/frontend.cpp src/pe_writer.cpp src/lua_frontend.cpp)
target_link_libraries(gptc PRIVATE lua_prose lua_compiler)
target_compile_options(gptc PRIVATE -Wall -Wextra -Wpedantic)
target_link_options(gptc PRIVATE -municode -static)

add_executable(sample-host examples/native_host.c)
target_link_options(sample-host PRIVATE -municode -static)

foreach(example IN ITEMS sample caveats)
    set(module "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${example}.dll")
    add_custom_command(OUTPUT "${module}"
        COMMAND $<TARGET_FILE:gptc> compile "${CMAKE_CURRENT_SOURCE_DIR}/examples/${example}.c" -o "${module}"
        DEPENDS gptc "examples/${example}.c" VERBATIM)
    add_custom_target(${example}_module ALL DEPENDS "${module}")
endforeach()

set(game_root "${CMAKE_CURRENT_SOURCE_DIR}/examples/tic-tac-toing")
file(GLOB_RECURSE game_sources CONFIGURE_DEPENDS "${game_root}/*.lua")
foreach(extension IN ITEMS exe dll)
    set(game "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/TicTacToing.${extension}")
    add_custom_command(OUTPUT "${game}" COMMAND $<TARGET_FILE:gptc> compile
        "${game_root}/tic-tac-toing.lua" --root "${game_root}" -o "${game}"
        DEPENDS gptc ${game_sources} VERBATIM)
    add_custom_target(game_${extension} ALL DEPENDS "${game}")
endforeach()

include(CTest)
install(TARGETS gptc sample-host gptrt RUNTIME DESTINATION .)
install(FILES "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/sample.dll"
              "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/caveats.dll" DESTINATION .)
install(FILES "${lua_dir}/LICENSE.txt" DESTINATION . RENAME LICENSE-Lua.txt)
install(FILES "$<TARGET_FILE:lua54>" "$<TARGET_FILE:gptcompat>" DESTINATION LuaHost)
install(FILES "${lua_dir}/LICENSE.txt" DESTINATION LuaHost RENAME LICENSE-Lua.txt)
install(FILES "$<TARGET_FILE:gptlua54>"
              "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/TicTacToing.exe"
              "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/TicTacToing.dll"
              "${game_root}/PLAY.txt" DESTINATION TicTacToing)
install(FILES "${game_root}/LICENSE" DESTINATION TicTacToing RENAME LICENSE-game.txt)
install(FILES "${lua_dir}/LICENSE.txt" DESTINATION TicTacToing RENAME LICENSE-Lua.txt)
if(BUILD_TESTING)
    add_executable(lua-opcodes-test tests/lua_opcodes.cpp)
    target_link_libraries(lua-opcodes-test PRIVATE lua_prose)
    target_include_directories(lua-opcodes-test PRIVATE "${lua_dir}")
    target_link_options(lua-opcodes-test PRIVATE -static)
    add_test(NAME lua_opcodes COMMAND lua-opcodes-test)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME independent_vm COMMAND "${Python3_EXECUTABLE}" -B
                 "${CMAKE_CURRENT_SOURCE_DIR}/tests/independent_vm.py" "$<TARGET_FILE_DIR:gptc>")
        set_tests_properties(independent_vm PROPERTIES TIMEOUT 60)
        set(GPT_LUA_TEST_SUITE "" CACHE PATH "Extracted upstream lua-5.4.9-tests directory")
        if(GPT_LUA_TEST_SUITE)
            add_test(NAME lua_official_basic
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua_official.py"
                        "$<TARGET_FILE_DIR:gptc>" "${GPT_LUA_TEST_SUITE}")
        endif()
        add_test(NAME lua_acceptance
            COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/lua_acceptance.py"
                    "$<TARGET_FILE_DIR:gptc>" "${CMAKE_C_COMPILER}" "$<TARGET_LINKER_FILE:lua54>")
        add_test(NAME assembly_acceptance
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/acceptance.py"
                    "$<TARGET_FILE_DIR:gptc>" --cc "${CMAKE_C_COMPILER}" --cxx "${CMAKE_CXX_COMPILER}")
    endif()
endif()
