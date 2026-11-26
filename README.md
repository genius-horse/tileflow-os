# TileFlow OS

TileFlow OS 是一个 Linux 操作系统课程项目。C++ 服务将图像按矩形切分，使用 Unix 域套接字分发给独立工作进程，在固定容量的 inode 镜像中提交结果，并把真实调度事件显示在浏览器里。

## 构建与启动

要求 Linux、C++17、CMake 3.16+、OpenCV 4.2+。在 Ubuntu 可安装 `build-essential cmake libopencv-dev python3`。HTTP 和 JSON 依赖随项目提供，前端在浏览器中直接运行。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 2
(cd build && ctest --output-on-failure)
./build/tileflow --image disk.img --port 8080
```

首次启动会初始化镜像，之后复用原镜像。浏览器访问 `http://127.0.0.1:8080`。远程运行时，在 VS Code Remote SSH 转发 8080，或在客户端执行 `ssh -N -L 8080:127.0.0.1:8080 YOUR_HOST` 后用同一地址访问。

| 角色 | 账号 | 演示密码 |
| --- | --- | --- |
| 普通用户 | `alice` | `alice-demo` |
| 普通用户 | `bob` | `bob-demo` |
| 管理员 | `admin` | `admin-demo` |

三个账号使用独立的浏览器配置或会话。可通过环境变量 `TILEFLOW_ALICE_PASSWORD`、`TILEFLOW_BOB_PASSWORD` 和 `TILEFLOW_ADMIN_PASSWORD` 更换密码。服务监听本机回环地址，适合通过 SSH 转发演示。

默认启动四个 worker，切块大小为 256 像素，镜像容量 256 MiB。用 `./build/tileflow --help` 查看参数。上传支持 PNG/JPEG，单文件最大 24 MiB，解码上限 16,777,216 像素。操作为灰度、反色和 5×5 高斯模糊，输出为 PNG。

## 现场演示

1. 以 `--tile-size 128 --demo-delay-ms 3000` 启动演示。管理员登录，点「运行样例」提交普通优先级灰度作业。
2. 普通队列有等待图块时，直接点「插入高优」。队列卡会显示插入后的首次实际派发，以及接手的 PID。也可在不同浏览器会话分别登录 Alice 和 Bob，通过「新建任务」提交各自的任务。
3. 点忙碌 PID 旁的「终止」并确认。工作台会在收到退出事件后定位实际受影响的图块，显示入队和重试状态。在 Linux 终端也可以用 `kill -KILL PID` 终止该服务的某个忙碌 worker。
4. 全部块提交后下载 PNG，用串行计算对照。正常停止服务，按原命令重新启动并重新下载。

```bash
./build/tileflow --serial grayscale web/demo-workbench.jpg /tmp/tileflow-reference.png
./build/tileflow --compare /tmp/tileflow-reference.png /path/to/download.png
```

## 自动验收与实验

```bash
python3 scripts/acceptance.py --binary build/tileflow --evidence acceptance.json
python3 scripts/acceptance.py --binary build/tileflow --benchmark --repeats 3 --evidence benchmark.json
```

验收脚本启动独立的临时服务和镜像，检查账户权限、四进程并行、按优先级派发、两种进程终止方式、结果去重、串行逐像素对照以及重启后下载。策略实验关闭演示延时，用相同作业、进程数与提交脚本比较 FIFO 和优先级调度，记录各轮实际等待与完成时间。

在 Linux x86-64、GCC 9.4、OpenCV 4.2.0、4 个 worker 的环境中，文件系统与图像单元测试均通过，三种操作的集成测试输出均为 0 个差异像素，网页终止与终端终止均成功重派。4 MiB 镜像耗尽后，失败作业的下载返回 404，此前成功作业在重启前后都能下载，字节一致。可复查 [完整验收事件](docs/evidence/acceptance.json)、[镜像空间耗尽结果](docs/evidence/disk-full.json) 和 [原始策略实验](docs/evidence/benchmark.json)。

