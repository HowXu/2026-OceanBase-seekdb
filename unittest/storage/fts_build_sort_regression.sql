# owner group: storage
# tags: fts
# description: Encoded FTS build sorting preserves collation, composite keys, MATCH and DML.
set names utf8mb4;
drop table if exists fts_build_sort;
create table fts_build_sort (
  id bigint primary key,
  ci_text varchar(1024) collate utf8mb4_general_ci,
  bin_text varchar(1024) collate utf8mb4_bin,
  body text collate utf8mb4_general_ci
) default charset=utf8mb4;
insert into fts_build_sort values
  (-5, null, null, null),
  (0, '', '', ''),
  (2, 'cafe CAFE café résumé resume', 'Alpha alpha BETA beta', '中华人民共和国 人民大会堂 数据库'),
  (7, 'CAFÉ resume résumé', 'beta Alpha alpha', '南京市 长江大桥 数据库'),
  (42, 'a', 'a', '研究生命起源 全文索引'),
  (3, 'A', 'A', '数据库 数据库'),
  (19, 'á', 'á', '中华人民共和国 全文索引'),
  (11, 'a ', 'a ', '人民大会堂'),
  (6, ' a', ' a', '南京市长江大桥'),
  (23, 'a  ', 'a  ', '全文索引 数据库'),
  (9007199254740993, '中文 😀 𠀀', '中文 😀 𠀀', '中文 数据库 全文索引');
insert into fts_build_sort values
  (15, cast(concat('a', char(0), 'b') as char character set utf8mb4), cast(concat('a', char(0), 'b') as char character set utf8mb4), '数据库'),
  (17, cast(repeat('é', 128) as char character set utf8mb4), cast(repeat('x', 256) as char character set utf8mb4), '人民大会堂 数据库');
select 'ci-asc-raw';
select /*+ opt_param('enable_newsort', 'false') */ id, concat('0x', hex(ci_text)) from fts_build_sort order by ci_text, id;
select 'ci-asc-encoded';
select /*+ opt_param('enable_newsort', 'true') */ id, concat('0x', hex(ci_text)) from fts_build_sort order by ci_text, id;
select 'bin-desc-raw';
select /*+ opt_param('enable_newsort', 'false') */ id, concat('0x', hex(bin_text)) from fts_build_sort order by bin_text desc, id desc;
select 'bin-desc-encoded';
select /*+ opt_param('enable_newsort', 'true') */ id, concat('0x', hex(bin_text)) from fts_build_sort order by bin_text desc, id desc;
alter table fts_build_sort add fulltext index fts_ci(ci_text, body) with parser ik;
alter table fts_build_sort add fulltext index fts_bin(bin_text) with parser beng;
select id, round(match(ci_text, body) against('数据库 全文索引'), 8) from fts_build_sort where match(ci_text, body) against('数据库 全文索引') order by id;
select id, round(match(bin_text) against('alpha beta'), 8) from fts_build_sort where match(bin_text) against('alpha beta') order by id;
select id, round(match(ci_text, body) against('南京市 长江大桥'), 8) from fts_build_sort where match(ci_text, body) against('南京市 长江大桥') order by id;
update fts_build_sort set ci_text='CAFÉ 数据库', body='全文索引', bin_text='alpha beta alpha' where id=42;
delete from fts_build_sort where id=7;
insert into fts_build_sort values (100, '数据库 数据库', 'BETA alpha', '全文索引');
select id, round(match(ci_text, body) against('数据库 全文索引'), 8) from fts_build_sort where match(ci_text, body) against('数据库 全文索引') order by id;
select id, round(match(bin_text) against('alpha beta'), 8) from fts_build_sort where match(bin_text) against('alpha beta') order by id;
drop table fts_build_sort;
drop table if exists fts_build_sort_fallback;
create table fts_build_sort_fallback(id int primary key, txt varchar(512) collate utf8mb4_unicode_ci) default charset=utf8mb4;
insert into fts_build_sort_fallback values (1, 'CAFÉ alpha alpha'), (2, 'cafe BETA'), (3, null);
alter table fts_build_sort_fallback add fulltext index ft(txt) with parser beng;
select id, round(match(txt) against('alpha beta'), 8) from fts_build_sort_fallback where match(txt) against('alpha beta') order by id;
drop table fts_build_sort_fallback;
