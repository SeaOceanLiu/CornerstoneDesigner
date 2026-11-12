Microsoft Visual C++ 2015-2022 x64 运行库安装说明
================================================

本发布包中的 CornerstoneDesigner.exe / CornerstoneLauncher.exe 使用 MSVC
动态运行库。目标机器若缺少对应运行库，程序可能提示找不到：

  MSVCP140.dll
  VCRUNTIME140.dll
  VCRUNTIME140_1.dll

适用系统：Windows 10 / Windows 11 x64。

一、推荐安装方法

1. 右键 install_vc_runtime.bat，选择“以管理员身份运行”。
2. 脚本会请求管理员权限、静默安装 vc_redist.x64.exe，并显示结果。
3. 如果提示需要重启，请重启 Windows 后再运行 CornerstoneLauncher.exe。

也可以直接双击 vc_redist.x64.exe，勾选许可协议后按向导安装。

二、静默安装

在“管理员”命令提示符中进入本目录，执行：

  vc_redist.x64.exe /install /quiet /norestart

常见退出码：

  0      安装成功
  3010   安装成功，需要重启
  1638   已安装更新或等效版本的运行库
  其他   安装失败，请查看系统事件或手动运行安装程序排查

三、注意事项

- 必须使用 x64 版本；本安装包只适用于 64 位 Windows。
- 建议使用随包提供的微软官方安装程序，不要手工复制 MSVCP140.dll /
  VCRUNTIME140.dll 等单个运行库文件。
- 不要在目标机器安装 Debug 版运行库（MSVCP140D.dll 等不可分发）。
- 安装程序需要管理员权限；普通用户可先联系管理员安装。
- 安装前请确认文件来自微软官方渠道，且数字签名为 Microsoft Corporation。
- 安装完成后可在“设置 → 应用”中查看 Microsoft Visual C++ 2015-2022
  Redistributable (x64)，或直接启动 CornerstoneLauncher.exe 验证。
