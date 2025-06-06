# Generated by devtools/yamaker/ym2

LIBRARY()

LICENSE(Apache-2.0)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

VERSION(2025-01-30)

ORIGINAL_SOURCE(https://github.com/google/tcmalloc/archive/c8dfee3e4c489c5ae0d30c484c92db102a69ec51.tar.gz)

NO_COMPILER_WARNINGS()

SRCS(
    # Options
    tcmalloc/want_hpaa.cc
)

INCLUDE(common.inc)

CFLAGS(
    -DTCMALLOC_256K_PAGES
)

END()

IF (NOT DLL_FOR)
    RECURSE(
        default
        dynamic
        malloc_extension
        no_percpu_cache
        numa_256k
        numa_large_pages
        profile_marshaller
        small_but_slow
        tcmalloc/internal
    )
ENDIF()
