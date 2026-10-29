# owner group: storage
# tags: fts
# description: preserve score filtering, projection and repeated scans.
set names utf8mb4;
create table ft_score_filter_regression (
  id int primary key,
  body varchar(255),
  fulltext index ft_body(body) with parser beng
);
insert into ft_score_filter_regression
select n, case mod(n, 6)
  when 0 then 'database database audit common'
  when 1 then 'database performance common'
  when 2 then 'audit performance common'
  when 3 then 'common'
  when 4 then ''
  else null end
from (
  select a.n + 10 * b.n + 100 * c.n n
  from (select 0 n union all select 1 union all select 2 union all select 3 union all select 4
        union all select 5 union all select 6 union all select 7 union all select 8 union all select 9) a
  cross join (select 0 n union all select 1 union all select 2 union all select 3 union all select 4
        union all select 5 union all select 6 union all select 7 union all select 8 union all select 9) b
  cross join (select 0 n union all select 1 union all select 2 union all select 3 union all select 4
        union all select 5 union all select 6 union all select 7 union all select 8 union all select 9) c
) seeds;

select id, match(body) against('database audit') score,
  cast(match(body) against('database audit') as signed) rounded_score
from ft_score_filter_regression where match(body) against('database audit') order by id;
select id, match(body) against('database audit') score
from ft_score_filter_regression where match(body) against('database audit') > 0.001 order by id;
select id, match(body) against('database audit') score
from ft_score_filter_regression where match(body) against('database audit') = 0 order by id;
select id, match(body) against('database audit') score
from ft_score_filter_regression where not match(body) against('database audit') order by id;
select id, match(body) against('database audit') score
from ft_score_filter_regression where match(body) against('database audit') and mod(id, 3) = 1 order by id;
select id, match(body) against('+database -audit' in boolean mode) score
from ft_score_filter_regression where match(body) against('+database -audit' in boolean mode) order by id;
select id, match(body) against('common') score
from ft_score_filter_regression where match(body) against('common') order by id limit 257 offset 249;
select count(*) from ft_score_filter_regression where match(body) against('absentword');
select id, match(body) against('database audit') score
from ft_score_filter_regression order by score desc, id limit 20;

prepare ft_score_reuse from 'select count(*), sum(match(body) against(?)) from ft_score_filter_regression where match(body) against(?)';
set @ft_query = 'database audit';
execute ft_score_reuse using @ft_query, @ft_query;
set @ft_query = 'performance';
execute ft_score_reuse using @ft_query, @ft_query;
set @ft_query = 'absentword';
execute ft_score_reuse using @ft_query, @ft_query;
set @ft_query = 'database audit performance common';
execute ft_score_reuse using @ft_query, @ft_query;
deallocate prepare ft_score_reuse;
drop table ft_score_filter_regression;
