"""paho-mqtt 2.x 封装 (TP-DEV-001 §3.6.2)。

只做连接管理，不在此处做任何业务解析或磁盘写入。
"""
from __future__ import annotations

import paho.mqtt.client as mqtt


def make_client(client_id: str, username: str | None, password: str | None,
                on_connect, on_message, on_disconnect=None) -> mqtt.Client:
    client = mqtt.Client(
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
        client_id=client_id,
        protocol=mqtt.MQTTv311,
        clean_session=True,
    )
    if username:
        client.username_pw_set(username, password)
    client.reconnect_delay_set(min_delay=1, max_delay=10)
    client.on_connect = on_connect
    client.on_message = on_message
    if on_disconnect:
        client.on_disconnect = on_disconnect
    return client
