cmake_minimum_required(VERSION 3.0)

find_path(SCSSDK_INC scssdk.h
  HINTS "C:/Users/Silas/Downloads/2016/scs_sdk_1_5/include"
)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(ETS2_HINT_PATH "C:/Program Files (x86)/Steam/SteamApps/common/Euro Truck Simulator 2/bin/win_x64")
else()
  set(ETS2_HINT_PATH "C:/Program Files (x86)/Steam/SteamApps/common/Euro Truck Simulator 2/bin/win_x86")
endif()

find_path(ETS2_PATH eurotrucks2.exe
  HINTS "${ETS2_HINT_PATH}"
)

include_directories("${SCSSDK_INC}")

add_library(ets2_dash_v2_plugin SHARED
  cJSON.h
  cJSON.c
  plugin.cpp
  plugin.def
)

target_link_libraries(ets2_dash_v2_plugin
  Ws2_32
)

install(TARGETS ets2_dash_v2_plugin
  DESTINATION "${ETS2_PATH}/plugins"
  CONFIGURATIONS Release
)