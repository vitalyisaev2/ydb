YQL_UDF_CONTRIB(string_udf)
    
    YQL_ABI_VERSION(
        2
        43
        0
    )
    
    SRCS(
        string_udf.cpp
    )
    
    PEERDIR(
        yql/essentials/public/langver
        yql/essentials/public/udf/arrow
        library/cpp/deprecated/split
        library/cpp/html/pcdata
        library/cpp/string_utils/base32
        library/cpp/string_utils/base64
        library/cpp/string_utils/levenshtein_diff
        library/cpp/string_utils/quote
    )
    
    END()

RECURSE_FOR_TESTS(
    test
)


