"""Endpoint for the future MQTT/fusion process. No sensor, broker or fusion is simulated here.

After fusion: link.send_frame(...). Poll link.poll_recenter(); perform real stationary
calibration, then link.acknowledge_center(request, source_seq=last_calibrated_seq).
UE requires a newer healthy frame after that ACK. Angles are degrees, not thrust.
"""
import configparser,json,math,socket,time,uuid
from pathlib import Path

class LunaUDP:
    def __init__(self,config_path=None):
        path=Path(config_path or Path(__file__).with_name('controller.json'))
        self.config=json.loads(path.read_text(encoding='utf-8'))
        ini=configparser.ConfigParser(strict=False,interpolation=None);ini.read(path.parent/self.config['ue_settings_file'],encoding='utf-8')
        settings=ini[self.config['ue_settings_section']]
        self.target=(self.config['ue_host'],settings.getint('InputPort',7001))
        self.socket=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
        self.socket.bind((settings.get('BridgeAddress','127.0.0.1'),settings.getint('ControlPort',7002)))
        self.socket.setblocking(False);self.bridge_id=uuid.uuid4().hex;self.boot_id=None
    def send_frame(self,*,boot_id,source_seq,source_time,pitch_deg,roll_deg,gesture_seq=0,gesture='NONE',valid=True,calibrated=False,source_bound_ms=0):
        now=time.monotonic()
        if not all(math.isfinite(v) for v in [source_time,pitch_deg,roll_deg,source_bound_ms]) or source_time>now+.02:raise ValueError('Invalid source clock or angles')
        if abs(pitch_deg)>45 or abs(roll_deg)>45 or source_bound_ms<0:raise ValueError('Angles outside UE limits')
        if gesture not in ('NONE','UP','DOWN') or source_seq<0 or gesture_seq<0:raise ValueError('Invalid counter or gesture')
        self.boot_id=str(boot_id)
        packet=dict(schema=1,type='input',bridge_id=self.bridge_id,boot_id=self.boot_id,source_seq=int(source_seq),source_age_ms=max(0,(now-source_time)*1000),source_bound_ms=source_bound_ms,valid=bool(valid),calibrated=bool(calibrated),pitch_deg=pitch_deg,roll_deg=roll_deg,gesture_seq=int(gesture_seq),gesture=gesture)
        self._send(packet)
    def poll_recenter(self):
        try:data,peer=self.socket.recvfrom(4096)
        except BlockingIOError:return None
        packet=json.loads(data.decode('utf-8'))
        if packet.get('schema')==1 and packet.get('type')=='recenter' and packet.get('bridge_id')==self.bridge_id and packet.get('boot_id')==self.boot_id and packet.get('request_id'):return packet
        return None
    def acknowledge_center(self,request,*,source_seq,ok=True):
        if request.get('bridge_id')!=self.bridge_id or request.get('boot_id')!=self.boot_id:raise ValueError('Stale centering transaction')
        self._send(dict(schema=1,type='recenter_ack',bridge_id=self.bridge_id,boot_id=self.boot_id,request_id=request['request_id'],source_seq=int(source_seq),ok=bool(ok),code='CENTERED' if ok else 'NOT_STATIONARY'))
    def _send(self,packet):self.socket.sendto(json.dumps(packet,allow_nan=False,separators=(',',':')).encode('utf-8'),self.target)
    def close(self):self.socket.close()
    def __enter__(self):return self
    def __exit__(self,*args):self.close()
