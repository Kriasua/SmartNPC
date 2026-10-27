# CLAUDE.md

Guidance for agents working in this repository. Human-facing material lives in `README.md`
(what it is, how to run it, demo) and `Docs/` (deep dives); keep the audiences separate.

UE **5.6** C++ project, single runtime module `SmartNPC`: an LLM chat brain (`UAIBrainComponent`)
plus a traditional perception → blackboard → behavior-tree cat AI — two halves meant to become one
agent.

## Read scope — `.gitignore` is a hard boundary, not just hygiene

**Never read, glob, or grep anything under:** `Intermediate/` `Binaries/` `Saved/`
`DerivedDataCache/` `.vs/` `.idea/` `.codegraph/` `Plugins/Developer/RiderLink/`.

Macro-expanded UE build output (`.generated.h`, `.gen.cpp`) costs tens of thousands of tokens per
file and says nothing `Source/` does not. `grep` honours `.gitignore`, but **`glob` does not** — a
bare `*` or `**/*.h` walks ~13 GB of build output. Anchor glob patterns to a subdirectory
(`Source/**/*.cpp`), never to the repo root, and never `read` a path a glob handed you. When
delegating to a subagent, repeat this ban and give it only `Source/`, `Config/`,
`Content/<subdir>/` or `Docs/` paths — never an open "explore the repo".

`memory/` and `Docs/项目规划与求职路线.md` are gitignored personal notes: read them only when asked.

## Non-negotiable invariants

- **No credential may reach a serialized asset.** A key typed into a Details panel is saved into
  placed-actor data under `Content/__ExternalActors__/**`; that is how one was committed here.
  Keep key properties `Transient` and inject via `SMARTNPC_LLM_API_KEY` or the gitignored
  `Config/LocalSecrets.ini`. Enforced by mechanism, not intention: `Tools/Check-ForSecrets.ps1`
  (`-IncludeUntracked`, `-ScanHistory`, `-InstallHook`), installed with
  `git config --local core.hooksPath Tools/githooks`. Deleting a file or a commit is **not** a fix
  — revoke the key, because it stays in history.
- `*.generated.h` is the **last** include in a header, and is never hand-edited.
- Capture `this` in async work through `TWeakObjectPtr`; cancel in-flight HTTP in `EndPlay`.
- Latent BTTasks keep per-execution state in `NodeMemory`, never in members: node instances are
  shared by every NPC running the tree.
- Follow the UE conventions the code already uses (`U`/`F`/`b` prefixes, `TEXT("...")`, `int32`).

## Build

No test suite / lint / CI — everything goes through Unreal's tooling. Regenerate project files after
touching `.Build.cs`, `.Target.cs` or adding sources; the editor target is the usual one
(`-projectfiles` and the `SmartNPC` game target: `README.md`):

```powershell
& "C:\Program Files\Epic Games\UE_5.6\Engine\Build\BatchFiles\Build.bat" SmartNPCEditor Win64 Development -project="C:\MyResearch\UE_Projects\SmartNPC\SmartNPC.uproject" -waitmutex
```

## Map — pointers only; the source is authoritative

- **LLM brain** `Source/SmartNPC/{Public,Private}/AIBrainComponent.*`: appends the user turn, POSTs
  chat/completions, pops that turn again on *any* failure, trims to `MaxTurns*2`, and always replies
  through `BroadcastOnGameThread` → `OnLLMChatFinished`. History is in-memory only, with no system
  prompt and no streaming — all by design. The key is resolved per request by `GetLLMAPIKey()`
  (env var → gitignored ini → `SetLLMAPIKey()`), never from a serialized property.
- **Steampunk-cat AI**: `SteampuckCatAIController.*` (perception writes blackboard facts only; the
  misspelled name is the real class name), `SteampunkCatNPC.*` (pawn, patrol points, `FireLaser()`),
  `FindSearchPoint` / `BTTask_SetPatrolLocation` / `BTTask_SetStillPatrolPointLoc` /
  `NPCPlayAttackMontage` (BTTasks), `AttackLaserNotify` (montage notify → `FireLaser()`).
  Blackboard keys: `IsPlayerInSight`, `ChasingTarget`, `LastKnownLocation`, `NPCBehaviour`,
  `TargetLocation`, `SearchPoint`. Assets: `Content/NPC/LittleCat/{BehavierTree,BlackBoard,Enum}`
  (spelling as-is) and `Content/Sabertooth/`.
- Module dependencies: `Source/SmartNPC/SmartNPC.Build.cs`. Everything else: read the sources.
- New BTTasks belong to the same runtime module (`Public/` + `Private/`), then regenerate projects.
- `Content/` is mostly third-party (Epic's `Factory_LP`, the store's `Sabertooth`): never describe
  it as this project's own art.

## Docs

`Docs/项目现状与路线图.md` is the **current-state and roadmap entry point**: what is done, which
directions were decided or rejected (and why), the M1/M2 plan, and the outstanding P0 items. Read it
before proposing work — it exists to stop settled decisions from being re-litigated.

`Docs/AIBrainComponent详解.md` walks the component line by line (Chinese) — keep it in sync when the
component changes, source wins. No equivalent exists yet for the cat AI; the source is the reference.
