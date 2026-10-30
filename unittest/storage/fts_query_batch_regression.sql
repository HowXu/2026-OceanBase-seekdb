# owner group: storage
# tags: fts
# description: DAAT/BMW iteration preserves scoring, filtering, batching, LIMIT and rescans.
set names utf8mb4;
set group_concat_max_len=1048576;
drop table if exists fts_query_batch;
create table fts_query_batch(id int primary key, title varchar(128), body text, en text) default charset=utf8mb4;
insert into fts_query_batch
select n,
  case n % 3 when 0 then '数据库' when 1 then '全文索引' else '中华人民共和国' end,
  case n % 5 when 0 then null when 1 then '数据库 全文索引 数据库' when 2 then '人民大会堂 数据库' when 3 then '全文索引' else '南京市 长江大桥' end,
  case n % 4 when 0 then null when 1 then 'alpha alpha beta' when 2 then 'beta gamma' else 'alpha gamma delta' end
from (
  select a.n + 10*b.n + 100*c.n + 1000*d.n as n
  from (select 0 n union all select 1 union all select 2 union all select 3 union all select 4 union all select 5 union all select 6 union all select 7 union all select 8 union all select 9) a
  cross join (select 0 n union all select 1 union all select 2 union all select 3 union all select 4 union all select 5 union all select 6 union all select 7 union all select 8 union all select 9) b
  cross join (select 0 n union all select 1 union all select 2 union all select 3 union all select 4 union all select 5 union all select 6 union all select 7 union all select 8 union all select 9) c
  cross join (select 0 n union all select 1 union all select 2) d
) numbers where n <= 2050;
alter table fts_query_batch add fulltext index ft_cn(title, body) with parser ik;
alter table fts_query_batch add fulltext index ft_en(en) with parser beng;
select count(*), sum(id), round(sum(match(title, body) against('数据库')), 8) from fts_query_batch where match(title, body) against('数据库');
select group_concat(concat(id, ':', round(match(en) against('alpha beta gamma'), 8)) order by id) from fts_query_batch where match(en) against('alpha beta gamma');
select id, round(match(title, body) against('数据库 数据库 全文索引'), 8) as score from fts_query_batch where match(title, body) against('数据库 数据库 全文索引') order by score desc, id limit 8;
select id, round(match(title, body) against('+数据库 -全文索引' in boolean mode), 8) as score from fts_query_batch where match(title, body) against('+数据库 -全文索引' in boolean mode) order by score desc, id limit 8;
select id, round(match(en) against('alpha beta'), 8) as score from fts_query_batch where match(en) against('alpha beta') > 0.5 order by score desc, id limit 7 offset 3;
select id, round(match(en) against('alpha beta'), 8) as score from fts_query_batch where match(en) against('alpha beta') > 0 and match(en) against('alpha beta') < 0.5 order by id limit 7;
select id, round(match(en) against('alpha'), 8) from fts_query_batch where id < 8 order by id;
select count(*) from fts_query_batch where match(en) against('no_such_word_zzzz');
select count(*) from fts_query_batch where match(en) against('');
select count(*) from fts_query_batch where match(en) against('the');
prepare fts_query_reuse from 'select count(*), sum(id) from fts_query_batch where match(en) against(?)';
set @query_words='alpha';
execute fts_query_reuse using @query_words;
set @query_words='beta gamma';
execute fts_query_reuse using @query_words;
set @query_words='';
execute fts_query_reuse using @query_words;
deallocate prepare fts_query_reuse;
select count(*) from fts_query_batch where match(en) against('alpha beta');
select count(*) from fts_query_batch where match(en) against('alpha beta') > 1.0e300;
select id, round(match(en) against('alpha beta'), 8) as score from fts_query_batch where match(en) against('alpha beta') > 0.5 order by id limit 17;
select count(*) from fts_query_batch where match(title, body) against('数据库') > 0;
prepare fts_query_filter_reuse from 'select count(*) from fts_query_batch where match(en) against(?) > 0';
set @query_words='alpha';
execute fts_query_filter_reuse using @query_words;
set @query_words='no_such_word_zzzz';
execute fts_query_filter_reuse using @query_words;
deallocate prepare fts_query_filter_reuse;
drop table if exists fts_query_seek;
create table fts_query_seek(id int primary key, en text) default charset=utf8mb4;
insert into fts_query_seek
select id, concat('denseword ',
  if(id % 2 = 0, 'evenword ', ''),
  if(id % 31 = 0, 'sparseword ', ''),
  if(id % 257 = 0, 'rareword rareword ', ''),
  if(id % 7 = 0, 'rankword rankword rankword', 'rankword'))
from fts_query_batch;
alter table fts_query_seek add fulltext index ft_seek(en) with parser beng;
select id, round(match(en) against('denseword sparseword rareword rankword'), 8) as score
from fts_query_seek where match(en) against('denseword sparseword rareword rankword')
order by score desc, id limit 1;
select id, round(match(en) against('denseword sparseword rareword rankword'), 8) as score
from fts_query_seek where match(en) against('denseword sparseword rareword rankword')
order by score desc, id limit 20;
select id, round(match(en) against('denseword evenword sparseword rareword'), 8) as score
from fts_query_seek where match(en) against('denseword evenword sparseword rareword') > 0
order by score desc, id limit 7 offset 31;
select group_concat(concat(id, ':', score) order by score desc, id)
from (select id, round(match(en) against('denseword sparseword rareword rankword'), 8) as score
from fts_query_seek where match(en) against('denseword sparseword rareword rankword')
order by score desc, id limit 1027) r;
prepare fts_seek_reuse from 'select id, round(match(en) against(?), 8) as score from fts_query_seek where match(en) against(?) order by score desc, id limit 5';
set @seek_words='denseword sparseword';
execute fts_seek_reuse using @seek_words, @seek_words;
set @seek_words='no_such_seek_word';
execute fts_seek_reuse using @seek_words, @seek_words;
deallocate prepare fts_seek_reuse;
delete from fts_query_seek where id in (0, 31, 1024, 2048);
update fts_query_seek set en='denseword rareword rareword rankword' where id=1025;
select id, round(match(en) against('denseword sparseword rareword rankword'), 8) as score
from fts_query_seek where match(en) against('denseword sparseword rareword rankword')
order by score desc, id limit 20;
drop table fts_query_seek;
update fts_query_batch set title='数据库 数据库', body='全文索引', en='gamma gamma alpha' where id=0;
delete from fts_query_batch where id in (1, 1024, 2048);
insert into fts_query_batch values(2051, '全文索引', '数据库', 'beta alpha');
select count(*), sum(id), round(sum(match(title, body) against('数据库 全文索引')), 8) from fts_query_batch where match(title, body) against('数据库 全文索引');
select id, round(match(en) against('alpha beta gamma'), 8) as score from fts_query_batch where match(en) against('alpha beta gamma') order by score desc, id limit 10 offset 1020;
drop table fts_query_batch;
