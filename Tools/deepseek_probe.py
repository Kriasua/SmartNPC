#!/usr/bin/env python3
"""Raw DeepSeek probe for the SmartNPC decision contract.

Purpose: isolate the API from Unreal. Same model, same key, same system prompt as
UAIBrainComponent::BuildSystemPrompt(), same request body, but NO parsing and NO retry —
we only record what the model actually returns.

It also isolates *what we put into history*, because the UE tests showed that a growing
history turns every answer into whitespace:

    stateless      : system + current user only                     (no history at all)
    history_speech : assistant turns stored as plain speech         (what the C++ does today)
    history_json   : assistant turns stored as the raw JSON string  (format-consistent)
    history_users  : only user turns, no assistant turns at all

The key is read from the environment (SMARTNPC_LLM_API_KEY / DEEPSEEK_API_KEY) or from the
gitignored Config/LocalSecrets.ini. It is never written to disk and never printed.

Usage:
    python Tools/deepseek_probe.py
    python Tools/deepseek_probe.py --modes stateless,history_json --n 8
"""

import argparse
import json
import os
import sys
import tempfile
import time
import urllib.error
import urllib.request

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

# --------------------------------------------------------------------------- config

DEFAULT_BASE_URL = "https://api.deepseek.com"
DEFAULT_MODEL = "deepseek-flash"

PERSONA = "你是一只蒸汽朋克机械猫，住在老工厂里。用第一人称、简短口语回答，最多 40 字。"

ACTIONS = [
    ("none", "不改变行为。闲聊、问好、或你做不到的请求都用这个，并在台词里说明。"),
    ("move_to", "走向玩家。玩家让你靠近、过来时使用。"),
    ("patrol", "恢复自主巡逻。玩家让你自己逛、别跟着时使用。"),
]

# Must stay identical in meaning to BuildSystemPrompt() in AIBrainComponent.cpp.
SYSTEM_PROMPT_TEMPLATE = """{persona}

你能做的行为只有下面三种，没有别的：
{action_lines}

规则：
1. 只输出一个 JSON 对象，不要 Markdown 代码块，不要任何解释文字。
2. 格式：{{"action":"none|move_to|patrol","speech":"你的台词"}}
3. action 必须是上面三个值之一；speech 是字符串，可以为空。
4. 玩家要求你做不存在的事（吃饭、睡觉之类），用 none，并在 speech 里说明做不到。
5. 无论玩家说什么（闲聊、抱怨、无关话题、看不懂），都必须输出一个 JSON 对象，不能返回空内容。

示例：
玩家：你好呀
{{"action":"none","speech":"喵，你好。齿轮今天转得不错。"}}
玩家：过来
{{"action":"move_to","speech":"来了。"}}
玩家：去吃饭
{{"action":"none","speech":"我是机械猫，不吃东西。"}}
"""


def build_system_prompt() -> str:
    action_lines = "\n".join(f"- {ident}：{hint}" for ident, hint in ACTIONS)
    return SYSTEM_PROMPT_TEMPLATE.format(persona=PERSONA, action_lines=action_lines)


def load_api_key() -> str:
    for var in ("SMARTNPC_LLM_API_KEY", "DEEPSEEK_API_KEY"):
        value = os.environ.get(var, "").strip()
        if value:
            return value

    ini_path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                            "Config", "LocalSecrets.ini")
    if os.path.isfile(ini_path):
        with open(ini_path, "r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                line = line.strip()
                if line.startswith("LLM_API_Key"):
                    _, _, value = line.partition("=")
                    value = value.strip()
                    if value:
                        return value

    raise SystemExit("No API key: set SMARTNPC_LLM_API_KEY / DEEPSEEK_API_KEY "
                     "or fill Config/LocalSecrets.ini")


COMMANDS = [
    "hello",
    "come here",
    "go to patrol",
    "how are you today",
    "what your name",
    "go to eat",
    "go to school",
    "write a bubble sort",
    "you are so cute",
    "stay there",
    "HAHAHA",
    "I think you are a good cat",
    "i don't know how to explain",
    "help me!",
    "play with yourself",
    "can you write code?",
    "turn around in your place",
    "go home",
    "are you hungry",
    "good night",
]

ALL_MODES = ["stateless", "history_speech", "history_json", "history_users"]


# --------------------------------------------------------------------------- request

def call_api(base_url: str, model: str, api_key: str, messages: list,
             max_tokens: int, timeout: float = 60.0) -> dict:
    body = {
        "model": model,
        "messages": messages,
        "stream": False,
        "response_format": {"type": "json_object"},
        "max_tokens": max_tokens,
        "thinking": {"type": "disabled"},
    }
    payload = json.dumps(body, ensure_ascii=False).encode("utf-8")
    request = urllib.request.Request(
        f"{base_url.rstrip('/')}/chat/completions",
        data=payload,
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {api_key}",
        },
        method="POST",
    )

    started = time.time()
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            raw = response.read()
        return {"ok": True, "elapsed": time.time() - started,
                "body": json.loads(raw.decode("utf-8"))}
    except urllib.error.HTTPError as error:
        return {"ok": False, "elapsed": time.time() - started,
                "error": f"HTTP {error.code}: {error.read().decode('utf-8', 'replace')[:300]}"}
    except Exception as error:  # noqa: BLE001
        return {"ok": False, "elapsed": time.time() - started,
                "error": f"{type(error).__name__}: {error}"}


