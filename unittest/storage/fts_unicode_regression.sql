# owner group: storage
# tags: fts
# description: preserve IK tokenization and MATCH results across Unicode character classes.

set names utf8mb4;

select tokenize('OceanBase 数据库 全文索引 ABC xyz 123', 'ik') as tokens;
select tokenize('ＡＢＣ ｘｙｚ １２３ abc.def@example.com 3.14 -42', 'ik') as tokens;
select tokenize('中文、ひらがな カタカナ 한글 😀 𠀀 é', 'ik') as tokens;
select tokenize(_utf8mb4'中文 ＡＢＣ１２３ 😀' collate utf8mb4_bin, 'ik') as tokens;
select tokenize(cast(concat('ABC', char(0), '中文 😀') as char character set utf8mb4), 'ik') as tokens;
select tokenize('，。！？；：、—（）【】　\t\n', 'ik') as tokens;
select tokenize('中华人民共和国人民大会堂 ＡＢＣ１２３ 😀', 'ik', '[{"additional_args": [{"ik_mode": "smart"}]}]') as tokens;
select tokenize('中华人民共和国人民大会堂 ＡＢＣ１２３ 😀', 'ik', '[{"additional_args": [{"ik_mode": "max_word"}]}]') as tokens;
select tokenize(cast(repeat('中文 OceanBase１２３ 😀，', 100) as char character set utf8mb4), 'ik', '[{"output": "all"}]') as tokens;

drop table if exists ft_ik_decode_regression;

create table ft_ik_decode_regression (
  id int primary key,
  body varchar(1024),
  fulltext index ft_body (body) with parser ik
) default charset=utf8mb4;

insert into ft_ik_decode_regression values
  (1, 'OceanBase 数据库 全文索引 ABC 123'),
  (2, '中文数据库 ＡＢＣ１２３ 😀'),
  (3, 'ひらがな カタカナ 한글'),
  (4, '电子邮件 abc.def@example.com 3.14'),
  (5, '中华人民共和国人民大会堂');

select id from ft_ik_decode_regression where match(body) against('数据库' in boolean mode) order by id;
select id from ft_ik_decode_regression where match(body) against('OceanBase' in boolean mode) order by id;
select id from ft_ik_decode_regression where match(body) against('人民大会堂' in boolean mode) order by id;
select id from ft_ik_decode_regression where match(body) against('电子邮件' in boolean mode) order by id;
select id from ft_ik_decode_regression where match(body) against('ひらがな 한글' in boolean mode) order by id;

select id, match(body) against('数据库 全文索引' in natural language mode) as score
from ft_ik_decode_regression
where match(body) against('数据库 全文索引' in natural language mode)
order by score desc, id;

select id, match(body) against('OceanBase ABC' in natural language mode) as score
from ft_ik_decode_regression
where match(body) against('OceanBase ABC' in natural language mode)
order by score desc, id;

drop table ft_ik_decode_regression;

# Exercise document ID merge order, equal scores, NULL text and batch boundaries.
create table ft_merge_id_regression (
  id bigint primary key,
  body varchar(255),
  fulltext index ft_body (body) with parser beng
);

insert into ft_merge_id_regression
select n, case when mod(n, 10) = 9 then null else
  case mod(n, 4)
    when 0 then 'database database audit common'
    when 1 then 'database performance common'
    when 2 then 'audit performance common'
    else 'database audit performance common'
  end end
from (
  select a.n + 10 * b.n + 100 * c.n as n
  from (select 0 n union all select 1 union all select 2 union all select 3 union all select 4
        union all select 5 union all select 6 union all select 7 union all select 8 union all select 9) a
  cross join (select 0 n union all select 1 union all select 2 union all select 3 union all select 4
        union all select 5 union all select 6 union all select 7 union all select 8 union all select 9) b
  cross join (select 0 n union all select 1 union all select 2 union all select 3 union all select 4
        union all select 5 union all select 6 union all select 7 union all select 8 union all select 9) c
) seeds;

select id, match(body) against('database audit performance common') as score
from ft_merge_id_regression where match(body) against('database audit performance common')
order by id;

select id, match(body) against('+database -audit' in boolean mode) as score
from ft_merge_id_regression where match(body) against('+database -audit' in boolean mode)
order by id;

select id, match(body) against('database database audit common' in boolean mode) as score
from ft_merge_id_regression where match(body) against('database database audit common' in boolean mode)
order by id;

select id, match(body) against('database audit performance common') as score
from ft_merge_id_regression where match(body) against('database audit performance common')
order by score desc, id limit 257 offset 249;

drop table ft_merge_id_regression;
