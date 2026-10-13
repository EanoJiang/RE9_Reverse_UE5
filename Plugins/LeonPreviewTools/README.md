# Leon Preview Tools

UE 5.8.2 的运行时预览控制、自定义毛发 Shader / Optimus 接口与编辑器工具。
当前工程唯一关卡是 `/Game/Leon/Preview/LeonVideoPreview`。

编辑器工具通过 Window > Leon Preview 或控制台 `LeonPreview.Open` 打开。
实时预览用工程根目录 `LaunchLeonPreview.cmd` 启动。动作库现在仅包含待机、行走和整理头发。

必须保留本插件的 Source、Binaries、Shaders 以及工程根目录的 `Shaders/LeonREHairGuideWrite.usf`。
历史批量导入和 Variant 工具源码作为完整模块的一部分保留；相关关卡与额外资产已移到工程外归档。

需要编译插件时，可使用当前项目进行构建，无需已归档的 HostProject：

```powershell
& 'E:\epic_games\UE_5.8\Engine\Binaries\ThirdParty\DotNet\10.0\win-x64\dotnet.exe' 'E:\epic_games\UE_5.8\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.dll' UnrealEditor Win64 Development '-Project=E:\RE9_Leon_UE5\LeonCharacterPreview.uproject' '-plugin=E:\RE9_Leon_UE5\Plugins\LeonPreviewTools\LeonPreviewTools.uplugin' -NoUBA -NoHotReload -MaxParallelActions=1
```
