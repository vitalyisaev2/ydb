/* postgres can not */
/* custom error:Cannot infer common type for Int64 and Uint64*/
PRAGMA warning('error', '1107');

SELECT
    AsList(
        7498311229109140978,
        254610204336699107,
        11580367904009864964
    )
;
