cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Set VITASDK to your VitaSDK, or build with ./build.sh (Docker)")
  endif()
endif()

project(macapad C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Macapad")
set(VITA_TITLEID "MCPD07317")
# APP_VER is two numbers, XX.YY; the release workflow passes the next one in
if(NOT DEFINED VITA_VERSION)
  set(VITA_VERSION "01.00")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -Wall -Wno-format-truncation -D_GNU_SOURCE")

# jansson, built here (see vendor/jansson/VENDORED.md)
file(GLOB JANSSON_SOURCES vendor/jansson/*.c)
add_library(jansson_vendored STATIC ${JANSSON_SOURCES})
target_include_directories(jansson_vendored PUBLIC vendor/jansson)
target_compile_definitions(jansson_vendored PRIVATE HAVE_CONFIG_H)
target_compile_options(jansson_vendored PRIVATE -w)

add_executable(${PROJECT_NAME}
  src/main.c
  src/macapad.c
  src/http.c
  src/gfx.c
  src/qrscan.c
  src/sys.c
)

target_link_libraries(${PROJECT_NAME}
  vita2d
  freetype
  bz2
  png
  jpeg
  jansson_vendored
  quirc
  z
  m
  SceAppMgr_stub
  SceAppUtil_stub
  SceCamera_stub
  SceCommonDialog_stub
  SceCtrl_stub
  SceDisplay_stub
  SceGxm_stub
  SceIme_stub
  SceNet_stub
  SceNetCtl_stub
  ScePgf_stub
  ScePower_stub
  ScePvf_stub
  SceSysmodule_stub
  SceTouch_stub
)

# UNSAFE: the app keeps its files in ux0:data/macapad, outside the sandbox safe homebrew gets
vita_create_self(eboot.bin ${PROJECT_NAME} UNSAFE)
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  FILE assets/fonts/Inter-Regular.ttf assets/fonts/Inter-Regular.ttf
  FILE assets/fonts/Inter-SemiBold.ttf assets/fonts/Inter-SemiBold.ttf
)
