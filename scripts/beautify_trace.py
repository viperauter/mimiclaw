import json
import sys
from datetime import datetime
from pathlib import Path

EVENT_COLORS = {
    "request_start": "\033[94m",
    "system_prompt": "\033[96m",
    "tools": "\033[93m",
    "messages": "\033[95m",
    "llm_call_start": "\033[92m",
    "llm_tool_use": "\033[93m",
    "tool_start": "\033[91m",
    "tool_result": "\033[91m",
    "llm_request": "\033[92m",
    "llm_response_raw": "\033[92m",
    "llm_http_error": "\033[91m",
    "llm_response": "\033[92m",
    "request_end": "\033[94m",
}
RESET = "\033[0m"
BOLD = "\033[1m"


def format_timestamp(ts_ms):
    try:
        dt = datetime.fromtimestamp(ts_ms / 1000)
        return dt.strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
    except:
        return str(ts_ms)


def simplify_trace_id(trace_id):
    if not trace_id:
        return ""
    return trace_id.split("-")[0]


def truncate(text, max_len=80):
    if not text:
        return ""
    text = text.replace("\n", "\\n")
    if len(text) > max_len:
        return text[:max_len] + "..."
    return text


def format_json_field(json_str, indent=2, max_len=80):
    try:
        data = json.loads(json_str)
        formatted = json.dumps(data, ensure_ascii=False, indent=indent)
        if max_len is not None and len(formatted) > max_len:
            return formatted[:max_len] + "\n  ... (truncated)"
        return formatted
    except:
        return json_str


def _format_tools_details(tools_json: str, show_tools: bool):
    if not tools_json:
        return ""
    try:
        tools_list = json.loads(tools_json)
        if not isinstance(tools_list, list):
            return ""
        
        if not show_tools:
            return f"tools count: {len(tools_list)}"
        
        out = f"tools count: {len(tools_list)}\n"
        for i, t in enumerate(tools_list, 1):
            name = t.get("name", "unknown")
            desc = t.get("description", "")
            schema = t.get("input_schema", {})
            
            out += f"\n  --- Tool {i}: {name} ---\n"
            if desc:
                out += f"  description: {desc}\n"
            if schema:
                props = schema.get("properties", {})
                required = schema.get("required", [])
                if props:
                    out += f"  parameters:\n"
                    for pname, pinfo in props.items():
                        ptype = pinfo.get("type", "")
                        pdesc = pinfo.get("description", "")
                        required_mark = " (required)" if pname in required else " (optional)"
                        out += f"    - {pname}{required_mark}: {ptype}"
                        if pdesc:
                            out += f" - {pdesc}"
                        out += "\n"
        return out
    except Exception as e:
        return f"Error parsing tools: {e}"


def format_event_data(event, data, show_tools=False, show_llm_json=False):
    if event == "request_start":
        return f"user_input: {truncate(data.get('user_input', ''))}"
    elif event == "system_prompt":
        prompt = data.get("prompt", "")
        return f"prompt length: {len(prompt)} chars\n\n{prompt}"
    elif event == "tools":
        tools = data.get("tools_json", "")
        return _format_tools_details(tools, show_tools)
    elif event == "messages":
        msgs = data.get("json", "")
        try:
            msgs_list = json.loads(msgs)
            return f"messages count: {len(msgs_list)}"
        except:
            return truncate(msgs, 50)
    elif event == "llm_call_start":
        return f"iteration: {data.get('iteration', '?')}"
    elif event == "llm_tool_use":
        text = data.get("assistant_text", "")
        return f"iteration: {data.get('iteration', '?')}\n  assistant: {truncate(text, 100)}"
    elif event == "tool_start":
        return f"tool: {data.get('tool_name', '?')}\n  input: {truncate(data.get('input', '{}'), 60)}"
    elif event == "tool_result":
        result = data.get("result", "")
        output = data.get("output", "")
        return f"tool: {data.get('tool_name', '?')}\n  result: {truncate(result, 40)}\n  output: {truncate(output, 60)}"
    elif event == "llm_request":
        raw = data.get("json", "")
        if show_llm_json:
            return "request json:\n" + format_json_field(raw, indent=2, max_len=None)
        return "request json: " + truncate(raw, 120)
    elif event == "llm_response_raw":
        raw = data.get("json", "")
        if show_llm_json:
            return "response json:\n" + format_json_field(raw, indent=2, max_len=None)
        return "response json: " + truncate(raw, 120)
    elif event == "llm_http_error":
        status = data.get("status", "")
        err = data.get("error", "")
        last = data.get("last_error", "")
        parts = []
        if status:
            parts.append(f"status: {status}")
        if err:
            parts.append(f"error: {err}")
        if last:
            parts.append(f"last_error: {truncate(last, 200)}")
        return "\n  ".join(parts) if parts else truncate(str(data), 120)
    elif event == "llm_response":
        return truncate(data.get("response_text", ""), 100)
    elif event == "request_end":
        return f"status: {data.get('status', 'unknown')}"
    else:
        return str(data)[:100]


def print_trace_record(record, show_json=False, show_tools=False, show_llm_json=False):
    ts = record.get("ts_ms", 0)
    trace_id = record.get("trace_id", "")
    event = record.get("event", "")
    channel = record.get("channel", "")
    chat_id = record.get("chat_id", "")

    color = EVENT_COLORS.get(event, "")
    ts_str = format_timestamp(ts)

    header = f"{color}[{ts_str}] {event}{RESET}"

    print(f"{header}")
    if channel or chat_id:
        print(f"  channel: {channel}, chat_id: {chat_id}")

    detail = format_event_data(event, record, show_tools=show_tools, show_llm_json=show_llm_json)
    if detail:
        for line in detail.split("\n"):
            print(f"  {line}")

    if show_json:
        print("  --- Full JSON ---")
        print("  " + json.dumps(record, ensure_ascii=False).replace("\n", "\n  "))

    print()


def process_file(file_path, show_json=False, show_tools=False, show_llm_json=False):
    print(f"{BOLD}{'='*60}")
    print(f"  Trace Log Beautifier")
    print(f"  File: {file_path}")
    print(f"{'='*60}{RESET}\n")

    records = []
    with open(file_path, "r", encoding="utf-8") as f:
        for line_num, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                record = json.loads(line)
                records.append(record)
            except json.JSONDecodeError as e:
                print(f"Error parsing line {line_num}: {e}")

    if not records:
        print("No valid records found.")
        return

    print(f"Total records: {len(records)}")

    trace_ids = set(r.get("trace_id", "") for r in records)
    print(f"Unique trace_ids: {len(trace_ids)}\n")

    for record in records:
        print_trace_record(record, show_json, show_tools=show_tools, show_llm_json=show_llm_json)


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description="Beautify chat trace JSONL files")
    parser.add_argument("file", nargs="?", default="chat_trace.jsonl", help="Input trace file")
    parser.add_argument("--json", "-j", action="store_true", help="Show full JSON for each record")
    parser.add_argument("--tools", action="store_true", help="Show tool names for tools events")
    parser.add_argument("--llm-json", action="store_true", help="Pretty-print llm_request/llm_response_raw JSON")
    parser.add_argument("--verbose", "-v", action="store_true", help="Enable all detailed output (equivalent to --tools --llm-json)")

    args = parser.parse_args()

    show_tools = args.verbose or args.tools
    show_llm_json = args.verbose or args.llm_json

    file_path = Path(args.file)
    if not file_path.exists():
        print(f"File not found: {file_path}")
        sys.exit(1)

    process_file(file_path, args.json, show_tools=show_tools, show_llm_json=show_llm_json)
