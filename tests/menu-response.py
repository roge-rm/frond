#!/usr/bin/env python3
import importlib.util

spec = importlib.util.spec_from_file_location('menu', 'tools/analyze-menu-response.py')
menu = importlib.util.module_from_spec(spec)
spec.loader.exec_module(menu)
trace = '''MENU INPUT 1000000 274 8
MENU TOUCH 1020000 2000000000 1 274 8
MENU FRAME 1200000 2180000000 1234 0 58000000 216 1
MENU FRAME 1300000 2280000000 1234 0 58000000 1200 0
MENU FRAME 1500000 2480000000 1234 0 58000000 1136 1
MENU PRESENT 1500500 500
MENU INPUT 4000000 274 8
MENU TOUCH 4010000 5000000000 1 274 8
MENU FRAME 4300000 5290000000 1234 0 58000000 216 1
MENU PRESENT 4300500 500
MENU INPUT 7000000 100 50
'''
r = menu.analyze(trace)
assert r['responses'] == 1 and r['median_ms'] == r['max_ms'] == 500.5
assert r['trials'][0]['panel_ms'] == 20 and r['trials'][0]['menu_frame_ms'] == 500
assert r['trials'][0]['panel_to_frame_guest_ms'] == 480
assert r['trials'][1]['status'] == 'no_response'
assert r['trials'][1]['present_ms'] is None
print('PASS: menu timing requires a new matching frame and presentation; missing responses retained')
