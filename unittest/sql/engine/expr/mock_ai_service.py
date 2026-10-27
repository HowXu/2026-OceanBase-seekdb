#!/usr/bin/env python3
"""Deterministic protocol mock for SQL AI function regression tests (no model calls)."""
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class MockAIHandler(BaseHTTPRequestHandler):
    requests = []

    def log_message(self, *args):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        self.requests.append((self.path, body))
        if self.path == '/generate':
            assert body.get('stream') is False
            assert self.headers.get_content_type() == 'application/json'
            result = {'response': body['prompt']}
        elif self.path.startswith('/complete'):
            content = body['messages'][-1]['content']
            if self.path == '/complete-empty':
                content = ''
            elif self.path == '/complete-null':
                content = None
            result = {'choices': [{'message': {'content': content}}]}
        elif self.path.startswith('/embed'):
            dimension = body.get('dimensions', 3)
            vectors = [[float(len(text))] * dimension for text in body['input']]
            if self.path == '/embed-bad':
                vectors[0] = [None] * dimension
            if self.path == '/embed-short':
                vectors[0] = vectors[0][:-1]
            if self.path == '/embed-ollama':
                result = {'embeddings': vectors}
            else:
                result = {'data': [{'index': i, 'embedding': v}
                                   for i, v in reversed(list(enumerate(vectors)))]}
                if self.path == '/embed-duplicate' and len(vectors) > 1:
                    result['data'][0]['index'] = result['data'][1]['index']
        elif self.path.startswith('/rerank'):
            # Deliberately unsorted results, with ties and numeric integer scores.
            result = {'results': [{'index': i, 'relevance_score': len(text) % 3}
                                  for i, text in reversed(list(enumerate(body['documents'])))]}
            if self.path == '/rerank-bad':
                result['results'][0]['index'] = len(body['documents'])
        else:
            self.send_error(404)
            return
        data = json.dumps(result).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)


if __name__ == '__main__':
    ThreadingHTTPServer(('0.0.0.0', 8000), MockAIHandler).serve_forever()
