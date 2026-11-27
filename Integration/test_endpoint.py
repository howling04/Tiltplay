"""Local UDP contract fixture; does not exercise a board, MQTT broker or fusion."""
import json,socket,time
from ue_udp import LunaUDP

with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as ue:
    ue.bind(('127.0.0.1',7001));ue.settimeout(1)
    with LunaUDP() as link:
        link.send_frame(boot_id='transport-fixture',source_seq=10,source_time=time.monotonic()-.02,pitch_deg=8,roll_deg=-4,calibrated=True)
        packet,peer=ue.recvfrom(4096);packet=json.loads(packet)
        assert packet['source_seq']==10 and 19<=packet['source_age_ms']<200
        assert packet['pitch_deg']==8 and packet['roll_deg']==-4
        request={'schema':1,'type':'recenter','request_id':'fixture-request','bridge_id':packet['bridge_id'],'boot_id':packet['boot_id']}
        ue.sendto(json.dumps(request).encode(),peer)
        pending=None
        for i in range(20):
            pending=link.poll_recenter()
            if pending:break
            time.sleep(.005)
        assert pending==request
        link.acknowledge_center(pending,source_seq=10)
        ack=json.loads(ue.recvfrom(4096)[0]);assert ack['request_id']=='fixture-request' and ack['code']=='CENTERED' and ack['source_seq']==10
        link.send_frame(boot_id='transport-fixture',source_seq=11,source_time=time.monotonic()-1,pitch_deg=0,roll_deg=0,calibrated=True)
        stale=json.loads(ue.recvfrom(4096)[0]);assert stale['source_age_ms']>=1000,'Bridge must not rejuvenate old MQTT data'
print('PASS: configured UDP input, recenter response and original sample age')
