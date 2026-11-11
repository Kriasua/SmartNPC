"""Probe the local Ollama model with the SAME prompt UE builds, and measure latency.

Usage:
    python Tools/qwen_probe.py                      # 3 runs, default model
    python Tools/qwen_probe.py --runs 5 --question "go to the cube"
    python Tools/qwen_probe.py --model qwen3-8b:latest --max-tokens 4096

No third-party deps (urllib only).
"""
import argparse
import json
import time
import urllib.request

PERSONA = "你是一只蒸汽朋克机械猫，住在老工厂里。用第一人称、简短口语回答，最多 40 字。"

TOOLS = [
    ("query_world", "查看整个地图：所有可交互物、玩家位置、自己状态。需要知道环境时先调它。", "无参数"),
    ("move_to", "走向一个目标，走路期间会阻塞，到达或失败后返回结果。target 只能用 query_world 返回的 id。",
     '{"target": "目标 id，例如 player:0 或 actor:3"}'),
    ("idle", "停下、原地站住，什么都不做。玩家让你停手、别打了、等着时用它。", "无参数"),
    ("follow", "进入跟随状态：一直跟着玩家，不需要目标参数。", "无参数"),
    ("patrol", "进入巡逻状态：自己在巡逻点之间走动，不需要目标参数。", "无参数"),
    ("combat", "进入战斗状态，追击并开火。玩家说 fight / 打 / 开打 / 干架 / 挑衅你时用它——"
               "不要只是口头回应\"真要打？\"，必须真的切换状态。", "无参数"),
]


def build_system_prompt(state="idle", tools_used=0):
    lines = "\n".join(f"- {i}：{hint}  args：{args}" for i, hint, args in TOOLS)
    return (
        f"{PERSONA}\n\n"
        "你可以通过工具行动。每轮只输出一个 JSON 对象，二选一：\n"
        '1) {"type":"tool_call","tool":"<工具 id>","args":{...}}\n'
        '2) {"type":"final_answer","text":"你的台词"}\n\n'
        f"可用工具：\n{lines}\n"
        "规则：\n"
        "1. 只输出一个 JSON 对象，不要 Markdown 代码块，不要任何解释文字。\n"
        "2. 只能用上面列出的工具 id；args 的字段见工具说明。\n"
        "3. 以 [观察] 开头的消息是引擎返回的工具结果，不是玩家说的话。收到后必须继续输出一个 JSON。\n"
        "4. 需要知道周围有什么时先调用 query_world，再从返回的 nearby 里挑目标 —— 只能使用出现过的 id。\n"
        "5. 工具失败时看 error.hint 调整，不要重复完全相同的调用。\n"
        "6. 想说话或结束时用 final_answer；它表示本轮结束。\n"
        "7. 玩家要求你做不到的事（吃饭、睡觉之类），直接用 final_answer 说明做不到，不要调用工具。\n"
        "8. 即使看不懂玩家的话，也必须输出一个 JSON 对象，不能返回空内容。\n"
        "9. 玩家让你停下、别打了、住手、等着时，用 idle 工具原地停住。"
        '只有当"当前状态"已经是 idle 时，才可以直接用 final_answer 回应；否则必须调工具。\n'
        "10. 绝不能在没有调用工具的情况下声称完成了动作。玩家让你去某处、做某事时，"
        "本轮必须至少调用一次工具；复合命令（先 A 再 B）要逐个调用 move_to。\n\n"
        "示例一（直接回答）：\n玩家：你好呀\n"
        '{"type":"final_answer","text":"喵，你好。齿轮今天转得不错。"}\n\n'
        "示例二（先观察、再行动、再回答）：\n玩家：去那个球那边\n"
        '{"type":"tool_call","tool":"query_world"}\n'
        '[观察] {"tool":"query_world","ok":true,"status":"Succeeded","step":1,"elapsed_ms":9,'
        '"result":{"nearby":[{"id":"actor:3","name":"红球","distance_m":8.0,"direction":"front"}]}}\n'
        '{"type":"tool_call","tool":"move_to","args":{"target":"actor:3"}}\n'
        '[观察] {"tool":"move_to","ok":true,"status":"Succeeded","step":2,"elapsed_ms":4200,'
        '"result":{"target":"actor:3","final_distance_m":1.2}}\n'
        '{"type":"final_answer","text":"喵，到球边上了。"}\n'
        f"\n当前状态：{state}（玩家要求停止时，如果这里不是 idle，你必须调用 idle 工具）\n"
        f"本轮已执行工具：{tools_used} 次（如果这条指令需要移动或切换状态，你必须先调用工具，不能直接声称完成）\n"
    )


def call(url, model, system, question, max_tokens, temperature):
    body = {
        "model": model,
        "messages": [
            {"role": "system", "content": system},
            {"role": "user", "content": question},
        ],
        "max_tokens": max_tokens,
        "temperature": temperature,
        "stream": False,
    }
    req = urllib.request.Request(
        url, data=json.dumps(body).encode(), headers={"Content-Type": "application/json"}
    )
    t0 = time.perf_counter()
    with urllib.request.urlopen(req, timeout=300) as resp:
        payload = json.loads(resp.read().decode())
    return (time.perf_counter() - t0) * 1000.0, payload


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://localhost:11434/v1/chat/completions")
    ap.add_argument("--model", default="qwen3-8b:latest")
    ap.add_argument("--question", default="hello there")
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--max-tokens", type=int, default=4096)
    ap.add_argument("--temperature", type=float, default=0.0)
    args = ap.parse_args()

    system = build_system_prompt()
    print(f"model={args.model} question={args.question!r} runs={args.runs} "
          f"prompt_chars={len(system)} temp={args.temperature}\n")

    times = []
    for i in range(args.runs):
        try:
            ms, payload = call(args.url, args.model, system, args.question,
                               args.max_tokens, args.temperature)
        except Exception as exc:  # noqa: BLE001 - probe script
            print(f"[{i + 1}] FAILED: {exc}")
            continue

        times.append(ms)
        choice = payload["choices"][0]
        msg = choice.get("message", {})
        content = (msg.get("content") or "").strip()
        reasoning = (msg.get("reasoning") or "").strip()
        usage = payload.get("usage", {})
        print(f"[{i + 1}] {ms:7.0f} ms  finish={choice.get('finish_reason')}  "
              f"in={usage.get('prompt_tokens')} out={usage.get('completion_tokens')}")
        print(f"      content   = {content[:200]!r}")
        if not content:
            print(f"      !! empty content; reasoning={reasoning[:120]!r}")
        print()

    if times:
        times.sort()
        print(f"n={len(times)} min={times[0]:.0f} ms  median={times[len(times) // 2]:.0f} ms  "
              f"max={times[-1]:.0f} ms")


if __name__ == "__main__":
    main()
