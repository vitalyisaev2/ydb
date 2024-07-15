--
-- BIT types
--
--
-- Build tables for testing
--
CREATE TABLE BIT_TABLE(b BIT(11));
INSERT INTO BIT_TABLE VALUES (B'00000000000');
INSERT INTO BIT_TABLE VALUES (B'11011000000');
INSERT INTO BIT_TABLE VALUES (B'01010101010');
CREATE TABLE VARBIT_TABLE(v BIT VARYING(11));
INSERT INTO VARBIT_TABLE VALUES (B'');
INSERT INTO VARBIT_TABLE VALUES (B'0');
INSERT INTO VARBIT_TABLE VALUES (B'010101');
INSERT INTO VARBIT_TABLE VALUES (B'01010101010');
-- test overflow cases
SELECT SUBSTRING('01010101'::bit(8) FROM 2 FOR 2147483646) AS "1010101";
SELECT SUBSTRING('01010101'::bit(8) FROM -10 FOR 2147483646) AS "01010101";
SELECT SUBSTRING('01010101'::bit(8) FROM -10 FOR -2147483646) AS "error";
SELECT SUBSTRING('01010101'::varbit FROM 2 FOR 2147483646) AS "1010101";
SELECT SUBSTRING('01010101'::varbit FROM -10 FOR 2147483646) AS "01010101";
SELECT SUBSTRING('01010101'::varbit FROM -10 FOR -2147483646) AS "error";
CREATE TABLE varbit_table (a BIT VARYING(16), b BIT VARYING(16));
DROP TABLE varbit_table;
CREATE TABLE bit_table (a BIT(16), b BIT(16));
DROP TABLE bit_table;
-- The following should fail
select B'001' & B'10';
select B'0111' | B'011';
select B'0010' # B'011101';
-- More position tests, checking all the boundary cases
SELECT POSITION(B'1010' IN B'0000101');   -- 0
SELECT POSITION(B'1010' IN B'00001010');  -- 5
SELECT POSITION(B'1010' IN B'00000101');  -- 0
SELECT POSITION(B'1010' IN B'000001010');  -- 6
SELECT POSITION(B'' IN B'00001010');  -- 1
SELECT POSITION(B'0' IN B'');  -- 0
SELECT POSITION(B'' IN B'');  -- 0
SELECT POSITION(B'101101' IN B'001011011011011000');  -- 3
SELECT POSITION(B'10110110' IN B'001011011011010');  -- 3
SELECT POSITION(B'1011011011011' IN B'001011011011011');  -- 3
SELECT POSITION(B'1011011011011' IN B'00001011011011011');  -- 5
SELECT POSITION(B'11101011' IN B'11101011'); -- 1
SELECT POSITION(B'11101011' IN B'011101011'); -- 2
SELECT POSITION(B'11101011' IN B'00011101011'); -- 4
SELECT POSITION(B'11101011' IN B'0000011101011'); -- 6
SELECT POSITION(B'111010110' IN B'111010110'); -- 1
SELECT POSITION(B'111010110' IN B'0111010110'); -- 2
SELECT POSITION(B'111010110' IN B'000111010110'); -- 4
SELECT POSITION(B'111010110' IN B'00000111010110'); -- 6
SELECT POSITION(B'111010110' IN B'11101011'); -- 0
SELECT POSITION(B'111010110' IN B'011101011'); -- 0
SELECT POSITION(B'111010110' IN B'00011101011'); -- 0
SELECT POSITION(B'111010110' IN B'0000011101011'); -- 0
SELECT POSITION(B'111010110' IN B'111010110'); -- 1
SELECT POSITION(B'111010110' IN B'0111010110'); -- 2
SELECT POSITION(B'111010110' IN B'000111010110'); -- 4
SELECT POSITION(B'111010110' IN B'00000111010110'); -- 6
SELECT POSITION(B'111010110' IN B'000001110101111101011'); -- 0
SELECT POSITION(B'111010110' IN B'0000001110101111101011'); -- 0
SELECT POSITION(B'111010110' IN B'000000001110101111101011'); -- 0
SELECT POSITION(B'111010110' IN B'00000000001110101111101011'); -- 0
SELECT POSITION(B'111010110' IN B'0000011101011111010110'); -- 14
SELECT POSITION(B'111010110' IN B'00000011101011111010110'); -- 15
SELECT POSITION(B'111010110' IN B'0000000011101011111010110'); -- 17
SELECT POSITION(B'111010110' IN B'000000000011101011111010110'); -- 19
SELECT POSITION(B'000000000011101011111010110' IN B'000000000011101011111010110'); -- 1
SELECT POSITION(B'00000000011101011111010110' IN B'000000000011101011111010110'); -- 2
SELECT POSITION(B'0000000000011101011111010110' IN B'000000000011101011111010110'); -- 0
-- Shifting
CREATE TABLE BIT_SHIFT_TABLE(b BIT(16));
INSERT INTO BIT_SHIFT_TABLE VALUES (B'1101100000000000');
INSERT INTO BIT_SHIFT_TABLE SELECT b>>1 FROM BIT_SHIFT_TABLE;
INSERT INTO BIT_SHIFT_TABLE SELECT b>>2 FROM BIT_SHIFT_TABLE;
INSERT INTO BIT_SHIFT_TABLE SELECT b>>4 FROM BIT_SHIFT_TABLE;
INSERT INTO BIT_SHIFT_TABLE SELECT b>>8 FROM BIT_SHIFT_TABLE;
CREATE TABLE VARBIT_SHIFT_TABLE(v BIT VARYING(20));
INSERT INTO VARBIT_SHIFT_TABLE VALUES (B'11011');
INSERT INTO VARBIT_SHIFT_TABLE SELECT CAST(v || B'0' AS BIT VARYING(6)) >>1 FROM VARBIT_SHIFT_TABLE;
INSERT INTO VARBIT_SHIFT_TABLE SELECT CAST(v || B'00' AS BIT VARYING(8)) >>2 FROM VARBIT_SHIFT_TABLE;
INSERT INTO VARBIT_SHIFT_TABLE SELECT CAST(v || B'0000' AS BIT VARYING(12)) >>4 FROM VARBIT_SHIFT_TABLE;
INSERT INTO VARBIT_SHIFT_TABLE SELECT CAST(v || B'00000000' AS BIT VARYING(20)) >>8 FROM VARBIT_SHIFT_TABLE;
DROP TABLE BIT_SHIFT_TABLE;
DROP TABLE VARBIT_SHIFT_TABLE;
-- Get/Set bit
SELECT get_bit(B'0101011000100', 10);
SELECT set_bit(B'0101011000100100', 15, 1);
SELECT set_bit(B'0101011000100100', 16, 1);	-- fail
-- Overlay
SELECT overlay(B'0101011100' placing '001' from 2 for 3);
SELECT overlay(B'0101011100' placing '101' from 6);
SELECT overlay(B'0101011100' placing '001' from 11);
SELECT overlay(B'0101011100' placing '001' from 20);
-- bit_count
SELECT bit_count(B'0101011100'::bit(10));
SELECT bit_count(B'1111111111'::bit(10));
-- This table is intentionally left around to exercise pg_dump/pg_upgrade
CREATE TABLE bit_defaults(
  b1 bit(4) DEFAULT '1001',
  b2 bit(4) DEFAULT B'0101',
  b3 bit varying(5) DEFAULT '1001',
  b4 bit varying(5) DEFAULT B'0101'
);