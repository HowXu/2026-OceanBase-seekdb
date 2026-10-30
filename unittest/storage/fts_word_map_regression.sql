# owner group: storage
# tags: fts
# description: Private word maps preserve collation grouping, frequencies and indexed results.
set names utf8mb4;
select tokenize('alpha ALPHA alpha beta BETA gamma', 'beng', '[{"output": "all"}]');
select tokenize(_utf8mb4'café CAFE café cafe résumé RESUME' collate utf8mb4_general_ci, 'beng', '[{"output": "all"}]');
select tokenize(_utf8mb4'café CAFE café cafe résumé RESUME' collate utf8mb4_bin, 'beng', '[{"output": "all"}]');
select tokenize('the THE and AND alpha alpha', 'beng', '[{"output": "all"}]');
select tokenize('', 'beng', '[{"output": "all"}]');
select tokenize(null, 'beng');
select tokenize(cast(repeat('alpha beta ALPHA café cafe gamma ', 100) as char character set utf8mb4), 'beng', '[{"output": "all"}]');
select tokenize('数据库 数据库 全文索引 全文索引 OceanBase OceanBase', 'ik', '[{"output": "all"}, {"additional_args": [{"ik_mode": "smart"}]}]');
select tokenize('数据库 数据库 全文索引 全文索引 OceanBase OceanBase', 'ik', '[{"output": "all"}, {"additional_args": [{"ik_mode": "max_word"}]}]');

drop table if exists ft_word_map_regression;
create table ft_word_map_regression (
  id int primary key,
  body varchar(1024),
  body_bin varchar(1024) collate utf8mb4_bin,
  body_ik varchar(1024)
) default charset=utf8mb4;
insert into ft_word_map_regression values
  (1, 'alpha alpha ALPHA beta café', 'alpha alpha ALPHA beta café', '数据库 数据库 全文索引'),
  (2, 'alpha gamma cafe', 'alpha gamma cafe', '数据库 分词器'),
  (3, 'beta gamma delta', 'beta gamma delta', '倒排索引 分词器'),
  (4, null, null, null),
  (5, '', '', ''),
  (6, 'epsilon zeta security', 'epsilon zeta security', '中文 搜索 查询');
alter table ft_word_map_regression add fulltext index ft_body(body) with parser beng;
alter table ft_word_map_regression add fulltext index ft_body_bin(body_bin) with parser beng;
alter table ft_word_map_regression add fulltext index ft_body_ik(body_ik) with parser ik;
select id, round(match(body) against('alpha beta'), 8) from ft_word_map_regression where match(body) against('alpha beta') order by id;
select id from ft_word_map_regression where match(body) against('cafe') order by id;
select id from ft_word_map_regression where match(body_bin) against('cafe') order by id;
select id from ft_word_map_regression where match(body) against('+alpha -gamma' in boolean mode) order by id;
select id from ft_word_map_regression where match(body_ik) against('数据库 分词器') order by id;
update ft_word_map_regression set body = 'gamma gamma gamma alpha', body_bin = 'gamma gamma gamma alpha', body_ik = '全文索引 全文索引' where id = 3;
delete from ft_word_map_regression where id = 1;
select id from ft_word_map_regression where match(body) against('alpha') order by id;
select id from ft_word_map_regression where match(body_bin) against('cafe') order by id;
select id from ft_word_map_regression where match(body_ik) against('全文索引') order by id;
drop table ft_word_map_regression;
