# 契约 API 迁移

契约头文件以 Contract 对象统一提供 interface 和 level。exports、import options 和 runtime options 只持有契约，构造时不需要 variant。configuration 持有完整三元组，由具体实现解释。契约类型去掉命名空间中已经表达的前缀。任务输入与结果继续使用契约的 interface 和 level 作为类型与版本。

otter 的 F0、Note 和 Align 契约公共类型分别改为 `F0::L1`、`Note::L1` 和 `Align::L1` 下的 `Contract`、`Exports`、`ImportOptions`、`RuntimeOptions`、`StartInput`、`Result` 和 `Executive`。`F0::L1::Exports::read`、`Note::L1::Exports::read` 和 `Align::L1::Exports::read` 不再接收 variant。AnalysisTask 通过输入与结果的契约比较 payload type 和 version。

ContribSpecPayload 只提供 contract() 多态接口。各类 payload 保存自己的身份，configuration 从其完整三元组返回契约引用，不重复保存契约字符串。configuration 校验三项，其余 payload 校验 interface 和 level。

2026-11-17 验证：使用 synthrt 的 build/contract-identity-install，构建退出码为 0。28 项 CTest 通过，GameGgmlInterpreter 和 TifaGgmlInterpreter 的两项 RealEngine 测试跳过，无失败。
