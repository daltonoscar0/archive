cmake_minimum_required(VERSION 3.16)
project(orderflow LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

add_library(orderflow_core
    src/parser/lobster_parser.cpp
    src/book/book.cpp
    src/tokenizer/tokenizer.cpp
)
target_include_directories(orderflow_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(validate_book src/tools/validate_book.cpp)
target_link_libraries(validate_book PRIVATE orderflow_core)

add_executable(book_source_compare src/tools/book_source_compare.cpp)
target_link_libraries(book_source_compare PRIVATE orderflow_core)

add_executable(tokenizer_fit src/tokenizer/fit_main.cpp)
target_link_libraries(tokenizer_fit PRIVATE orderflow_core)

add_executable(tokenize src/tokenizer/tokenize_main.cpp)
target_link_libraries(tokenize PRIVATE orderflow_core)

add_executable(token_roundtrip src/tokenizer/roundtrip_main.cpp)
target_link_libraries(token_roundtrip PRIVATE orderflow_core)

enable_testing()
set(FIXTURE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures)

add_executable(test_parser tests/test_parser.cpp)
target_link_libraries(test_parser PRIVATE orderflow_core)
add_test(NAME parser COMMAND test_parser ${FIXTURE_DIR})

add_executable(test_book tests/test_book.cpp)
target_link_libraries(test_book PRIVATE orderflow_core)
add_test(NAME book COMMAND test_book ${FIXTURE_DIR})

add_executable(test_validate tests/test_validate.cpp)
target_link_libraries(test_validate PRIVATE orderflow_core)
target_include_directories(test_validate PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/tools)
add_test(NAME validate_core COMMAND test_validate)

# Tests are assert-based; Release's -DNDEBUG must not silence them.
target_compile_options(test_parser PRIVATE -UNDEBUG)
target_compile_options(test_book PRIVATE -UNDEBUG)
target_compile_options(test_validate PRIVATE -UNDEBUG)

add_executable(test_tokenizer tests/test_tokenizer.cpp)
target_link_libraries(test_tokenizer PRIVATE orderflow_core)
target_include_directories(test_tokenizer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/tokenizer)
target_compile_options(test_tokenizer PRIVATE -UNDEBUG)
add_test(NAME tokenizer COMMAND test_tokenizer ${FIXTURE_DIR})

add_test(NAME validate_fixture
    COMMAND validate_book ${FIXTURE_DIR}/tiny_messages.csv
                          ${FIXTURE_DIR}/tiny_orderbook.csv)
add_test(NAME validate_fixture_strict_depth
    COMMAND validate_book ${FIXTURE_DIR}/tiny_messages.csv
                          ${FIXTURE_DIR}/tiny_orderbook.csv 2)
add_test(NAME validate_detects_divergence
    COMMAND validate_book ${FIXTURE_DIR}/tiny_messages.csv
                          ${FIXTURE_DIR}/tiny_orderbook_bad.csv)
set_tests_properties(validate_detects_divergence PROPERTIES WILL_FAIL TRUE)
# A mismatch confined to the deepest level is a warning, not a failure
# (LOBSTER omits events outside its price window). Default K = levels - 1.
add_test(NAME validate_boundary_only_warns
    COMMAND validate_book ${FIXTURE_DIR}/tiny_messages.csv
                          ${FIXTURE_DIR}/tiny_orderbook_boundary_bad.csv)
# Fully strict (K = levels): the same boundary mismatch becomes fatal.
add_test(NAME validate_boundary_fatal_when_strict
    COMMAND validate_book ${FIXTURE_DIR}/tiny_messages.csv
                          ${FIXTURE_DIR}/tiny_orderbook_boundary_bad.csv 3)
set_tests_properties(validate_boundary_fatal_when_strict PROPERTIES WILL_FAIL TRUE)
# --summary-only suppresses detail tables but must not change exit semantics.
add_test(NAME validate_summary_only
    COMMAND validate_book --summary-only ${FIXTURE_DIR}/tiny_messages.csv
                          ${FIXTURE_DIR}/tiny_orderbook_bad.csv)
set_tests_properties(validate_summary_only PROPERTIES WILL_FAIL TRUE)
add_test(NAME book_source_compare_smoke
    COMMAND book_source_compare ${FIXTURE_DIR}/tiny_messages.csv
                                ${FIXTURE_DIR}/tiny_orderbook.csv 2)
# Full tokenizer pipeline over the fixture: fit -> tokenize -> round-trip.
add_test(NAME tokenizer_fit_fixture
    COMMAND tokenizer_fit ${FIXTURE_DIR}/tiny_messages.csv --ticker TINY
            --manifest tok_manifest.json --train-frac 0.8)
set_tests_properties(tokenizer_fit_fixture PROPERTIES
    FIXTURES_SETUP tok_manifest)
add_test(NAME tokenize_fixture
    COMMAND tokenize ${FIXTURE_DIR}/tiny_messages.csv
            ${FIXTURE_DIR}/tiny_orderbook.csv --ticker TINY
            --manifest tok_manifest.json -o tiny_tokens.bin)
set_tests_properties(tokenize_fixture PROPERTIES
    FIXTURES_SETUP tok_tokens FIXTURES_REQUIRED tok_manifest)
add_test(NAME token_roundtrip_fixture
    COMMAND token_roundtrip tiny_tokens.bin tok_manifest.json)
set_tests_properties(token_roundtrip_fixture PROPERTIES
    FIXTURES_REQUIRED "tok_manifest;tok_tokens")
# Structural guard: tokenize must refuse to run without fitted bins.
add_test(NAME tokenize_requires_fit
    COMMAND tokenize ${FIXTURE_DIR}/tiny_messages.csv
            ${FIXTURE_DIR}/tiny_orderbook.csv --ticker NOFIT
            --manifest tok_manifest.json -o nofit_tokens.bin)
set_tests_properties(tokenize_requires_fit PROPERTIES
    FIXTURES_REQUIRED tok_manifest WILL_FAIL TRUE)
