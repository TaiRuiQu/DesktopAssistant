"""Loopback-only integration fixture. No user settings, keys or external requests."""
import http.server
import base64
import io
import json
import pathlib
import subprocess
import sys
import threading
import time
import wave

seen = []
errors = []

def sample_wav():
    buffer = io.BytesIO()
    with wave.open(buffer, 'wb') as audio:
        audio.setnchannels(1)
        audio.setsampwidth(2)
        audio.setframerate(24000)
        audio.writeframes(b'\x00\x00' * 240)
    return buffer.getvalue()

class Fixture(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        seen.append(self.path)
        if self.headers.get('Authorization'):
            errors.append('API key leaked to audio download')
        if self.path != '/audio.wav?signature=fixture':
            errors.append('signed audio URL query not preserved')
        payload = sample_wav()
        self.send_response(200)
        self.send_header('Content-Type', 'audio/wav')
        self.send_header('Content-Length', str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_POST(self):
        body = self.rfile.read(int(self.headers['Content-Length']))
        seen.append(self.path)
        key = 'speech-fixture-key' if self.path.startswith('/speech/') else 'tts-fixture-key' if self.path.startswith('/tts/') else 'fixture-key'
        if self.headers.get('Authorization') != f'Bearer {key}':
            errors.append('fixture authorization missing')
        text = body.decode('utf-8', errors='replace')
        status = 200
        headers = {}
        if 'http-error' in text:
            status, payload = 401, b'{"error":"fixture-key must never be shown"}'
        elif 'http-redirect' in text:
            status, payload = 302, b''
            headers['Location'] = 'http://localhost:1/should-not-follow'
        elif self.path in ('/v1/audio/transcriptions', '/speech/v1/audio/transcriptions'):
            if 'multipart/form-data' not in self.headers.get('Content-Type', '') or b'RIFF' not in body or b'fixture-model' not in body:
                errors.append('invalid multipart transcription')
            payload = b'{"text":"fixture transcript"}'
        elif self.path == '/speech/v1/chat/completions':
            request = json.loads(body)
            content = request['messages'][0]['content']
            audio = next(part['input_audio'] for part in content if part['type'] == 'input_audio')
            dashscope = 'asr_options' in request
            encoded = audio['data']
            if dashscope:
                if len(content) != 1 or not encoded.startswith('data:audio/wav;base64,') or not request['asr_options'].get('enable_itn'):
                    errors.append('invalid DashScope ASR request')
                encoded = encoded.split(',', 1)[-1]
            elif audio.get('format') != 'wav' or len(content) != 2:
                errors.append('invalid audio chat ASR request')
            if not base64.b64decode(encoded).startswith(b'RIFF') or request.get('stream') is not False:
                errors.append('invalid audio chat WAV or stream mode')
            transcript = 'dashscope transcript' if dashscope else 'audio chat transcript'
            payload = json.dumps({'choices': [{'message': {'content': transcript}, 'finish_reason': 'stop'}]}).encode()
        elif self.path == '/tts/v1/services/aigc/multimodal-generation/generation':
            request = json.loads(body)
            if request.get('model') != 'qwen3-tts-flash' or request['input'].get('voice') != 'Cherry' or request['input'].get('language_type') != 'Auto':
                errors.append('invalid DashScope TTS request')
            prompt = request['input']['text']
            if prompt == 'download-audio':
                audio = {'url': f'http://127.0.0.1:{self.server.server_port}/audio.wav?signature=fixture'}
            elif prompt == 'invalid-audio-url':
                audio = {'url': 'file:///private.wav'}
            elif prompt == 'inline-pcm':
                audio = {'data': base64.b64encode(b'\x00\x00' * 240).decode()}
            else:
                audio = {'data': base64.b64encode(sample_wav()).decode()}
            payload = json.dumps({'output': {'audio': audio}}).encode()
        elif self.path == '/v1/chat/completions':
            request = json.loads(body)
            if request.get('store') is not False or not request.get('stream') or len(request['messages']) < 2:
                errors.append('invalid Chat Completions request')
            payload = b'data: {"choices":[{"delta":{"content":"fixture answer"},"finish_reason":null}]}\n\ndata: {"choices":[{"delta":{},"finish_reason":"stop"}]}\n\ndata: [DONE]\n\n'
        elif self.path == '/v1/responses':
            request = json.loads(body)
            if request.get('store') is not False or not request.get('stream'):
                errors.append('invalid Responses request')
            payload = b'data: {"type":"response.output_text.delta","delta":"fixture answer"}\n\n'
            if 'http-truncated' not in text and 'http-blocked' not in text:
                payload += b'data: {"type":"response.completed"}\n\n'
        else:
            errors.append('unexpected route')
            status, payload = 404, b''
        self.send_response(status)
        for key, value in headers.items():
            self.send_header(key, value)
        self.send_header('Content-Type', 'text/event-stream' if b'data:' in payload else 'application/json')
        if 'http-blocked' not in text:
            self.send_header('Content-Length', str(len(payload)))
        self.end_headers()
        try:
            # Deliberately split every byte to exercise WinHTTP/SSE buffering.
            for byte in payload:
                self.wfile.write(bytes([byte]))
            self.wfile.flush()
            if 'http-blocked' in text:
                time.sleep(3)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass

if __name__ == '__main__':
    executable = pathlib.Path(sys.argv[1]).resolve()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Fixture)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    endpoint = f'http://127.0.0.1:{server.server_port}/v1/'
    if executable.stem == 'DesktopAssistant':
        result = subprocess.run([str(executable), '--ui-checks', '--fixture-url', endpoint, '--outputdir', str(pathlib.Path(__file__).resolve().parent.parent / 'artifacts/ui-integration')], timeout=45)
        expected = 2
    else:
        result = subprocess.run([str(executable), '--http-tests', endpoint], timeout=45)
        expected = 16
    server.shutdown()
    if result.returncode:
        sys.exit(result.returncode)
    if errors or len(seen) != expected:
        print('FAIL: loopback fixture:', errors, 'requests:', len(seen))
        sys.exit(1)
    print(f'PASS: {expected} loopback HTTP requests validated; no external API calls.')
