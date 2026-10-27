#!/usr/bin/env python3
"""Run against a freshly built seekdb: AI_TEST_PORT=18881 python3 <this file>."""
import json
import os
from pathlib import Path
import subprocess
import threading
import unittest
from http.server import ThreadingHTTPServer
from mock_ai_service import MockAIHandler

ROOT = Path(__file__).resolve().parents[4]
CLIENT = ROOT / 'deps/3rd/u01/obclient/bin/obclient'


def quote(value):
    return "'" + value.replace('\\', '\\\\').replace("'", "''") + "'"


class AIEnhancementTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = ThreadingHTTPServer(('127.0.0.1', 8000), MockAIHandler)
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()
        for name, kind in [('complete', 'completion'), ('embed', 'dense_embedding'), ('rerank', 'rerank')]:
            cls.sql("call dbms_ai_service.create_ai_model(%s, %s)" % (
                quote('reg_' + name), quote(json.dumps({'type': kind, 'model_name': 'regression-model'}))))
            cls.sql("call dbms_ai_service.create_ai_model_endpoint(%s, %s)" % (
                quote('reg_' + name), quote(json.dumps({'ai_model_name': 'reg_' + name,
                    'url': 'http://127.0.0.1:8000/' + name, 'access_key': 'test-key',
                    'provider': 'siliconflow' if name == 'rerank' else 'openai'}))))

    @classmethod
    def tearDownClass(cls):
        for name in ['complete', 'embed', 'rerank']:
            cls.sql('call dbms_ai_service.drop_ai_model_endpoint(%s)' % quote('reg_' + name))
            cls.sql('call dbms_ai_service.drop_ai_model(%s)' % quote('reg_' + name))
        cls.server.shutdown()
        cls.server.server_close()

    @staticmethod
    def sql(sql, success=True):
        proc = subprocess.run([str(CLIENT), '-h127.0.0.1', '-P' + os.environ.get('AI_TEST_PORT', '18881'),
                               '-uroot', '-Doceanbase', '-N', '-B', '--raw', '-e', sql],
                              text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if success and proc.returncode:
            raise AssertionError(proc.stderr)
        if not success and not proc.returncode:
            raise AssertionError('expected SQL failure: ' + sql)
        return proc.stdout.strip()

    def endpoint(self, name, **changes):
        self.sql('call dbms_ai_service.alter_ai_model_endpoint(%s,%s)' % (
            quote('reg_' + name), quote(json.dumps(changes))))

    def complete(self, prompt, config='NULL'):
        return self.sql("select ai_complete('reg_complete',%s,%s)" % (prompt, config))

    def test_prompt(self):
        self.assertEqual('abcdef' * 10, self.complete("ai_prompt('{0}{0}{0}{0}{0}{0}{0}{0}{0}{0}', 'abcdef')"))
        self.assertEqual('outer:inner:世界', self.complete("ai_prompt('outer:{0}',ai_prompt('inner:{0}','世界'))"))
        self.assertEqual({'a': [1, 2]}, json.loads(self.complete("ai_prompt('{0}',json_object('a',json_array(1,2)))")))
        self.sql("select ai_complete('reg_complete', ai_prompt('{999999999999999999999999999999999999999999999}', 'x'))", False)
        self.sql('create table reg_text (t text, a longtext)')
        try:
            self.sql("insert into reg_text values ('{0}{0}', repeat('a', 70000))")
            self.assertEqual('140000', self.sql("select length(json_unquote(ai_complete('reg_complete',ai_prompt(t,a)))) from reg_text"))
        finally:
            self.sql('drop table reg_text')

    def test_complete(self):
        self.assertEqual('hello', self.complete("'hello'"))
        self.assertEqual('hello', self.complete("'hello'", "json_object('temperature',0.2,'max_tokens',12)"))
        self.assertEqual(12, MockAIHandler.requests[-1][1]['max_tokens'])
        self.assertEqual(0.2, MockAIHandler.requests[-1][1]['temperature'])
        self.assertEqual('hello', self.complete("'hello'", quote('{"temperature":0.3}')))
        for config in ["json_array(1)", "'[]'", "'broken-json'"]:
            self.sql("select ai_complete('reg_complete','hello',%s)" % config, False)
        for path in ['complete-empty', 'complete-null']:
            self.endpoint('complete', url='http://127.0.0.1:8000/' + path)
            self.sql("select ai_complete('reg_complete','hello')", False)
        self.endpoint('complete', url='http://127.0.0.1:8000/complete')

    def test_http_failures(self):
        try:
            for path in ['root-array', 'root-null', 'invalid-json', 'http-error', 'disconnect']:
                self.endpoint('complete', url='http://127.0.0.1:8000/' + path)
                self.sql("select ai_complete('reg_complete','hello')", False)
                self.assertEqual('1', self.sql('select 1'))
        finally:
            self.endpoint('complete', url='http://127.0.0.1:8000/complete')

    def test_embed(self):
        self.assertEqual([3., 3., 3.], json.loads(self.sql("select ai_embed('reg_embed','abc')")))
        self.assertEqual([3.] * 5, json.loads(self.sql("select ai_embed('reg_embed','abc',5)")))
        self.assertEqual('1', self.sql("select ai_embed('reg_embed',NULL) is NULL"))
        for dimension in ['0', '-1', 'NULL', '1.5']:
            self.sql("select ai_embed('reg_embed','abc',%s)" % dimension, False)
        for path in ['embed-bad', 'embed-short']:
            self.endpoint('embed', url='http://127.0.0.1:8000/' + path)
            self.sql("select ai_embed('reg_embed','abc',3)", False)
        self.endpoint('embed', url='http://127.0.0.1:8000/embed')

    def test_embed_batch(self):
        self.sql('create table reg_batch (id int, t text)')
        try:
            values = ','.join('(%d,%s)' % (i, 'NULL' if i % 7 == 0 else quote('x' * (i % 11 + 1))) for i in range(150))
            self.sql('insert into reg_batch values ' + values)
            MockAIHandler.requests.clear()
            out = self.sql("select /*+ enable_parallel_dml parallel(1) */ id, ai_embed('reg_embed',t,4) from reg_batch where id >= 3 order by id limit 130")
            for line in out.splitlines():
                key, value = line.split('\t', 1)
                i = int(key)
                self.assertEqual('NULL' if i % 7 == 0 else [float(i % 11 + 1)] * 4,
                                 value if value == 'NULL' else json.loads(value))
            self.assertTrue(any(len(body['input']) > 1 for path, body in MockAIHandler.requests))
        finally:
            self.sql('drop table reg_batch')

    def test_embed_batch_boundaries_and_failures(self):
        self.sql('create table reg_batch_errors (id int, t text, dim int)')
        try:
            values = ','.join('(%d,%s,%d)' % (i, quote('x' * (i % 9 + 1)), i % 4 + 1) for i in range(150))
            self.sql('insert into reg_batch_errors values ' + values)
            rows = self.sql("select id,ai_embed('reg_embed',t,dim) from reg_batch_errors order by id")
            for line in rows.splitlines():
                key, value = line.split('\t', 1)
                i = int(key)
                self.assertEqual([float(i % 9 + 1)] * (i % 4 + 1), json.loads(value))
            MockAIHandler.requests.clear()
            self.assertEqual('150', self.sql("select sum(ai_embed('reg_embed',case when id >= 0 then NULL else t end) is NULL) from reg_batch_errors"))
            self.assertEqual([], MockAIHandler.requests)
            for path in ['embed-duplicate', 'embed-missing', 'embed-index-type', 'embed-shape', 'embed-late-bad']:
                self.endpoint('embed', url='http://127.0.0.1:8000/' + path)
                MockAIHandler.requests.clear()
                self.sql("select ai_embed('reg_embed',t,3) from reg_batch_errors order by id", False)
                if path == 'embed-late-bad':
                    self.assertGreaterEqual(len(MockAIHandler.requests), 2)
                self.assertEqual('1', self.sql('select 1'))
                self.endpoint('embed', url='http://127.0.0.1:8000/embed')
            self.assertEqual('150', self.sql("select count(ai_embed('reg_embed',t,3)) from reg_batch_errors"))
        finally:
            self.endpoint('embed', url='http://127.0.0.1:8000/embed')
            self.sql('drop table reg_batch_errors')

    def test_ollama(self):
        try:
            self.endpoint('complete', provider='OlLaMa', access_key='', url='http://127.0.0.1:8000/generate')
            self.assertEqual('hello', self.complete("'hello'"))
            self.endpoint('embed', provider='OLLAMA', access_key='', url='http://127.0.0.1:8000/embed-ollama')
            self.assertEqual([3.] * 3, json.loads(self.sql("select ai_embed('reg_embed','abc')")))
        finally:
            self.endpoint('complete', provider='openai', access_key='test-key', url='http://127.0.0.1:8000/complete')
            self.endpoint('embed', provider='openai', access_key='test-key', url='http://127.0.0.1:8000/embed')

    def test_ollama_endpoint_crud_without_key(self):
        self.sql("call dbms_ai_service.create_ai_model('reg_ollama','{\"type\":\"completion\",\"model_name\":\"local-model\"}')")
        try:
            self.sql("call dbms_ai_service.create_ai_model_endpoint('reg_ollama'," + quote(json.dumps({
                'ai_model_name': 'reg_ollama', 'url': 'http://127.0.0.1:8000/generate', 'provider': 'ollama'})) + ')')
            try:
                self.assertEqual('hello', self.sql("select ai_complete('reg_ollama','hello')"))
                self.assertEqual('1', self.sql("select count(*) from oceanbase.dba_ob_ai_model_endpoints where endpoint_name='reg_ollama'"))
                self.sql("call dbms_ai_service.alter_ai_model_endpoint('reg_ollama','{\"provider\":\"openai\"}')", False)
            finally:
                self.sql("call dbms_ai_service.drop_ai_model_endpoint('reg_ollama')")
        finally:
            self.sql("call dbms_ai_service.drop_ai_model('reg_ollama')")

    def test_rerank_invalid_responses(self):
        try:
            for path in ['rerank-duplicate', 'rerank-missing', 'rerank-score', 'rerank-shape']:
                self.endpoint('rerank', url='http://127.0.0.1:8000/' + path)
                self.sql("select ai_rerank('reg_rerank','query','[\"a\",\"bb\"]')", False)
                self.assertEqual('1', self.sql('select 1'))
        finally:
            self.endpoint('rerank', url='http://127.0.0.1:8000/rerank')
        for docs, key in [("'[{}]'", "'text'"), ("'[{\"text\":1}]'", "'text'"), ("'[\"a\"]'", "'text'")]:
            self.sql("select ai_rerank('reg_rerank','query',%s,%s)" % (docs, key), False)

    def test_rerank(self):
        documents = ['x' * (i % 5 + 1) for i in range(47)]
        expected = sorted(range(len(documents)), key=lambda i: (-(len(documents[i]) % 3), i))
        result = json.loads(self.sql("select ai_rerank('reg_rerank','query',%s)" % quote(json.dumps(documents))))
        self.assertEqual(expected, [r['index'] for r in result])
        objects = [{'id': i + 100, 'text': text} for i, text in enumerate(documents)]
        result = json.loads(self.sql("select ai_rerank('reg_rerank','query',%s,'text')" % quote(json.dumps(objects))))
        self.assertEqual([i + 100 for i in expected], [r['id'] for r in result])
        self.endpoint('rerank', url='http://127.0.0.1:8000/rerank-bad')
        try:
            self.sql("select ai_rerank('reg_rerank','query','[\"abc\"]')", False)
        finally:
            self.endpoint('rerank', url='http://127.0.0.1:8000/rerank')


if __name__ == '__main__':
    unittest.main(verbosity=2)
