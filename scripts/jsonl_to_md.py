#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Any, Dict, Iterable, List, Optional, Tuple

#
# Renderer goals (based on llm_trace.*):
# - All events include: ts_ms, trace_id, event.
# - Some fields are huge strings (prompt / tools_json / json / text).
# This script renders trace JSONL into Markdown that is scannable:
#   - grouped by trace_id
#   - readable timestamps (keep original ms)
#   - per-trace timeline table
#   - each record collapsible
#

FOLD_KEYS = ("prompt", "tools_json", "json", "text")
PREVIEW_KEYS = ("user_input", "error", "last_error")


def _best_fence(text: str) -> str:
    return "~~~" if "```" in text else "```"


def _one_line(s: str, limit: int = 120) -> str:
    s = " ".join(s.replace("\r\n", "\n").replace("\r", "\n").splitlines()).strip()
    if len(s) <= limit:
        return s
    return s[: limit - 1] + "…"


def _iter_jsonl(path: str) -> Iterable[Tuple[int, Any]]:
    """
    Iterate a file as JSONL (one JSON value per non-empty line).

    If the file is not valid JSONL (e.g. a multi-line JSON file), fall back to
    parsing the whole file as a single JSON value.
    """
    try:
        with open(path, "r", encoding="utf-8") as f:
            for i, ln in enumerate(f, 1):
                ln = ln.strip()
                if not ln:
                    continue
                yield i, json.loads(ln)
        return
    except json.JSONDecodeError:
        pass

    with open(path, "r", encoding="utf-8") as f:
        content = f.read().strip()
        if not content:
            return
        yield 1, json.loads(content)


def _format_ts_ms(ts_ms: Any) -> Optional[str]:
    try:
        ms = int(ts_ms)
    except Exception:
        return None
    dt = datetime.fromtimestamp(ms / 1000.0, tz=timezone.utc).astimezone()
    base = dt.strftime("%Y-%m-%d %H:%M:%S")
    ms_part = f"{ms % 1000:03d}"
    tz_part = dt.strftime("%z")
    tz_part = f"{tz_part[:3]}:{tz_part[3:]}" if len(tz_part) == 5 else tz_part
    return f"{base}.{ms_part} {tz_part} ({ms} ms)"


def _format_json(v: Any) -> str:
    return json.dumps(v, ensure_ascii=False, indent=2, sort_keys=False)


def _try_parse_json_str(s: str) -> Optional[Any]:
    s2 = s.strip()
    if not s2:
        return None
    if not (s2.startswith("{") or s2.startswith("[") or s2.startswith('"')):
        return None
    try:
        return json.loads(s2)
    except Exception:
        return None


@dataclass
class Record:
    line: int
    obj: Any
    trace_id: str
    event: str
    ts_ms: Optional[int]


def _as_records(inp: str) -> List[Record]:
    out: List[Record] = []
    for line, obj in _iter_jsonl(inp):
        if isinstance(obj, dict):
            trace_id = str(obj.get("trace_id") or "")
            event = str(obj.get("event") or "")
            ts_ms = None
            try:
                if obj.get("ts_ms") is not None:
                    ts_ms = int(obj["ts_ms"])
            except Exception:
                ts_ms = None
            out.append(Record(line=line, obj=obj, trace_id=trace_id, event=event, ts_ms=ts_ms))
        else:
            out.append(Record(line=line, obj=obj, trace_id="", event="", ts_ms=None))
    return out


def _record_title(obj: Dict[str, Any], line: int) -> str:
    if not isinstance(obj, dict):
        return f"L{line}"

    parts: List[str] = []
    ts_fmt = _format_ts_ms(obj.get("ts_ms"))
    if ts_fmt:
        parts.append(ts_fmt.split(" (", 1)[0])
    if obj.get("event"):
        parts.append(str(obj["event"]))
    if obj.get("iteration") is not None:
        parts.append(f"iter={obj.get('iteration')}")
    return " · ".join(parts) or f"L{line}"


