cmake_minimum_required(VERSION 3.10)

# プロジェクト名と使用する言語を設定
project(media-receiver C CXX)

set(CMAKE_CXX_STANDARD 17)


# pkg-config を使ってモジュールを読み込む
find_package(PkgConfig)

# gstreamer-1.0 の存在チェック
pkg_check_modules(GSTREAMER REQUIRED 
  glib-2.0
  json-glib-1.0
  libsoup-2.4
  gstreamer-1.0 
  gstreamer-sdp-1.0
  gstreamer-rtp-1.0
  gstreamer-webrtc-1.0)

# gstreamer のヘッダーファイルへのパスを表示
message("GSTREAMER_INCLUDE_DIRS: ${GSTREAMER_INCLUDE_DIRS}")

# gstreamer のライブラリへのパスを表示
message("GSTREAMER_LIBRARY_DIRS: ${GSTREAMER_LIBRARY_DIRS}")


# gstreamermm のライブラリのパスを設定
link_directories(${GSTREAMER_LIBRARY_DIRS})

# コンパイルフラグを設定
#set(CMAKE_C_FLAGS "-Wall")
#set(CMAKE_C_FLAGS_DEBUG "-g3 -O0 -pg")
#set(CMAKE_C_FLAGS_RELEASE "-O2 -s -DNDEBUG -march=native")
#set(CMAKE_C_FLAGS_RELWITHDEBINFO "-g3 -Og -pg")
#set(CMAKE_C_FLAGS_MINSIZEREL "-Os -s DNDEBUG -march=native")

#set(CMAKE_CXX_FLAGS "-Wall -std=c++17")
#set(CMAKE_CXX_FLAGS_DEBUG "-g -DDEBUG_BUILD")
#set(CMAKE_CXX_FLAGS_RELEASE "-O2 -s -DNDEBUG -march=native")
#set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-g3 -Og -pg")
#set(CMAKE_CXX_FLAGS_MINSIZEREL "-Os -s DNDEBUG -march=native")

# 実行ファイルの作成
add_executable(media-receiver 
  src/main.cpp src/http.cpp src/http.h)

# gstreamer ヘッダーへのパスを設定
target_include_directories(media-receiver  PUBLIC ${GSTREAMER_INCLUDE_DIRS})

# gstreamermm ライブラリの設定
target_link_libraries(media-receiver  ${GSTREAMER_LIBRARIES} )
#target_link_libraries(media-receiver  pthread)

# gstreamer のコンパイルオプションを設定
target_compile_options(media-receiver  PUBLIC ${GSTREAMER_CFLAGS_OTHER})
