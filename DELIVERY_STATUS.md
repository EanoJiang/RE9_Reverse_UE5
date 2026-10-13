# LeonVideoPreview 清理结果

仅保留 `/Game/Leon/Preview/LeonVideoPreview` 一张地图和 243 个依赖包。

归档 34,644 个无用包，含其余 5 张地图。Content 从 30.07 GiB 缩减到 451.55 MiB。原始素材、工具副本和历史构建资料也已移出活跃工程。

Fresh UE 5.8.2 验证 23 项全部通过；243 个包全部加载成功且文件 SHA256 与清理前一致。DX12/SM6 实际窗口核对通过，三个动作按钮加载成功，毛发 Optimus 图已挂接。最后已恢复默认展示姿态并保持预览窗口打开。

归档路径：`E:\RE9_Leon_UE5_CleanupArchive_20261013_131111`。同盘归档不释放磁盘空间。恢复入口是 `Scripts/RestoreLeonVideoCleanup.ps1`。

本次没有重新保存关卡/资产，也没有修改毛发算法；保留的现有管线仍使用 UE HairStrands 完成最终光栅化/BSDF。
