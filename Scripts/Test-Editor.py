"""Verification utility only: control the UE editor through its native MCP API."""
import base64
import json
from pathlib import Path
import socket
import sys
import time

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'Scripts'))
from ue_mcp import Client

def port_is_free():
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as s:
        try:
            s.bind(('127.0.0.1',7001))
            return True
        except OSError:
            return False

client=Client()
report={'cycles':[]}
def call(tool,arguments=None,toolset='EditorToolset.EditorAppToolset'):
    result=client.call('call_tool',{'toolset_name':toolset,'tool_name':tool,'arguments':arguments or {}})
    if result.get('isError'):
        raise RuntimeError(result)
    return result
def value(result):
    return json.loads(result['content'][0]['text'])['returnValue']
try:
    for attempt in range(40):
        try:
            client.initialize()
            level=value(call('get_current_level',toolset='editor_toolset.toolsets.scene.SceneTools'))
            if level=='/Game/LunaArt/Maps/L_WanderingLuna':
                break
        except Exception:
            pass
        try:
            client.close()
        except Exception:
            pass
        client=Client()
        time.sleep(1)
    else:
        raise RuntimeError('MVP editor did not become ready')
    assert level=='/Game/LunaArt/Maps/L_WanderingLuna',level
    report['level']=level
    assert not value(call('IsPIERunning')),'PIE is already active; left untouched'
    assert port_is_free(),'Port 7001 was already in use; left untouched'
    for index in range(2):
        call('StartPIE',{'options':{'bSimulate':False,'playMode':'PlayMode_InEditorFloating','warmupSeconds':1.0}})
        running=value(call('IsPIERunning'))
        bound=not port_is_free()
        if index==1:
            shot=call('CaptureEditorImage')
            image=next((x['data'] for x in shot['content'] if x['type']=='image'),None)
            if not image:
                image=value(shot)['data']
            (ROOT/'Saved/Verification/editor-play.png').write_bytes(base64.b64decode(image))
        call('StopPIE')
        stopped=not value(call('IsPIERunning'))
        released=port_is_free()
        record={'cycle':index+1,'running':running,'udp_bound':bound,'stopped':stopped,'udp_released':released}
        report['cycles'].append(record)
        assert all(record[k] for k in ('running','udp_bound','stopped','udp_released')),record
    report['passed']=True
finally:
    client.close()
    (ROOT/'Saved/Verification/editor-lifecycle.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print(json.dumps(report,indent=2))
