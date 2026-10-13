# LeonVideoPreview

本工程只保留截图中的单角色里昂预览关卡：
`/Game/Leon/Preview/LeonVideoPreview`。

双击 `LaunchLeonPreview.cmd` 打开带左右控制面板的实时预览。
双击 `LaunchLeonEditor.cmd` 在 UE 5.8.2 编辑器中打开同一个关卡；进入 Play 后可使用运行时控制面板。
工程启动地图和游戏默认地图均已统一为这个关卡。

保留夹克角色、原材质/纹理、Native Groom、Binding、头部碰撞资源、自定义毛发 Shader、Optimus 图以及三个动作：待机、行走、整理头发。现有外观及模拟控制随关卡保留，本次只清理资源，不修改毛发算法。

Content 保留 UE AssetRegistry 核实的 243 个包。依赖扫描包含硬引用、软引用、管理引用和 searchable-name 引用，并额外包含代码动态加载的毛发图与三个动作。旧关卡及其他动画均已移出活跃工程。

清理归档：`E:\RE9_Leon_UE5_CleanupArchive_20261013_131111`。
归档保留排除的 Content、原始导出素材、工具副本、旧脚本/报告以及原配置；同盘移动没有释放磁盘空间。
详细清单见 `Scripts/leon_video_cleanup_plan.json`。关闭 UE 后可运行 `Scripts/RestoreLeonVideoCleanup.ps1` 恢复缺失的历史文件；该脚本不会覆盖现有工作，旧 Config 保留在归档中。

维护入口：

- `Scripts/AuditLeonVideoDependencies.py`：UE 中重新扫描依赖。
- `Scripts/VerifyLeonVideoCleanup.py`：只读验证保留包、唯一地图、三个动作、角色和 Groom。
- `Scripts/VerifySavedLeonVideoGroomMap.py`：验证保存后的 Groom 绑定和参数。
- `Scripts/BuildLeonREHairOptimusGraph.py`：需要重建毛发图时使用；会保存该资产。
- `Scripts/OpenLeonPreviewUI.py`：在编辑器中打开预览工具。

保留整个 `Plugins/LeonPreviewTools` 的 Source、Binaries 和 Shaders，以及 `Shaders/LeonREHairGuideWrite.usf`。它们参与现有毛发渲染和预览控制，不能作为无用缓存删除。
