# Generated by devtools/yamaker from nixpkgs 24.05.

LIBRARY()

LICENSE(
    BSL-1.0 AND
    CC0-1.0
)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

VERSION(1.88.0)

ORIGINAL_SOURCE(https://github.com/boostorg/container/archive/boost-1.88.0.tar.gz)

PEERDIR(
    contrib/restricted/boost/assert
    contrib/restricted/boost/config
    contrib/restricted/boost/intrusive
    contrib/restricted/boost/move
)

ADDINCL(
    GLOBAL contrib/restricted/boost/container/include
)

NO_COMPILER_WARNINGS()

NO_UTIL()

IF (DYNAMIC_BOOST)
    CFLAGS(
        GLOBAL -DBOOST_CONTAINER_DYN_LINK
    )
ENDIF()

SRCS(
    src/alloc_lib.c
    src/dlmalloc.cpp
    src/global_resource.cpp
    src/monotonic_buffer_resource.cpp
    src/pool_resource.cpp
    src/synchronized_pool_resource.cpp
    src/unsynchronized_pool_resource.cpp
)

END()
