# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Unreal Engine **5.6** C++ project. A single runtime game module (`SmartNPC`) whose purpose is the `UAIBrainComponent` — an `ActorComponent` that gives an NPC a multi-turn LLM chat "brain" by calling an OpenAI-compatible chat/completions endpoint (default: Alibaba DashScope / Qwen) over HTTP.

Engine is bound via `EngineAssociation: "5.6"` in `SmartNPC.uproject`. The dev machine is Windows; paths and build commands below assume a standard `UE_5.6` install.

## Build / run

There is no test suite, lint step, or CI. Build and run go through Unreal's tooling, not a generic compiler.

Generate solution/project files after changing `.Build.cs`, `.Target.cs`, or adding source files:
```powershell
& "C:\Program Files\Epic Games\UE_5.6\Engine\Build\BatchFiles\Build.bat" -projectfiles -project="C:\MyResearch\UE_Projects\SmartNPC\SmartNPC.uproject" -game -engine
```

Build the editor target (most common during development):
```powershell
& "C:\Program Files\Epic Games\UE_5.6\Engine\Build\BatchFiles\Build.bat" SmartNPCEditor Win64 Development -project="C:\MyResearch\UE_Projects\SmartNPC\SmartNPC.uproject" -waitmutex
```

Build the standalone game target:
```powershell
& "C:\Program Files\Epic Games\UE_5.6\Engine\Build\BatchFiles\Build.bat" SmartNPC Win64 Development -project="C:\MyResearch\UE_Projects\SmartNPC\SmartNPC.uproject" -waitmutex
```

`SmartNPC.sln` (or the `.uproject` double-click → "Open with editor") also works. Targets: `SmartNPC` (Game) and `SmartNPCEditor` (Editor), defined in `Source/*.Target.cs`.

## Architecture

The module depends on `HTTP`, `Json`, and `JsonUtilities` (see `Source/SmartNPC/SmartNPC.Build.cs`) — these are the modules to keep in mind when touching the LLM code.

Everything of substance lives in `UAIBrainComponent` (`Source/SmartNPC/Public/AIBrainComponent.h`, `Private/AIBrainComponent.cpp`). The flow:

1. Blueprint calls `SendRequestToLLM(UserMessage)`. The component appends the user turn to in-memory `ConversationMessages`, builds the request JSON, and fires an async HTTP POST. `bLLMRequestInFlight` guards against concurrent requests; only one is in flight at a time.
2. `OnLLMHttpComplete` parses the response. On success it appends the assistant turn, calls `TrimRecentTurns()` (keeps the last `MaxTurns * 2` messages), and broadcasts. On any failure it **pops the user turn it just added** so history stays consistent, then broadcasts an error.
3. Results are always delivered via `BroadcastOnGameThread` → `OnLLMChatFinished` (a `BlueprintAssignable` delegate with `bool bSuccess, FString Message`). The marshal to the game thread exists because the HTTP callback may run off-thread; keep UI/Blueprint-facing work behind it.

Conversation history is **in-memory only** (`TArray<FLLMChatMessage>`), reset by `ClearLLMConversation()` and not persisted across sessions. There is no system prompt and requests are non-streaming (`stream: false`) by design — adding either means changing only the JSON-building / parsing sections of the `.cpp`, not the Blueprint pins.

Configuration is exposed as `UPROPERTY` fields editable in the editor Details panel under category `AI_System|LLM`: `LLM_BaseUrl` (no trailing `/chat/completions` — that's appended in code), `LLM_API_Key` (empty by default; set it in the editor, don't commit it), `LLM_Model`, and `MaxTurns`.

## Conventions

- Follow Unreal C++ conventions already in the code: `U`-prefixed `UObject` classes, `F`-prefixed structs, `b`-prefixed bools, `UPROPERTY`/`UFUNCTION`/`USTRUCT` reflection macros, `TEXT("...")` for string literals, `int32`/`FString`/`TArray`.
- `AIBrainComponent.generated.h` must be the **last** include in the header. Never hand-edit `*.generated.h`.
- Use UE smart pointers (`TSharedPtr`/`TSharedRef`, `MakeShared`) and `TWeakObjectPtr` when capturing `this` into async lambdas (as `BroadcastOnGameThread` does) to avoid dangling references after component destruction.
- Cancel/unbind any in-flight HTTP request in `EndPlay` (the component already does this) when adding new async work.

## Docs

`Docs/AIBrainComponent详解.md` is a detailed line-by-line walkthrough of `UAIBrainComponent` (in Chinese) aimed at readers new to UE C++. If you change the component's source, keep that doc in sync — source is authoritative.
