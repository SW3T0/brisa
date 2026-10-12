cmake_minimum_required(VERSION 3.20)
project(Brisa VERSION 0.3.1 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "Este MVP utiliza Win32 y necesita Windows x64.")
endif()

add_executable(Brisa WIN32 src/main.cpp)
target_compile_features(Brisa PRIVATE cxx_std_17)
target_include_directories(Brisa PRIVATE vendor/pdfium/include)
target_link_libraries(Brisa PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/vendor/pdfium/lib/pdfium.dll.lib"
  user32
  gdi32
  comdlg32
  shell32
  psapi
  gdiplus
)

if(MSVC)
  target_compile_options(Brisa PRIVATE /utf-8 /O2)
else()
  target_link_options(Brisa PRIVATE -municode)
endif()

add_custom_command(TARGET Brisa POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/pdfium.dll"
    "$<TARGET_FILE_DIR:Brisa>/pdfium.dll"
)
