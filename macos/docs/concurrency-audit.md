# 并发安全审查 —— 2026-07-31

对所有 `@unchecked Sendable` 类型与 actor 边界做的一次系统审查。**结论:无活的数据竞争。**

## 方法

`@unchecked Sendable` 是对编译器的手动承诺(「这个类我保证并发安全」),它**关掉**了编译器的
数据竞争检查。验证承诺是否兑现,最权威的手段是把 target 切到 **Swift 6 语言模式**跑一遍 ——
那是真正的数据竞争检查器,编过即证明安全,报错即真问题。

(先试过「摘掉注解看还编不编得过」,但本项目用 Swift 5 语言模式,它对 Sendable 检查宽松,
「编过」不能证明安全 —— 所以改用 v6 模式这个权威手段。)

## Swift 6 模式的结论:14 处标记,全部集中在两个边界,都不是活 bug

### 1. `ClashAPI`(actor)→ `ClashService`(MainActor)传 `[String: Any]`(12 处)

`ClashAPI` 这个 actor 把 `/connections`、`/proxies`、`/configs` 的 JSON 解析成
`[String: Any]` / `[[String: Any]]` 返回给 MainActor。`[String: Any]` 不是 `Sendable`,
因为 `Any` 理论上可能装引用类型。

**为什么当前安全**:`JSONSerialization` 产出的全是值类型(`String`/`NSNumber`/数组/字典),
且 actor 解析完就返回、**不保留引用**。所以跨 actor→MainActor 边界传的这份字典,发送方之后
不会再碰它 —— 不存在并发读写。只是这个不变式没被类型系统表达出来。

**将来 v6 迁移的正解**:让 `ClashAPI` 返回 **typed Sendable 结构体**(如 `struct Connection`)
而不是 `[String: Any]`。这会牵动 `ClashService` 的解析层(现在是 `as?` 取字典键),是个
中等规模的 refactor。收益是把「JSON 是值类型」这个事实固化进类型,杜绝未来有人给 actor
加一个「缓存上次解析结果」的字段时不小心引入真竞争。

### 2. `MacHelperClient.withProxy` 的 sending 闭包(2 处)

XPC 调用的通用封装把 `body` 闭包传进 `withCheckedThrowingContinuation`。v6 认为这个闭包
「可能与当前任务并发执行」。**当前安全**:闭包只被 XPC 的回复/错误回调触发一次(`OnceFlag`
保证 continuation 只 resume 一次)。v6 正解是给闭包标 `sending`,纯注解、无逻辑改动。

## 各 `@unchecked Sendable` 类型的安全依据(逐个核对过)

| 类型 | 同步手段 | 安全依据 |
|---|---|---|
| `SQLiteDatabase` | `DispatchQueue` | 所有 DB 访问串行化,WAL + FULLMUTEX |
| `HistoryStore` | `NSLock` | live/pending 状态全在锁内;查询走 DB 队列 |
| `Redirector` | `DispatchQueue` | 欺骗定时器 + 收尾全在同一串行队列(见其注释) |
| `OUIDatabase` | `NSLock` | 单例,懒加载 + 查询全在锁内 |
| `OnceFlag` | `NSLock` | 就是个原子 claim |
| `SpeedProbe` / `FileDownloader` | 串行 delegate 队列 | `maxConcurrentOperationCount=1`,状态只在该队列上碰 |
| `ConfigBuilder` / `SubscriptionStore` / `DeviceStore` / `SystemProxy` | MainActor 限定 | 均作为 `let` 存在 `@MainActor` 类里,只从 MainActor 访问;DB 访问再经 SQLiteDatabase 串行化 |

## 决定

**维持 Swift 5 语言模式,不现在迁 v6。** 理由:14 处标记全部可证今天安全,而彻底迁 v6
(主要是把 `ClashAPI` 的返回类型结构体化)是个中等 refactor,为「已安全」的东西冒引入 bug
的风险不划算。本文档把「v6 会标记什么、正解是什么」记清楚,将来真要迁时照此办即可。

**审查结论:无活的数据竞争。** `@unchecked Sendable` 的承诺逐个兑现了。
