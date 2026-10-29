# owner group: storage
# tags: fts
# description: Ordered IK arbitration chains preserve gaps, overlaps and segmentation modes.
set names utf8mb4;
select tokenize('中华人民共和国人民大会堂 数据库 全文索引', 'ik', '[{"additional_args": [{"ik_mode": "smart"}]}]');
select tokenize('中华人民共和国人民大会堂 数据库 全文索引', 'ik', '[{"additional_args": [{"ik_mode": "max_word"}]}]');
select tokenize('ABC 123 abc.def@example.com 3.14 -42 三百二十五公斤', 'ik');
select tokenize('，。！？；：、—（）【】', 'ik');
select tokenize('中文 ひらがな カタカナ 한글 😀 𠀀 ＡＢＣ１２３', 'ik');
select tokenize(cast(concat('ABC', char(0), '数据库 全文索引') as char character set utf8mb4), 'ik');
select tokenize(cast(repeat('数据库，全文索引！OceanBase１２３ 😀 ', 100) as char character set utf8mb4), 'ik', '[{"output": "all"}, {"additional_args": [{"ik_mode": "smart"}]}]');
select tokenize(cast(repeat('数据库，全文索引！OceanBase１２３ 😀 ', 100) as char character set utf8mb4), 'ik', '[{"output": "all"}, {"additional_args": [{"ik_mode": "max_word"}]}]');
