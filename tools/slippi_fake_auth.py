#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A stand-in for the Firebase and Slippi endpoints the account pages call
(src/pc/slippi/slp_user.c), so Login, Register and the setup steps run on a
desktop with no real account touched.

    python3 tools/slippi_fake_auth.py --port 8791 \
        --user 'new@x.test:secret:Dummy Rules' \
        --user 'rules@x.test:secret:Rules Only:verified,code'

    MELEE_SLIPPI_FIREBASE_KEY=fake \
    MELEE_SLIPPI_AUTH_HOST=http://127.0.0.1:8791 \
    MELEE_SLIPPI_GRAPHQL_ENDPOINT=http://127.0.0.1:8791/graphql  build/.../melee

A user is email:password:name[:flags], flags from verified, rules, code. The
replies copy the real ones' shapes, errors included. A verification email
verifies the address after --verify-after checks, as if its link was opened;
GET /verify?email=... does it at once.
"""
import argparse
import json
import secrets
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

lock = threading.Lock()
users = {}   # email -> dict
tokens = {}  # id or refresh token -> email


def new_tokens(email):
    idt, ref = 'id-' + secrets.token_hex(8), 'rf-' + secrets.token_hex(8)
    tokens[idt] = tokens[ref] = email
    return idt, ref


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        sys.stderr.write('fake-auth: ' + (fmt % args) + '\n')

    def send(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def fb_error(self, message):
        self.send(400, {'error': {'code': 400, 'message': message}})

    def do_GET(self):
        u = urlparse(self.path)
        if u.path == '/verify':
            email = parse_qs(u.query).get('email', [''])[0]
            with lock:
                if email in users:
                    users[email]['verified'] = True
            self.send(200, {'ok': email in users})
        elif u.path == '/state':
            with lock:
                self.send(200, {e: {k: v for k, v in d.items() if k != 'password'} for e, d in users.items()})
        else:
            self.send(404, {})

    def do_POST(self):
        u = urlparse(self.path)
        n = int(self.headers.get('Content-Length', 0))
        try:
            body = json.loads(self.rfile.read(n) or b'{}')
        except ValueError:
            self.fb_error('INVALID_JSON')
            return
        bearer = (self.headers.get('Authorization') or '').removeprefix('Bearer ')
        with lock:
            self.route(u.path, body, bearer)

    def route(self, path, body, bearer):
        if path == '/v1/accounts:signInWithPassword':
            d = users.get(body.get('email', ''))
            if d is None or d['password'] != body.get('password'):
                self.fb_error('INVALID_LOGIN_CREDENTIALS')
                return
            idt, ref = new_tokens(body['email'])
            self.send(200, {'idToken': idt, 'refreshToken': ref, 'localId': d['uid'], 'email': body['email']})
        elif path == '/v1/token':
            email = tokens.get(body.get('refresh_token', ''))
            if email is None:
                self.fb_error('INVALID_REFRESH_TOKEN')
                return
            idt, ref = new_tokens(email)
            self.send(200, {'id_token': idt, 'refresh_token': ref, 'user_id': users[email]['uid']})
        elif path == '/v1/accounts:lookup':
            email = tokens.get(body.get('idToken', ''))
            if email is None:
                self.fb_error('INVALID_ID_TOKEN')
                return
            d = users[email]
            d['checks'] += 1
            if d['sent'] and d['checks'] > args.verify_after:
                d['verified'] = True
            self.send(200, {'users': [{'localId': d['uid'], 'email': email, 'emailVerified': d['verified']}]})
        elif path == '/v1/accounts:sendOobCode':
            if body.get('requestType') == 'PASSWORD_RESET':
                if body.get('email') not in users:
                    self.fb_error('EMAIL_NOT_FOUND')
                    return
                self.send(200, {'email': body['email']})
                return
            email = tokens.get(body.get('idToken', ''))
            if email is None:
                self.fb_error('INVALID_ID_TOKEN')
                return
            users[email]['sent'] = True
            users[email]['checks'] = 0
            self.send(200, {'email': email})
        elif path == '/createUserNew':
            data = body.get('data', {})
            name, email, pw = data.get('displayName', ''), data.get('email', ''), data.get('password', '')
            if not name:
                self.send(400, {'error': {'message': 'Display name must be at least 1 character',
                                          'status': 'INVALID_ARGUMENT'}})
            elif email in users:
                self.send(400, {'error': {'message': 'The email address is already in use by another account.',
                                          'status': 'ALREADY_EXISTS'}})
            elif len(pw) < 6:
                self.send(400, {'error': {'message': 'The password must be a string with at least 6 characters.',
                                          'status': 'INVALID_ARGUMENT'}})
            else:
                add_user(email, pw, name, '')
                self.send(200, {'result': {}})
        elif path == '/graphql':
            email = tokens.get(bearer)
            if email is None:
                self.send(200, {'errors': [{'message': 'Unauthorized'}], 'data': None})
                return
            d = users[email]
            q = body.get('query', '')
            v = body.get('variables', {})
            if 'userAcceptRules' in q:
                d['rules'] = v.get('num', 0)
                self.send(200, {'data': {'userAcceptRules': {'fbUid': d['uid'], 'rulesAccepted': d['rules']}}})
            elif 'userInitNetplay' in q:
                tag = v.get('codeStart', '')
                if tag in ('BAD', 'NOPE'):
                    self.send(200, {'errors': [{'message': 'That code is not allowed'}], 'data': None})
                    return
                d['code'] = f'{tag}#{secrets.randbelow(900) + 100}'
                self.send(200, {'data': {'userInitNetplay': {'fbUid': d['uid']}}})
            else:
                self.send(200, {'data': {
                    'getUser': {'fbUid': d['uid'], 'displayName': d['name'],
                                'connectCode': {'code': d['code']} if d['code'] else None,
                                'private': {'playKey': 'pk-' + d['uid']} if d['code'] else None,
                                'rulesAccepted': d['rules']},
                    'getLatestDolphin': {'version': '3.6.4'}}})
        else:
            self.send(404, {})


def add_user(email, pw, name, flags):
    f = set(x for x in flags.split(',') if x)
    users[email] = {'password': pw, 'name': name, 'uid': 'fake-' + secrets.token_hex(6),
                    'verified': 'verified' in f, 'rules': 1 if 'rules' in f else 0,
                    'code': (''.join(c for c in name.upper() if c.isalpha())[:4] or 'FAKE') + '#123'
                    if 'code' in f else '', 'sent': False, 'checks': 0}


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', type=int, default=8791)
    ap.add_argument('--user', action='append', default=[])
    ap.add_argument('--verify-after', type=int, default=1,
                    help='checks after a verification email before it counts as opened')
    args = ap.parse_args()
    for spec in args.user:
        parts = spec.split(':')
        add_user(parts[0], parts[1], parts[2], parts[3] if len(parts) > 3 else '')
    ThreadingHTTPServer(('127.0.0.1', args.port), Handler).serve_forever()
