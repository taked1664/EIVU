cmake_minimum_required(VERSION 3.11)
project(EIVU)
set(CMAKE_CXX_STANDARD 17)

set(obj "./util/shm.cpp")
set(flags -O3 -march=x86-64 -std=c++17 -pthread -mtune=generic -Wall -Wextra -ffast-math -mfpmath=both -g)
#set(flags -O3 -march=native -std=c++17 -pthread -mtune=native -Wall -Wextra -ffast-math -mfpmath=both -g)

add_executable(rx.out rx.cpp ${obj})
target_link_libraries(rx.out PUBLIC rt)
target_include_directories(rx.out PUBLIC util)
target_compile_options(rx.out PRIVATE ${flags})

add_executable(tx.out tx.cpp ${obj})
target_link_libraries(tx.out PUBLIC rt)
target_include_directories(tx.out PUBLIC util)
target_compile_options(tx.out PRIVATE ${flags})

add_executable(nf.out nf.cpp ${obj})
target_link_libraries(nf.out PUBLIC rt)
target_include_directories(nf.out PUBLIC util)
target_compile_options(nf.out PRIVATE ${flags})

file(COPY "${CMAKE_SOURCE_DIR}/shells/run.sh" DESTINATION ${CMAKE_BINARY_DIR})
file(COPY "${CMAKE_SOURCE_DIR}/shells/statrun.sh" DESTINATION ${CMAKE_BINARY_DIR})

