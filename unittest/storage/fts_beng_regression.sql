# owner group: storage
# tags: fts
# description: BEng normalized tokens must survive iterator advancement and destruction.
set names utf8mb4;
select tokenize('DATABASE performance AUDIT database indexing tokenizer', 'beng');
select tokenize('A a AB ab ABC abc ABCD abcd punctuation.email@example.com 123', 'beng');
select tokenize('中文 DATABASE ＡＢＣ performance café audit 😀', 'beng');
select tokenize(_utf8mb4'ABC xyz 0123 I i' collate utf8mb4_bin, 'beng');
select tokenize(cast(concat('ABC', char(0), 'DEF', char(127), 'ghi') as char character set utf8mb4), 'beng');
select tokenize(' ,.!?\t\r\n', 'beng');
select tokenize(cast(char(0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127) as char character set utf8mb4), 'beng');
select tokenize(cast(repeat('DATABASE audit performance indexing tokenizer ', 100) as char character set utf8mb4), 'beng');

drop table if exists ft_beng_lifetime_regression;
create table ft_beng_lifetime_regression (
  id int primary key,
  body varchar(1024)
) default charset=utf8mb4;
insert into ft_beng_lifetime_regression values
  (1, 'DATABASE performance AUDIT database indexing tokenizer'),
  (2, 'audit AUDIT audit performance'),
  (3, 'tokenizer indexing security review'),
  (4, '中文 DATABASE café performance 😀'),
  (5, null);
alter table ft_beng_lifetime_regression add fulltext index ft_body(body) with parser beng;
select id from ft_beng_lifetime_regression where match(body) against('database') order by id;
select id from ft_beng_lifetime_regression where match(body) against('audit') order by id;
select id from ft_beng_lifetime_regression where match(body) against('+audit -database' in boolean mode) order by id;
update ft_beng_lifetime_regression set body = 'database audit security' where id = 3;
select id from ft_beng_lifetime_regression where match(body) against('database audit') order by id;
delete from ft_beng_lifetime_regression where id = 1;
select id from ft_beng_lifetime_regression where match(body) against('database') order by id;
drop table ft_beng_lifetime_regression;
