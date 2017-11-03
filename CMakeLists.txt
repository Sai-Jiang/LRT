cmake_minimum_required(VERSION 3.7)
project(LLRTP)

set(CMAKE_C_STANDARD 99)

set(SOURCE_FILES include/LLRTP.h src/LLRTP.c)
set(INClUDE_DIR ./include)
set(LIB_DIR ./lib)

#set(CMAKE_C_FLAGS " -g ${CMAKE_CXX_FLAGS}")
#set(CMAKE_CXX_FLAGS " -fsanitize=address -g ${CMAKE_CXX_FLAGS}")

include_directories(${INClUDE_DIR})
link_directories(${LIB_DIR})

link_libraries(kodoc)

add_executable(Sender ${SOURCE_FILES} src/Tx.c test/TestSender.c)
add_executable(Receiver ${SOURCE_FILES} src/Rx.c test/TestReceiver.c)

target_link_libraries(Sender kodoc pthread)
target_link_libraries(Receiver kodoc pthread)