def _write_kv(out, k: str, v: Any) -> None:
    if v is None:
        return

    if k == "ts_ms":
        ts = _format_ts_ms(v)
        out.write(f"- **ts**: {ts or v}\n\n")
        return

    if isinstance(v, str):
        # If this is a folded key, try parsing JSON string for nicer rendering.
        if k in FOLD_KEYS:
            parsed = _try_parse_json_str(v)
            summary = f"{k}: {_one_line(v, 100)}" if v.strip() else k
            out.write("<details>\n")
            out.write(f"<summary><code>{summary}</code></summary>\n\n")
            out.write("<div style=\"margin-left: 1em;\">\n\n")
            if parsed is not None and not isinstance(parsed, str):
                txt = _format_json(parsed)
                fence = _best_fence(txt)
                out.write(f"{fence}json\n{txt}\n{fence}\n\n")
            else:
                fence = _best_fence(v)
                out.write(f"{fence}text\n{v}\n{fence}\n\n")
            out.write("</div>\n\n")
            out.write("</details>\n\n")
            return

        if "\n" in v or len(v) > 160:
            fence = _best_fence(v)
            out.write(f"- **{k}**:\n\n{fence}text\n{v}\n{fence}\n\n")
        else:
            out.write(f"- **{k}**: {v}\n\n")
        return

    # Non-string -> JSON
    txt = _format_json(v)
    fence = _best_fence(txt)
    out.write(f"- **{k}**:\n\n{fence}json\n{txt}\n{fence}\n\n")


def _write_record_details(out, rec: Record) -> None:
    if not isinstance(rec.obj, dict):
        fence = _best_fence(str(rec.obj))
        out.write(f"{fence}\n{rec.obj}\n{fence}\n\n")
        return

    obj: Dict[str, Any] = rec.obj

    # Minimal meta (avoid repeating what is already in section title/table)
    meta_bits: List[str] = []
    # Skip ts_ms - already shown in the summary line
    if isinstance(obj.get("ts"), str) and obj.get("ts"):
        meta_bits.append(f"**ts**={obj.get('ts')}")
    if isinstance(obj.get("channel"), str) and obj.get("channel"):
        meta_bits.append(f"**channel**={obj.get('channel')}")
    if isinstance(obj.get("chat_id"), str) and obj.get("chat_id"):
        meta_bits.append(f"**chat_id**={obj.get('chat_id')}")
    if meta_bits:
        out.write("- " + " · ".join(meta_bits) + "\n\n")

    # Preview (only if present)
    for k in PREVIEW_KEYS:
        v = obj.get(k)
        if isinstance(v, str) and v.strip():
            out.write(f"- **{k}**: {_one_line(v, 220)}\n")

    # Payload keys: skip common/meta keys so record content is not repetitive
    skip = set(PREVIEW_KEYS) | {"event", "trace_id", "ts_ms", "ts", "channel", "chat_id", "iteration"}
    payload_keys = [k for k in obj.keys() if k not in skip]
    for k in sorted(payload_keys):
        _write_kv(out, k, obj.get(k))


def _has_substantial_content(obj: Dict[str, Any]) -> bool:
    """Check if record has content worth expanding (beyond just ts_ms)."""
    if not isinstance(obj, dict):
        return True
    skip = {"event", "trace_id", "ts_ms", "ts", "channel", "chat_id", "iteration"}
    for k in obj.keys():
        if k not in skip:
            return True
    return False


