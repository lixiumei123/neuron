set(CMAKE_SYSTEM_NAME Linux)
set(COMPILER_PREFIX x86_64-linux-gnu)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(LIBRARY_DIR /home/neuron/libs)

set(CMAKE_C_COMPILER ${COMPILER_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${COMPILER_PREFIX}-g++)
set(CMAKE_AR ${COMPILER_PREFIX}-ar)
set(CMAKE_LINKER ${COMPILER_PREFIX}-ld)
set(CMAKE_NM ${COMPILER_PREFIX}-nm)
set(CMAKE_OBJDUMP ${COMPILER_PREFIX}-objdump)
set(CMAKE_RANLIB ${COMPILER_PREFIX}-ranlib)
set(CMAKE_STAGING_PREFIX ${LIBRARY_DIR}/${COMPILER_PREFIX})
set(CMAKE_PREFIX_PATH ${CMAKE_STAGING_PREFIX})

include_directories(SYSTEM ${CMAKE_STAGING_PREFIX}/include)
include_directories(SYSTEM ${CMAKE_STAGING_PREFIX}/openssl/include)
set(CMAKE_FIND_ROOT_PATH ${CMAKE_STAGING_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
link_directories(${CMAKE_STAGING_PREFIX})

file(COPY ${CMAKE_STAGING_PREFIX}/lib/libzlog.so.1.2 DESTINATION /usr/local/lib)
