cmake_minimum_required(VERSION 3.4.1)
include_directories(crypto curve25519 ed25519 playfair plist/plist)


aux_source_directory(. play_src)
set(DIR_SRCS ${play_src})

add_library( play-lib
        SHARED
        ${DIR_SRCS}
        )
find_library( log-lib
        log )
target_link_libraries( play-lib
        crypto
        curve25519
        ed25519
        playfair
        plist
        avcodec
        avutil
        ${log-lib})