def _write_trace_group(out, trace_id: str, recs: List[Record]) -> None:
    recs_sorted = sorted(recs, key=lambda r: (r.ts_ms is None, r.ts_ms or 0, r.line))

    # Calculate summary info for the trace
    ts_list = sorted([r.ts_ms for r in recs if r.ts_ms is not None])
    start = ""
    dur = ""
    if ts_list:
        start = (_format_ts_ms(ts_list[0]) or "").split(" (", 1)[0]
        if len(ts_list) >= 2:
            dur = f"{ts_list[-1] - ts_list[0]} ms"

    # Fold entire trace group by default (with anchor for linking - use hidden span for better compatibility)
    anchor_id = trace_id.replace(":", "_").replace(".", "_").replace("-", "_") if trace_id else "none"
    out.write(f'<span id="trace-{anchor_id}"></span>\n')
    out.write("<details>\n")
    out.write(f"<summary><code>trace_id: {trace_id or '(none)'}</code> · <strong>{len(recs)} events</strong> · start: {start} · duration: {dur}</summary>\n\n")

    out.write(f"## trace_id: `{trace_id or '(none)'}`\n\n")

    # Timeline: each record as a collapsible item with rich summary
    # No separate table - the summary line itself contains all scannable info
    prev: Optional[int] = None
    for r in recs_sorted:
        o = r.obj if isinstance(r.obj, dict) else {}
        ts_fmt = _format_ts_ms(r.ts_ms) if r.ts_ms is not None else None
        ts_short = ts_fmt.split(" (", 1)[0] if ts_fmt else ""
        delta = ""
        if r.ts_ms is not None and prev is not None:
            delta = f"+{r.ts_ms - prev}ms"
        if r.ts_ms is not None:
            prev = r.ts_ms
        preview = ""
        if isinstance(o, dict):
            for k in PREVIEW_KEYS:
                v = o.get(k)
                if isinstance(v, str) and v.strip():
                    preview = f" · {_one_line(v, 60)}"
                    break

        # Level 2 indent: wrap each event (inside trace group) with 1em indent
        out.write("<div style=\"margin-left: 1em;\">\n")

        # Always use <details> for visual consistency, even if no content
        has_content = _has_substantial_content(o)
        out.write("<details>\n")
        out.write(f"<summary><code>L{r.line}</code> {ts_short} <code>{delta:>6}</code> <strong><code>{r.event}</code></strong>{preview}</summary>\n\n")
        if has_content:
            # Level 3 indent: content inside each event
            out.write("<div style=\"margin-left: 1em;\">\n\n")
            _write_record_details(out, r)
            out.write("</div>\n\n")
        else:
            # No content: show a subtle indicator
            out.write("<div style=\"margin-left: 1em; color: #666; font-style: italic;\">\n")
            out.write("(无额外内容)\n\n")
            out.write("</div>\n")
        out.write("</details>\n\n")

        out.write("</div>\n")

    out.write("</details>\n\n")


def _get_user_input(recs: List[Record]) -> str:
    """Get user_input preview from the first request_start event in a trace."""
    for r in recs:
        if isinstance(r.obj, dict) and r.obj.get("event") == "request_start":
            user_input = r.obj.get("user_input")
            if isinstance(user_input, str) and user_input.strip():
                return _one_line(user_input, 100)
    return ""


def main() -> int:
    ap = argparse.ArgumentParser(description="Render JSONL into a trace-friendly Markdown file.")
    ap.add_argument("input", help="Path to .jsonl")
    ap.add_argument("-o", "--output", default="", help="Output .md path or directory (default: traces_logs/<basename>.md)")
    args = ap.parse_args()

    inp = os.path.abspath(args.input)
    base = os.path.basename(inp)

    if args.output:
        outp = os.path.abspath(args.output)
        # If output is a directory, generate filename inside it
        if os.path.isdir(outp):
            outp = os.path.join(outp, f"{base}.md")
    else:
        # Default: output to traces_logs directory under workspace root
        script_dir = os.path.dirname(os.path.dirname(__file__))
        out_dir = os.path.join(script_dir, "traces_logs")
        outp = os.path.join(out_dir, f"{base}.md")

    os.makedirs(os.path.dirname(outp), exist_ok=True)

    records = _as_records(inp)
    by_tid: Dict[str, List[Record]] = {}
    for r in records:
        by_tid.setdefault(r.trace_id, []).append(r)

    now = datetime.now(timezone.utc).astimezone().isoformat(timespec="seconds")
    with open(outp, "w", encoding="utf-8") as out:
        out.write(f"# LLM trace render\n\n- **input**: `{inp}`\n- **generated**: `{now}`\n\n")

        out.write("## Summary\n\n")
        out.write("| trace_id | events | start | end | duration | preview |\n")
        out.write("|---|---:|---|---|---:|---|\n")
        for tid, recs in sorted(by_tid.items(), key=lambda kv: kv[0]):
            ts_list = sorted([r.ts_ms for r in recs if r.ts_ms is not None])
            start = ""
            end = ""
            dur = ""
            if ts_list:
                start = (_format_ts_ms(ts_list[0]) or "").split(" (", 1)[0]
                end = (_format_ts_ms(ts_list[-1]) or "").split(" (", 1)[0]
                if len(ts_list) >= 2:
                    dur = f"{ts_list[-1] - ts_list[0]} ms"
            user_input = _get_user_input(recs)
            anchor_id = tid.replace(":", "_").replace(".", "_").replace("-", "_") if tid else "none"
            out.write(f"| [`{tid or '(none)'}`](#trace-{anchor_id}) | {len(recs)} | {start} | {end} | {dur} | {user_input} |\n")

        out.write("\n---\n\n")

        for tid, recs in sorted(by_tid.items(), key=lambda kv: kv[0]):
            _write_trace_group(out, tid, recs)

    print(outp)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

