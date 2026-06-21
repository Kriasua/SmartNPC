# SmartNPC

> 基于 **Unreal Engine 5.6** 的「LLM 智能体 NPC」探索项目 —— 用大语言模型作为 NPC 的"大脑"，传统游戏 AI（感知 / 行为树 / 导航）作为"身体"，探索把生成式 AI 与游戏核心玩法深度融合。

## 简介

SmartNPC 是一个 UE 5.6 C++ 项目，目标是打造**有记忆、有性格、能动态决策**的智能体 NPC。
当前已实现一套完整的、由感知驱动的传统游戏 AI（追击 / 搜索 / 巡逻），并搭建了对接 OpenAI 兼容大模型的对话大脑组件；后续将二者融合成真正的 agent。

## ✨ 已实现功能

### 🤖 LLM 对话大脑（`UAIBrainComponent`）
- 多轮对话，调用 OpenAI 兼容的 `chat/completions` 接口（默认 Alibaba DashScope / Qwen）
- 异步 HTTP 请求，回调经 `BroadcastOnGameThread` 安全地回到游戏线程
- 对话历史管理与轮次裁剪（`MaxTurns`）
- `BaseUrl / Model / API Key / MaxTurns` 均在编辑器 Details 面板可配

### 🐈 SteampunkCat 智能 NPC（传统游戏 AI）
基于 **感知 → 黑板 → 行为树** 的经典架构：

| 状态 | 行为 |
|------|------|
| **追击 Chase** | 视觉感知到带 `Player` 标签的目标 → 持续跟随（MoveTo Actor 自动重寻路） |
| **搜索 Search** | 丢失目标后，前往最后已知位置，并以其为圆心、半径 r 内随机游走约 10 秒 |
| **巡逻 Patrol** | 无目标时在固定巡逻点间往返（ping-pong） |

三种状态通过黑板枚举 `NPCBehaviour` 驱动行为树分支切换。

## 🏗️ 架构

**感知与决策解耦**：AIController 只把"事实"写进黑板，行为树读黑板做决策，两者不直接通信。

```
玩家 / 环境
   │  AI Perception（视觉）
   ▼
AIController ──写事实──▶  Blackboard  ──读事实──▶  Behavior Tree ──▶ 移动 / 动画
                         IsPlayerInSight            (Chase/Search/Patrol)
                         ChasingTarget
                         LastKnownLocation
                         NPCBehaviour
```

**两层分工**（项目最终形态）—— LLM 当"指挥官"做高层决策，传统 AI 当"士兵"做每帧的可靠执行：

```
┌─ LLM 大脑（慢，偶尔思考，高层决策）─┐
│   追？搜？说什么？                  │
└──────────────┬─────────────────────┘
               ▼  写入 Blackboard
┌─ 行为树 / 状态树（快，每帧，稳定执行）┐
│   寻路 / 避障 / 播动画 / 到了没      │
└────────────────────────────────────┘
```

## 📂 核心代码

```
Source/SmartNPC/
├── AIBrainComponent.{h,cpp}              # LLM 对话大脑组件
├── SteampuckCatAIController.{h,cpp}      # 感知配置 + 感知→黑板 桥接
├── SteampunkCatNPC.{h,cpp}              # NPC 角色（巡逻点等）
├── FindSearchPoint.{h,cpp}             # BTTask：圆心半径内取随机搜索点
├── BTTask_SetStillPatrolPointLoc.{h,cpp} # BTTask：取下一个固定巡逻点
└── BTTask_SetPatrolLocation.{h,cpp}    # BTTask：随机巡逻点
```

## 🔧 技术栈

UE 5.6 · C++ · Behavior Tree · Blackboard · AI Perception · NavMesh

模块依赖：`HTTP` `Json` `JsonUtilities` `AIModule` `GameplayTasks` `NavigationSystem`

## 🚀 构建

生成工程文件（改动 `.Build.cs` / `.Target.cs` 或新增源文件后）：

```powershell
& "C:\Program Files\Epic Games\UE_5.6\Engine\Build\BatchFiles\Build.bat" -projectfiles -project="<路径>\SmartNPC.uproject" -game -engine
```

编译编辑器目标：

```powershell
& "C:\Program Files\Epic Games\UE_5.6\Engine\Build\BatchFiles\Build.bat" SmartNPCEditor Win64 Development -project="<路径>\SmartNPC.uproject" -waitmutex
```

> 配置 `LLM_API_Key` 请在编辑器 Details 面板填写，**不要提交到仓库**。

## 🗺️ Roadmap

1. **chatbot → agent**：让 LLM 输出结构化决策（tool-calling），驱动游戏行为
2. **记忆与性格**：持久化记忆（落盘 + 长期记忆摘要）+ 人格 system prompt
3. **多智能体**：NPC 之间互相感知、对话与协作
4. **工具链**：数据驱动配置 NPC 性格（让非程序员也能调教）
5. **性能**：LLM 调用异步队列 / 节流 / 缓存 / 按距离分级思考