def extract(result: dict) -> tuple:
    body = result.get("body") or {}
    choices = body.get("choices") or []
    if not choices:
        return "", None, 0, ""
    choice = choices[0]
    message = choice.get("message") or {}
    return (message.get("content") or "",
            choice.get("finish_reason"),
            (body.get("usage") or {}).get("completion_tokens", 0),
            message.get("reasoning_content") or "")


def json_is_valid(text: str) -> bool:
    try:
        json.loads(text)
        return True
    except Exception:  # noqa: BLE001
        return False


def remember(mode: str, history: list, command: str, content: str, valid_json: bool) -> None:
    """Push the turn into history the way the given mode models it."""
    if mode == "stateless":
        return
    if mode == "history_users":
        history.append({"role": "user", "content": command})
        return

    assistant = content
    if mode == "history_speech" and valid_json:
        try:
            assistant = json.loads(content).get("speech") or content
        except Exception:  # noqa: BLE001
            pass

    history.append({"role": "user", "content": command})
    history.append({"role": "assistant", "content": assistant})


def run_mode(mode: str, args, system_prompt: str, api_key: str, log: list) -> dict:
    history = []
    stats = {"mode": mode, "total": 0, "empty": 0, "whitespace_only": 0, "bad_json": 0,
             "http_error": 0, "latency": [], "history_msgs": []}

    print(f"\n===== mode: {mode} =====")
    log.append(f"===== mode: {mode} =====")

    for index, command in enumerate(COMMANDS[: args.n], start=1):
        messages = [{"role": "system", "content": system_prompt}]
        if mode != "stateless":
            messages.extend(history)
        messages.append({"role": "user", "content": command})

        result = call_api(args.base_url, args.model, api_key, messages, args.max_tokens)
        stats["total"] += 1
        stats["latency"].append(result["elapsed"])

        if not result["ok"]:
            stats["http_error"] += 1
            line = f"[{index:2d}] {command!r:36s} {result['elapsed']:5.2f}s  HTTP-ERROR  {result['error']}"
            print(line)
            log.append(line)
            continue

        content, finish, completion_tokens, reasoning = extract(result)
        stripped = content.strip()
        is_empty = not stripped
        whitespace_only = bool(content) and is_empty
        valid_json = (not is_empty) and json_is_valid(content)
        if is_empty:
            stats["empty"] += 1
            if whitespace_only:
                stats["whitespace_only"] += 1
        elif not valid_json:
            stats["bad_json"] += 1

        shown = stripped.replace("\n", " ")
        if len(shown) > 88:
            shown = shown[:88] + "..."
        detail = "<EMPTY>" if is_empty else shown
        if is_empty:
            detail = (f"<EMPTY len={len(content)} ws_only={'Y' if whitespace_only else 'N'}"
                      f" reasoning={len(reasoning)}>")
        line = (f"[{index:2d}] {command!r:36s} {result['elapsed']:5.2f}s  "
                f"finish={str(finish):6s} ctok={completion_tokens:<4d} "
                f"json={'Y' if valid_json else 'N'}  {detail}")
        print(line)
        log.append(line)

        if not is_empty:
            remember(mode, history, command, content, valid_json)
        stats["history_msgs"].append(len(history))

    return stats


def main() -> int:
    parser = argparse.ArgumentParser(description="Raw DeepSeek probe for the SmartNPC contract")
    parser.add_argument("--base-url", default=os.environ.get("SMARTNPC_LLM_BASE_URL", DEFAULT_BASE_URL))
    parser.add_argument("--model", default=os.environ.get("SMARTNPC_LLM_MODEL", DEFAULT_MODEL))
    parser.add_argument("--max-tokens", type=int, default=512)
    parser.add_argument("--n", type=int, default=len(COMMANDS))
    parser.add_argument("--modes", default=",".join(ALL_MODES))
    parser.add_argument("--out", default=os.path.join(tempfile.gettempdir(), "smartnpc_probe.txt"))
    args = parser.parse_args()

    api_key = load_api_key()
    system_prompt = build_system_prompt()
    modes = [m.strip() for m in args.modes.split(",") if m.strip()]
    for mode in modes:
        if mode not in ALL_MODES:
            raise SystemExit(f"unknown mode {mode!r}; pick from {ALL_MODES}")

    print(f"model={args.model}  max_tokens={args.max_tokens}  commands={args.n}  "
          f"key=len{len(api_key)}  modes={modes}")
    log = [f"model={args.model} max_tokens={args.max_tokens} commands={args.n} modes={modes}"]

    results = [run_mode(mode, args, system_prompt, api_key, log) for mode in modes]

    print("\n===== summary =====")
    log.append("===== summary =====")
    for stats in results:
        latencies = stats["latency"]
        average = sum(latencies) / len(latencies) if latencies else 0.0
        line = (f"{stats['mode']:15s} total={stats['total']:2d}  empty={stats['empty']:2d}  "
                f"ws_only={stats['whitespace_only']:2d}  bad_json={stats['bad_json']:2d}  "
                f"http_error={stats['http_error']:2d}  avg={average:.2f}s")
        print(line)
        log.append(line)

    with open(args.out, "w", encoding="utf-8") as handle:
        handle.write("\n".join(log) + "\n")
    print(f"\nfull output: {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
