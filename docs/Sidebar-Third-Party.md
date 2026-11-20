# 上游和第三方组件

## MPC-HC

播放器源自 [clsid2/mpc-hc](https://github.com/clsid2/mpc-hc)，基于 2.8.5 标签。保留 [GPL v3 许可证](../COPYING.txt)、[原作者列表](Authors.txt) 以及各源文件的版权声明。

本仓库新增和修改的内容包括文件夹侧栏、目录模型、倍速手势、相关主窗口和工具栏处理、倍速提示样式及回归测试。公开快照没有改写上游作者署名。

## 发布运行组件

除修改后的 `mpc-hc.exe` 和本版说明／初始配置外，运行文件来自 [官方 2.8.5 x86 便携 ZIP](https://github.com/clsid2/mpc-hc/releases/download/2.8.5/MPC-HC.2.8.5.x86.zip)。

官方 ZIP 的 SHA256：`bc7fe059c08051feb9b94b2c1f52cf949aea00848676d3fa6c453cf18c79156b`。

- LAV Filters：[源码](https://github.com/clsid2/LAVFilters)，版本由 `.gitmodules` 及 Git 子模块引用固定。
- MPC Video Renderer：[源码和版本](https://github.com/Aleksoid1978/VideoRenderer)，官方 2.8.5 发布说明标注版本 0.11.2.2638。
- MediaInfo：[项目与许可](https://mediaarea.net/en/MediaInfo)，随官方运行包提供。
- 其他库的源码与许可声明位于本仓库 `src/thirdparty`、`include` 和相应子模块中。

各组件沿用其原有许可证，不能将整个包视为由本项目单独创作。未把本机安装器、卸载程序或使用记录混入运行包。

## FFmpeg 缩略图 helper

FFmpeg 没有捆绑在本发布包中。用户可以从 [FFmpeg 官方下载页](https://ffmpeg.org/download.html#build-windows) 选择适合系统的 Windows 构建，解压后将 `ffmpeg.exe` 放入 `SidebarTools`。

FFmpeg 的具体许可取决于构建选项，请保留所下载构建的许可证和来源说明。本项目通过独立进程调用 FFmpeg 生成本地视频缩略图。

随包保留的许可证文本见 docs/third-party-licenses：LAV Filters COPYING、MPC Video Renderer GPL v3 和 MediaInfo BSD 2-Clause。
