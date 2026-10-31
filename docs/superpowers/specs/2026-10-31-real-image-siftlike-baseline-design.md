# 真实图片 SIFT-like 数据集与 V5 基线设计

## 目标

使用本机 `$HOME/Pictures` 的真实图片生成可复现的 250K×128 FP32+L2 数据集，保存到本地并传送到远端，随后用冻结 V5 跑四轮有效基线。该数据用于让图索引评测比归一化高斯数据更接近 SIFT 描述子分布，但不声称还原官方 SIFT250K 或预测官方成绩。

CAGRA 合规优化作为后续独立任务，不与数据生成和 V5 基线测量混合提交。

## 可用输入与依赖

本机有 194 张可解码 PNG/JPEG，其中 174 张宽高均至少 128 像素。使用现有 Pillow 和 NumPy，不安装 OpenCV、skimage 或其他依赖。原图只在本机读取；远端只接收派生的 `base.npy`、`queries.npy`、`truth.npy` 和 `manifest.json`。

## 描述子算法

实现确定性的单尺度 SIFT-like 描述子，而非完整 SIFT 关键点检测：

1. 图片转灰度 FP32，范围 `[0,1]`；仅使用宽高均至少 128 的图片。
2. 从 `{32,48,64,96}` 中确定性随机选择 patch 边长和位置，resize 到 32×32。
3. 使用可分离 `[1,4,6,4,1]/16` 核平滑，计算 x/y 梯度。
4. 按 4×4 位置单元和 8 个方向 bin 累加高斯加权梯度，形成 128 维非负描述子。
5. L2 归一化、将分量截断到 0.2 后再次归一化，模拟常见 SIFT 描述子后处理。
6. 拒绝梯度过低或范数退化的平坦 patch，并以固定 seed 重试，确保最终恰好 250,000 条 base。

这不是完整多尺度 SIFT，也不是官方数据重建；生成器和 manifest 必须明确记录这一限制。

## Base、Query 与 Ground Truth

- Base：从不同图片、位置、尺度的合格 patch 独立抽样，ID 为行号 `0..249999`。
- Query 共 4096 条：70% 从 base 已采样位置提取带确定性位置/尺度扰动的 held-out patch，30% 使用新的独立 patch。相关 query 不直接复制 base 向量。
- 所有向量保持 FP32，距离为平方 L2；ground truth 使用现有 `exact_topk(..., k=100)` 计算。
- 新数据集名为 `siftlike250k-d128`，distribution 为 `siftlike`，seed 为 `20261109`，不覆盖已有 gaussian/clustered 数据或基线。

## 缓存、传输与复现

本地缓存为 `/home/howxu/eval-data/siftlike250k-d128/`，使用现有评测器格式：

- `base.npy`：250000×128 float32
- `queries.npy`：4096×128 float32
- `truth.npy`：4096×100 int32
- `manifest.json`：DatasetSpec、各数组 SHA256、生成器版本、源图片数量及组合哈希

`contest_eval.py` 增加对应 spec 和生成路径，使缓存可校验、可重建；单元测试使用内存合成图片验证描述子，不依赖 Pictures 内容。`dataset_kind` 将该数据按 SIFT-like 质量门禁处理，但报告仍注明 surrogate。

将目录传送到远端 `/work/evals/datasets/siftlike250k-d128/`。远端通过相同 manifest 和数组哈希校验后才允许测试。

## V5 四轮基线

使用冻结 V5 `634888d9b037153fa143d2c913ba4b25503e1d62` 和已构建 trace binary。每轮使用新 server 目录：

- 构建计时为 DDL + 第一条 Top-10 查询，不含数据加载。
- Top-10 和 Top-100 各执行 4096 条独立 SQL。
- 要求真实 GPU serve、snapshot build 证据和无 CPU fallback；证据不完整轮次不计入平均，另跑替代轮次。
- 保存四个有效轮次的 ready、QPS、Recall、资源和二进制哈希，计算算术平均。

结果归档为 `results-contest-v5-siftlike-four-20261031.{json,md}`，同时列出 `23s、147/143 QPS、Recall≈1` 的外部参考及额外增强 10% 的值。该基线冻结后用于后续 CAGRA 优化对照。

## 验证

1. 描述子测试：输出 128 维、FP32、非负、单位范数；零 patch 被拒绝；固定输入和 seed 可复现。
2. 现有 contest evaluator、benchmark、bridge 和 adaptor component 回归通过。
3. 本地缓存形状、dtype、ID 范围、truth 和 SHA256 校验通过。
4. 远端重新校验缓存后完成四个有效 V5 轮次，否则不出平均值。
