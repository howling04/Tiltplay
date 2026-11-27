"""Local UE native MCP client; no third-party packages required.

Default action only discovers tools. Tool calls must be explicitly requested.
"""
import argparse
import datetime
import json
from pathlib import Path
import urllib.request


class Client:
    def __init__(self):
        self.url = "http://127.0.0.1:8000/mcp"
        self.session = None
        self.protocol = None
        self.sequence = 0
        self.http = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def request(self, method, params=None, notification=False):
        self.sequence += 1
        data = {"jsonrpc": "2.0", "method": method}
        if not notification:
            data["id"] = self.sequence
        if params is not None:
            data["params"] = params
        headers = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream"}
        if self.session:
            headers["Mcp-Session-Id"] = self.session
        if self.protocol:
            headers["Mcp-Protocol-Version"] = self.protocol
        req = urllib.request.Request(self.url, json.dumps(data).encode(), headers)
        with self.http.open(req, timeout=120) as response:
            self.session = response.headers.get("Mcp-Session-Id", self.session)
            if response.headers.get_content_type() == "text/event-stream":
                lines = []
                while True:
                    line = response.readline()
                    if not line:
                        raise RuntimeError("SSE ended before JSON-RPC response")
                    line = line.decode("utf-8").rstrip("\r\n")
                    if line.startswith("data:"):
                        lines.append(line[5:].lstrip())
                    elif not line and lines:
                        candidate = json.loads("\n".join(lines))
                        lines = []
                        if candidate.get("id") == data.get("id"):
                            result = candidate
                            break
            else:
                body = response.read()
                if not body:
                    if notification:
                        return None
                    raise RuntimeError("Empty response")
                result = json.loads(body)
        if "error" in result:
            raise RuntimeError(json.dumps(result["error"], ensure_ascii=False))
        return result.get("result")

    def initialize(self):
        info = self.request("initialize", {
            "protocolVersion": "2025-06-18", "capabilities": {},
            "clientInfo": {"name": "Luna-MCP-check", "version": "1.0"}})
        self.protocol = info["protocolVersion"]
        self.request("notifications/initialized", notification=True)
        return info

    def call(self, name, arguments):
        result = self.request("tools/call", {"name": name, "arguments": arguments})
        if result.get("isError"):
            raise RuntimeError(json.dumps(result, ensure_ascii=False))
        return result

    def close(self):
        if self.session:
            req = urllib.request.Request(self.url, method="DELETE", headers={
                "Mcp-Session-Id": self.session, "Mcp-Protocol-Version": self.protocol})
            with self.http.open(req, timeout=10) as response:
                response.read()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", help="Explicit native MCP tool to call")
    parser.add_argument("--arguments", default="{}", help="JSON arguments")
    parser.add_argument("--arguments-file", type=Path, help="UTF-8 JSON arguments file")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    client = Client()
    try:
        report = {"checked_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  "endpoint": client.url, "initialize": client.initialize()}
        if args.tool:
            arguments = json.loads(args.arguments_file.read_text(encoding="utf-8-sig")
                                   if args.arguments_file else args.arguments)
            report["result"] = client.call(args.tool, arguments)
        else:
            report["tools"] = client.request("tools/list")
            report["toolsets"] = client.call("list_toolsets", {})
        rendered = json.dumps(report, ensure_ascii=False, indent=2)
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(rendered + "\n", encoding="utf-8")
        print(rendered)
    finally:
        client.close()


if __name__ == "__main__":
    main()
