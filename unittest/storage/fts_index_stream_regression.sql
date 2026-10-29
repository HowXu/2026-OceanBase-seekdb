# owner group: storage
# tags: fts
# description: compare populated-table index builds, document boundaries and DML.
set names utf8mb4;
create table ft_build_stream_regression (
  id bigint primary key,
  title varchar(255),
  body text,
  english text
) default charset=utf8mb4;
insert into ft_build_stream_regression
select n, concat('数据库 database ', n),
  case when mod(n, 19) = 0 then null
       when mod(n, 17) = 0 then ''
       when mod(n, 13) = 0 then repeat('数据库 数据库 全文索引 中文 ＡＢＣ 😀 ', 300)
       when mod(n, 11) = 0 then 'the THE and for'
       else concat('全文索引 中文 database audit ', n) end,
  case when mod(n, 19) = 0 then null
       when mod(n, 17) = 0 then ''
       when mod(n, 13) = 0 then repeat('Database DATABASE audit common performance ', 300)
       when mod(n, 11) = 0 then 'the THE and for'
       else concat('Database audit common performance row ', n) end
from (
  select a.n + 10 * b.n + 100 * c.n n
  from (select 0 n union all select 1 union all select 2 union all select 3 union all select 4
        union all select 5 union all select 6 union all select 7 union all select 8 union all select 9) a
  cross join (select 0 n union all select 1 union all select 2 union all select 3 union all select 4
        union all select 5 union all select 6 union all select 7 union all select 8 union all select 9) b
  cross join (select 0 n union all select 1 union all select 2 union all select 3 union all select 4
        union all select 5 union all select 6 union all select 7 union all select 8 union all select 9) c
) seeds;
alter table ft_build_stream_regression add fulltext index ft_cn(title, body) with parser ik;
alter table ft_build_stream_regression add fulltext index ft_en(english) with parser beng;

select id, match(title, body) against('数据库 全文索引 中文 database') score
from ft_build_stream_regression
where match(title, body) against('数据库 全文索引 中文 database') order by id;
select id, match(english) against('database audit common performance') score
from ft_build_stream_regression
where match(english) against('database audit common performance') order by id;
select id, match(english) against('+database -audit' in boolean mode) score
from ft_build_stream_regression
where match(english) against('+database -audit' in boolean mode) order by id;

update ft_build_stream_regression set body = '更新 数据库 rareword rareword', english = 'rareword rareword database'
where mod(id, 17) = 1;
delete from ft_build_stream_regression where mod(id, 19) = 2;
insert into ft_build_stream_regression values (1001, '数据库', '数据库 rareword', 'database rareword');
select id, match(title, body) against('数据库 rareword') score
from ft_build_stream_regression where match(title, body) against('数据库 rareword') order by id;
select id, match(english) against('database rareword') score
from ft_build_stream_regression where match(english) against('database rareword') order by id;
drop table ft_build_stream_regression;
