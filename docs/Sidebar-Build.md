# 构建 MPC-HC Sidebar

## 获取源码

克隆本仓库后，在仓库根目录执行：

```console
git submodule update --init --recursive
```

子模块版本由仓库固定，不要将子模块随意更新到最新版本。`tests` 子模块遵循上游的 `update = none` 设置；本项目的独立回归测试位于 `contrib`，不依赖它。

GitHub 自动生成的源码 ZIP 不含子模块源码。需要编译时，优先使用 Git 克隆并初始化子模块。

## 工具和构建

使用 Visual Studio 的 C++ 桌面工具、Windows SDK、ATL、MFC 和 NASM，按上游的 [Compilation.md](Compilation.md) 配置环境。`build.user.bat` 是各开发者自己的本机配置，不应提交或分享。

打开 `mpc-hc.sln`，选择 `Release / Win32` 并编译。输出为 `bin/mpc-hc_x86/mpc-hc.exe`。本版二进制以 Visual Studio 2022 构建；完整依赖配置与可选翻译、安装包步骤见上游文档。

发布构建只在 EXE 中记录 PDB 文件名，不记录构建机器上的完整 PDB 路径。公开发布时还应检查二进制中的其他绝对路径，不要包含 `.pdb`、`.obj`、本机配置、日志、缓存或播放记录。

侧栏缩略图 helper 是外部 FFmpeg 程序，放在运行目录 `SidebarTools/ffmpeg.exe`，无需链接到播放器中。

## 独立回归测试

在 Visual Studio x86 开发者命令提示符中，从仓库根目录执行：

```bat
mkdir bin\sidebar-tests
cl /nologo /EHsc /std:c++17 /W4 /utf-8 contrib\HoldSpeedGestureTests.cpp /Fe:bin\sidebar-tests\hold-tests.exe /Fo:bin\sidebar-tests\hold-tests.obj
bin\sidebar-tests\hold-tests.exe
cl /nologo /EHsc /std:c++17 /W4 /utf-8 /DNOMINMAX /DWIN32_LEAN_AND_MEAN contrib\FolderBrowserTests.cpp Shlwapi.lib User32.lib /Fe:bin\sidebar-tests\folder-tests.exe /Fo:bin\sidebar-tests\folder-tests.obj
bin\sidebar-tests\folder-tests.exe bin\sidebar-tests\fixture
```

文件夹测试的最后一个参数必须是尚不存在的目录。测试会在此生成少量夹具，完成后可删除 `bin/sidebar-tests`。再次运行前请使用新的夹具目录。

还应在实际播放器中检查侧栏显示、子目录展开、排序、滚轮与滚动条拖动、单击播放、重启后空列表，以及按住／拖动／松开倍速手势。

本版发布构建还使用编译器的确定性构建和路径映射选项，将诊断字符串中的源码／MFC 构建目录转换为相对路径。通过二进制扫描后才打包；编译输出和 PDB 文件不属于发布附件。