| 3 轮中位数，关闭演示延时 | 高优先 | 全局 FIFO |
| --- | ---: | ---: |
| 高优作业等待首次派发 | 4 ms | 148 ms |
| 高优作业提交至完成 | 6 ms | 372 ms |
| 普通作业提交至完成 | 385 ms | 382 ms |
| 整批作业耗时 | 385 ms | 385 ms |

此组实验先提交 3073×2049 模糊作业，在观察到普通队列积压后再提交 129×97 灰度作业。等待以服务端接纳到首次派发的事件时间为准，表中的毫秒数包含该负载下的进程通信及镜像写入影响。

英文 [课程报告](docs/TileFlow%20OS%20Course%20Report.docx) 与 [答辩幻灯片](docs/TileFlow%20OS%20Defense.pptx) 收录了架构、现场演示步骤、实测截图和已知限制。提交前请填入真实组号、成员及课程信息，确认各组员实际贡献。

## 实现

- **调度与 IPC**：`src/service.cpp` 按图块从两条 FIFO 队列派发，默认高优先，同级以入队顺序为准。每个槽位持有一条 Unix stream 套接字、实际的子进程 PID 和至多一个执行尝试。退出时用 `waitpid` 回收并重新派发未提交的图块。完整回包通过尝试编号、尺寸和字节数校验，写入镜像后才增加完成数。
- **图像**：`src/worker.cpp` 用 OpenCV 完成解码、运算和编码。5×5 高斯模糊从原图提取额外两像素，回传中心区域。最终图像从镜像中已提交的图块按坐标重建。
- **文件系统**：`src/disk.cpp` 实现 4096 字节块、超级块、inode 表、数据块位图、目录项与直接/间接块。服务端独占镜像。创建或替换先为完整内容分配空间并写入，再提交 inode 等元数据，镜像空间不足时回滚尚未提交的分配。每个作业位于 `/jobs/ID`，保存上传字节、`meta.json`、已提交图块和 `final.png`。
- **可视化**：页面轮询服务端状态和递增事件编号。Canvas 从已落盘的 PNG 加载图块，时间条与图块历史用服务端记录的真实 PID、执行时间及状态绘制。

镜像的元数据更新覆盖正常停止与镜像空间不足。整机断电和服务进程在磁盘元数据写入中途被强杀超出这个文件系统的一致性保证。事件流保留当前运行最近 4096 条，图块尝试历史随作业元数据保存。假如宿主磁盘写入也失败，应保留镜像备份用于排查。

## 依赖与参考

- [cpp-httplib](https://github.com/yhirose/cpp-httplib) 提供 HTTP 传输和 multipart 解析，MIT。固定版本及许可证位于 `third_party/`。
- [nlohmann/json](https://github.com/nlohmann/json) 提供 JSON 读写，MIT。固定版本及许可证位于 `third_party/`。
- [OpenCV](https://github.com/opencv/opencv) 提供图像运算，按实际链接的版本适用其 [许可证](https://opencv.org/license/)。
- [xv6 磁盘结构](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.h) 与 [块分配实现](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.c) 用作文件系统结构参考。[Ray 任务状态](https://docs.ray.io/en/latest/ray-observability/reference/doc/ray.util.state.common.TaskState.html) 用作区分任务与执行尝试的参考。
- 进程泳道、任务色块、队列计数和渐进拼图的布局参考 [Perfetto](https://perfetto.dev/docs/visualization/perfetto-ui#track-event-selections)、[Dask Dashboard](https://docs.dask.org/en/stable/dashboard.html#task-stream)、[bull-board](https://github.com/felixmosh/bull-board) 和 [Leaflet rastercoords](https://commenthol.github.io/leaflet-rastercoords/)。工作台使用原生 CSS、Canvas 和 JavaScript。
- 登录封面和两种演示样例取自同一张内置 ImageGen 图。普通图位于 `web/demo-workbench.jpg`，高优图位于 `web/demo-priority.jpg`。画面提示词描述了蓝调时刻的雪峰、松林与静水，铜橙和淡紫晚霞倒映在湖面，并要求画面有均匀分布的纹理。
