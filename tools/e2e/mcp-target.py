#!/usr/bin/env python3
"""Probe bearer MCP on a unique md-l7-owned window; never dump the desktop tree.

--click is opt-in and only targets a unique button below that owned window.
WARNING md-ixkl: coordinate-based action can hit an occluding window instead.
Use --click only with the owned occlusion fixture covering the target, never
with an operator window above it. Without it, require input tools absent.
"""
import argparse
import json
from pathlib import Path
import re
import time
import urllib.error
import urllib.request

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--url', required=True)
p.add_argument('--token', type=Path, required=True)
p.add_argument('--window', required=True)
p.add_argument('--button', required=True)
p.add_argument('--click', action='store_true')
a = p.parse_args()
if not a.window.startswith('md-l7') or not a.button.startswith('md-l7'):
    p.error('only md-l7-owned fixtures may be targeted')
class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None  # Never forward the bearer credential to a redirected URL.


opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())
token = a.token.read_text().strip()
ident = 0


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def send(method, params=None, auth=True, status=200):
    global ident
    ident += 1
    rpc = dict(jsonrpc='2.0', method=method, params=params or {})
    if not method.startswith('notifications/'):
        rpc['id'] = ident
    headers = {'Content-Type': 'application/json'}
    if auth:
        headers['Authorization'] = 'Bearer ' + token
    req = urllib.request.Request(a.url, json.dumps(rpc).encode(), headers)
    started = time.monotonic()
    try:
        response = opener.open(req, timeout=90)
    except urllib.error.HTTPError as e:
        response = e
    with response:
        require(response.status == status, (method, response.status, status))
        body = response.read()
    print(json.dumps(dict(method=method, status=status, seconds=time.monotonic() - started)), flush=True)
    return json.loads(body) if status == 200 else None


def tool(name, arguments):
    result = send('tools/call', dict(name=name, arguments=arguments))
    require('result' in result and not result['result'].get('isError'), name)
    return json.loads(result['result']['content'][0]['text'])


def nodes(node):
    if isinstance(node, dict):
        yield node
        for child in node.get('children', []):
            yield from nodes(child)
    elif isinstance(node, list):
        for child in node:
            yield from nodes(child)


send('initialize', auth=False, status=401)
require('result' in send('initialize', dict(protocolVersion='2025-03-26', capabilities={},
                                           clientInfo=dict(name='md-l7-owned-target', version='1'))),
        'initialize failed')
send('notifications/initialized', status=202)
listing = send('tools/list')['result']['tools']
names = {t['name'] for t in listing}
windows = tool('metadesk_windows', {})
owned = [w for w in windows if w.get('label', '').casefold() == a.window.casefold()]
require(len(owned) == 1, 'owned window not unique/present')
window_id = owned[0]['id']
tree = tool('metadesk_subtree', dict(id=window_id, depth=12, format='json'))
buttons = [n for n in nodes(tree) if n.get('role') == 'button' and n.get('label') == a.button]
require(len(buttons) == 1, 'owned button not unique/present')
target_id = buttons[0]['id']
if a.click:
    require('metadesk_click' in names, 'action unavailable')
    before = re.findall(r'md-l7 clicks (\d+)', json.dumps(tree))
    require(len(before) == 1, 'owned click counter missing/ambiguous')
    result = send('tools/call', dict(name='metadesk_click', arguments=dict(target_id=target_id)))
    require('result' in result and not result['result'].get('isError'), 'action failed')
    after_tree = tool('metadesk_subtree', dict(id=window_id, depth=12, format='json'))
    after = re.findall(r'md-l7 clicks (\d+)', json.dumps(after_tree))
    require(len(after) == 1 and int(after[0]) == int(before[0]) + 1, 'no target-only state change')
    print(json.dumps(dict(action_by_id='PASS', before=int(before[0]), after=int(after[0]))))
else:
    require('metadesk_click' not in names, 'readonly host unexpectedly lists input')
    denied = send('tools/call', dict(name='metadesk_click', arguments=dict(target_id=target_id)))
    require('error' in denied or denied.get('result', {}).get('isError'), 'input-disabled action accepted')
    print(json.dumps(dict(tree='PASS', input_disabled_negative='PASS', action_by_id='NOT_RUN')))
