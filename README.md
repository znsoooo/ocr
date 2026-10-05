# OCR: 本地离线文字识别（图片 / 视频）

识别图片或视频里的文字，输出**文字内容 + 每行文字的四点坐标**（倾斜文字也能贴合），
纯本地离线推理，不联网、不上传任何图片。

* 单一可执行文件：`ocr --image shot.png` 或 `ocr --video clip.mp4`
* 依赖只有 ONNX Runtime + 两个 stb 单头文件（**不需要 OpenCV / PaddlePaddle**）
* 模型与样例素材随目录附带，离线开箱可用
* Windows 开箱即用版见 `windows/`，源码在 `cpp/`

---

## 1. 目录与用法

```
ocr/
├── README.md                  ← 本文档（用法 + 实现记录）
├── 使用说明.txt                ← Windows 用户快速上手（面向非开发者）
├── 第三方组件来源.md           ← 每个外部文件从哪个网址下载的
├── PixPin技术路线.md           ← 调研与实现记录（归档快照，不再更新）
├── windows/                   ← 开箱即用的 Windows 版（exe + DLL）
├── models/                    ← 已附带的 ONNX 模型，开箱即用
│   ├── det_v4.onnx            4.53 MB  PP-OCRv4 检测 (DBNet)
│   ├── rec_v4.onnx           10.35 MB  PP-OCRv4 识别 (CRNN+CTC)，中英数字 6623 字
│   ├── keys_v4.txt           字典 (6623 行)
│   ├── det_v5.onnx / rec_v5.onnx / keys_v5.txt      PP-OCRv5 (18383 字，中英日韩通用性更好)
├── onnxruntime-win-x64-1.30.0/   ← 编译用的头文件 + 导入库（.lib / .a）
├── cpp/                       ← 源码（不依赖 OpenCV / PaddlePaddle）
│   ├── ocr_engine.h           公共 OCR 引擎：检测→后处理→识别→CTC→行分组
│   ├── video_io.h             视频探测 / 逐帧解码（ffmpeg 管道）
│   ├── main.cpp               ocr 的唯一入口：--image / --video 两种模式
│   ├── CMakeLists.txt         编译出单一可执行文件 ocr（MinGW-w64）
│   ├── build_windows.bat      一键编译
│   └── stb_image.h / stb_image_write.h    单头文件图像 IO
└── samples/
    ├── sample_ui.png          模拟软件界面截图（自带中文/英文/深色标题栏）
    ├── ch_en_num.jpg          真实商品图样例
    ├── game_dialogue.png      真实游戏截图（深底白字字幕 + 橙色说话人）
    └── test_video.mp4         测试视频：1280x720/25fps/5s，底部字幕每秒换一句
```

### 1.1 快速上手

**Windows 开箱即用**（不需要编译）：

```bat
cd windows
ocr.exe --image samples\sample_ui.png
ocr.exe --video samples\test_video.mp4 --every-seconds 0.5
```

**自己编译**：`cd cpp` 后运行 `build_windows.bat`（详见下一节）。

### 1.2 编译（MinGW-w64）

依赖只有 ONNX Runtime + 两个 stb 单头文件，**不需要 OpenCV，也不需要 PaddlePaddle**。

**Windows（推荐，用 MinGW-w64）**：

```bat
cd cpp
build_windows.bat
```

需要 MinGW-w64 的 `g++` / `mingw32-make` 在 PATH 里。两个最简单的获取方式：

- [w64devkit](https://github.com/skeeto/w64devkit/releases) —— 免安装，解压后运行 `w64devkit.exe`，在它打开的终端里执行脚本即可（g++/make/cmake 都自带）
- [MSYS2](https://www.msys2.org/) —— 打开 "MSYS2 UCRT64" 终端，执行
  `pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake`

ONNX Runtime 的头文件和导入库已随仓库附带（`onnxruntime-win-x64-1.30.0/`），不用另外下载。

手工等价命令：

```bat
cd cpp
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_CXX_COMPILER=g++ ^
      -DONNXRUNTIME_ROOT=..\onnxruntime-win-x64-1.30.0 -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
build\ocr.exe --image ..\samples\sample_ui.png --visual out.png --json out.json
```

**Linux**：

```bash
cd cpp
cmake -S . -B build -DONNXRUNTIME_ROOT=/path/to/onnxruntime-linux-x64-1.30.0
cmake --build build -j
./build/ocr --image ../samples/sample_ui.png --visual out.png
```

`CMakeLists.txt` 已处理两处平台差异：Windows 下自动把 `onnxruntime.dll` 拷到 exe 旁边；
Linux/macOS 给可执行文件写入 `RPATH` 指向 `libonnxruntime.so/.dylib`，无需设 `LD_LIBRARY_PATH`。
MinGW 下还会自动加 `-static -static-libgcc -static-libstdc++`，产出的 exe 不依赖任何 MinGW 的 DLL。

实测：同一张 760×460 截图，**4 线程下 176 ms**（含模型加载），识别出 15 行。

模型目录无需手工指定：程序会从 exe 所在目录和当前目录**逐级向上搜索** `models/`，
所以"源码目录里跑"、"`cpp/build/` 里跑"、"便携版里双击"都不用改参数；
全都找不到时直接报错退出（没有 `--model-dir` 这样的开关）。

### 1.3 用法：`ocr`（图片 / 视频二合一）

一个可执行文件，用 `--image` 或 `--video` 指定输入（二选一，必填其一）；
其余参数两种模式共用。

```bash
ocr --image shot.png                                        # 图片: 整幅识别
ocr --image shot.png --roi 0,0,0.5,1 --visual out.png        # 只识别左半幅
ocr --video clip.mp4                                        # 视频: 逐帧识别整帧
ocr --video clip.mp4 --every-seconds 0.5                    # 每 0.5 秒取一帧
ocr --video clip.mp4 --roi 300,780,960,200 --visual frames/  # 只认底部字幕区
```

| 参数 | 适用 | 说明 |
|---|---|---|
| `--image PATH` | | 图片文件（与 `--video` 二选一） |
| `--video PATH` | | 视频文件 |
| `--roi x,y,w,h` | 两者 | 感兴趣区域。**四个值全 ≤1 时自动按相对比例解释**（`0,0,0.5,1` = 左半幅），否则按像素（`300,780,960,200`）。默认整幅 |
| `--every-frames N` | 视频 | 每隔 N 帧取 1 帧，**默认 1（逐帧）** |
| `--every-seconds S` | 视频 | 每隔 S 秒取 1 帧（按真实时间戳选帧） |
| `--max-frames N` | 视频 | 最多处理 N 个抽样帧（调试用） |
| `--v5` | 两者 | 用 PP-OCRv5 模型（默认 PP-OCRv4） |
| `--unclip R` | 两者 | 文本框外扩系数，默认 2.0 |
| `--threads N` | 两者 | 推理线程数，默认 4 |
| `--min-score X` | 两者 | 过滤掉置信度低于 X 的行 |
| `--json PATH` | 两者 | 结果写 JSON（逐行文字 + 置信度 + 四点坐标） |
| `--visual PATH` | 两者 | 图片模式 = 输出一张画了识别框的 PNG 文件；视频模式 = 输出目录，逐帧存 PNG（目录不存在会自动创建） |

几点约定：

* **只输出识别到文字的帧**。视频模式下空白帧只计入汇总，不逐帧打印。
* **视频解码用 ffmpeg 管道**：ffmpeg 解成裸 RGB 帧从 stdout 出来直接读入 —— 不链接
  ffmpeg 开发库，任何 ffmpeg 版本都能用。程序会在**自身目录**和**系统 PATH** 里找
  `ffmpeg` / `ffprobe`，都找不到时直接报错（没有 `--ffmpeg` 这类开关）；
  把这两个 exe 放到程序旁边即可。
* **`--roi` 是性能关键**：ROI 会作为 `crop` 参数交给 ffmpeg，**管道里只传 ROI 像素**，
  既省带宽又大幅减少识别耗时 —— 实测视频模式同一帧整帧 160 ms → ROI 800×140 只要 75 ms；
  图片模式同理，整幅 130 ms → ROI 66 ms。
* **坐标一律是整幅 / 整帧坐标系**：ROI 裁掉后输出时会把偏移加回去，下游拿到 `(148,596)`
  可以直接在原画面上标注，不用自己换算。

实测（`samples/test_video.mp4`，1280×720 / 25fps / 5s，底部字幕每秒换一句）：

```bash
./build/ocr --video ../samples/test_video.mp4 --every-seconds 1 --roi 100,570,800,140
```

```
视频      : ../samples/test_video.mp4
画面      : 1280x720  平均 25.000 fps  时长 00:05.000  125 帧  最后一帧 PTS 4.960s
感兴趣区域: (100,570) 800x140  [像素]
抽帧间隔  : 每 1.000 秒 1 帧 (按真实时间戳选取, 共 5 帧)
模型      : PP-OCRv4
坐标说明  : 输出的四点坐标已加回 ROI 偏移, 即相对原帧左上角
---------------------------------------------------------------
[帧 000000] t=00:00.000  2 行
    #1  0.998  (148,596) (275,596) (275,636) (148,636)  莱茵多特
    #2  0.999  (149,648) (634,649) (634,686) (149,684)  别看她那副样子，似乎是在热心工作
[帧 000025] t=00:01.000  2 行
    #1  0.997  ...  莱茵多特
    #2  0.999  ...  其实她本质上是个随心所欲的女人。
...
---------------------------------------------------------------
抽样帧数  : 5
含文字帧  : 5
文字总行数: 10
耗时      : 0.4 s  (平均 83.9 ms/帧)
```

几个设计要点：

* **时间基准按视频类型自动选择，并且会打印走的是哪条路。**
  - **恒定帧率（CFR）**：每帧间隔相同，时间点可以直接由「帧号 ÷ 帧率」算出，于是
    **跳过全片 PTS 扫描**。判定要求两个信号同时成立——标称帧率与平均帧率一致（相对差 < 0.5%），
    且（时长与总帧数可读时）总帧数 ÷ 时长也对得上（相对差 < 1%）；任一信号可疑就退回扫 PTS，
    宁可多花时间也不让时间点不准。实测一个 30 秒 / 900 帧的视频，单独扫一遍 PTS 要 **0.71 s**，
    跳过之后整个流程只要 **0.75 s**。
  - **变帧率（VFR）**：帧间隔不均匀，「帧号 ÷ 平均帧率」会明显偏掉——实测一个屏幕录制
    视频最大偏差 **0.32 秒**。这类视频用 ffprobe 取一次全片逐帧 `best_effort_timestamp_time`，
    之后时间点直接查表；`--every-seconds` 也按这张时间网格选帧，保证抽样间隔稳定在 S 秒附近
    （实测 0.5 秒档实际间隔 0.500~0.534 秒）。
  - 头部信息的 `时间基准` 一行会明确写出这次走的是哪条、有没有完整扫描全片：

    ```
    时间基准  : 恒定帧率 25.000 fps -> 跳过 PTS 扫描, 时间点由帧号推算
    时间基准  : 变帧率 (标称 60.000 fps / 平均 26.692 fps) -> 已完整扫描全片 PTS, 303 帧
    时间基准  : 读不到逐帧 PTS -> 时间点按平均帧率 26.692 fps 估算
    ```
* **抽帧在本地按 PTS 判定，没有交给 ffmpeg 的 `select` 滤镜。** 因为 `select` 只能表达
  "每 N 帧"这类规则，表达不了"第 1.37 秒那一帧"这种时间网格；而丢弃帧的 `fread` 开销
  实测可忽略（整片读管道的时间远小于识别耗时）。顺带也避免了 `select` 滤镜里
  转义逗号在 Windows cmd / POSIX sh 下的引号差异。
* **帧序号是源视频的原始帧号**（0, 25, 50…），不是抽样序号，便于回溯到具体画面。
* **`--visual` 在视频模式存的是 ROI 那一块的图**（因为管道里只传 ROI），框按 ROI 局部坐标画。
* **性能提醒**：逐帧跑 1080p 视频约 100~200 ms/帧（4 线程），比实时慢数倍。
  长视频建议配合 `--roi` 缩小区域 + `--every-seconds` 抽帧；或者按需提高 `--threads`。

---

## 2. 踩坑记录（都是实跑出来的，不是抄的文档）

这三个坑任何人自己写一遍都会遇到，记录下来：

**坑 1：四点排序会自交。** `minAreaRect` 这类接口返回的 4 个角点是无序的，
常见写法「按 x 排序，再比较 y」在**两条竖边的 x 恰好相同**（极常见，横排文字框就是这种）时会算出自交四边形，
透视变换出来的裁剪图是一条 16px 宽的斜条，识别结果直接变空白。
正确做法是 PaddleOCR 的 `get_mini_boxes` 逻辑：先按 x 排序，再分别取"左边两个点中靠上的"作为左上、
"右边两个点中靠上的"作为右上。**本项目最初就踩了这个坑，已在 `order_points()` 里修正。**

**坑 2：最小外接矩形在参考边竖直时输出翻转的框。** 自己实现旋转卡壳时，如果直接用凸包边作为参考方向，
当参考边接近竖直（`atan2 ≈ ±90°`）时算出的局部坐标系是"x 轴朝上"的，四点顺序整体翻转 180°，
裁剪图变成倒影，识别结果同样为空。
修复方式：把参考角度归一到 `(-90°, 90°]`，若恰好为 90° 就换成等价的水平参考并交换宽高。
**这是实测时出现的漏检根因，修复后整行文字不再丢失。**

**坑 3：深色底浅色字会识别失败。** 深色标题栏、终端截图上的白字，识别网络直接输出空或低置信度单字。
本项目加了「反色重试」：裁剪图平均亮度 < 110 时，额外把图像反色再跑一次，**取两者置信度更高的结果**。
加上这一步后，样例里深色标题栏上的浅色文字从 0.07 分提升到 0.96 分正确识别。

**坑 4：置信度只能在"框内"算，不能在"文字像素上"算。** C++ 版最初用连通域像素的平均概率打分，
因为连通域是基于**膨胀后**的掩码取的，膨胀进来的低概率像素把均值拉低，导致整行文字被过滤掉。
改成 PaddleOCR 的 `box_score_fast`（最小外接矩形内的平均概率）后恢复正常。

**坑 5：`popen` 的模式字符串在 Windows 和 Linux 上是相反的。** 视频版通过管道读 ffmpeg 输出的
裸 RGB 帧，管道必须用**二进制**模式，否则 Windows 会把帧数据里的 `\n` 翻译成 `\r\n`，读出来的
图像直接错位。但 `"rb"` 在 Linux 上会让 `popen` **返回 NULL** —— glibc 只接受 `"r"`/`"w"`/`"re"`/`"we"`，
第二个字符不是 `e` 就直接 `EINVAL`。而 MSVC 的 `_popen` 恰恰需要 `"rb"`。
所以只能按平台分开写（`PIPE_READ_BIN`），Windows 上再补一句
`_setmode(_fileno(pipe), _O_BINARY)` 双保险。

**坑 6：模型目录解析要向上搜，不能只试固定相对路径。** 原来只按 `exe目录/models → ./models →
../models` 三个固定路径找模型，在 `cpp/build/` 里运行就全部落空。改成从 exe 目录和当前目录
**逐级向上搜 4~5 层**，这样在仓库根目录、`cpp/`、`cpp/build/`、`/tmp` 下运行、
以及便携版里双击，都能自动命中（已实测这 5 种情形）。

**坑 7：变帧率视频里「帧号 ÷ 平均帧率」算出来的时间点是错的。** 手机/屏幕录制出来的视频
常常是 VFR：容器标称 60fps，实际平均只有 26.7fps，帧与帧的真实间隔是不均匀的。
用平均帧率换算，实测**最大偏差 0.32 秒**，而且"每 0.5 秒取一帧"抽出来的帧间隔在
0.2~0.5 秒之间乱跳。正确做法是取逐帧真实 PTS（`ffprobe -show_entries frame=best_effort_timestamp_time`）
并**按时间网格选帧**，而不是按帧号取模。修复后同一段视频的时间点与 ffprobe 完全一致，
0.5 秒档的实际间隔稳定在 0.500~0.534 秒。

**坑 8：Windows cmd 对"以引号开头 + 后面还有第二组引号"的命令行会解析错。**
视频工具用 `_popen` 调 ffmpeg，我原来把程序名也加了引号：
`"ffprobe" -v error ... "video.mp4"` —— 这恰好同时命中 cmd 的两条歧义规则，
cmd 会把**第一个引号到最后一个引号之间的全部内容**当成可执行文件名，于是报出：

```
Can't recognize 'ffprobe" -v error ... "video.mp4' as an internal or external command
```

**这个 bug 在 Linux 上永远复现不出来**（sh 的引号规则完全不同），是我用 MinGW 交叉编译 +
wine 实跑 Windows 版时才抓到的。修法：程序名能不加引号就不加；路径带空格时先转 8.3 短路径
把空格消掉；实在躲不掉才用 `cmd /c ""prog" args"` 惯用写法整体再套一层引号。
顺带把默认程序名改成**优先在 exe 同目录找 ffmpeg.exe**，这样用户把 ffmpeg 拖进目录就能用。

**坑 9：不能让 ffmpeg 做帧率转换，否则帧号和时间戳会整体错位。**
最早的写法里有 `-vsync 0`（意思是"原样输出每一帧，不做帧率补偿"）。后来发现新版 ffmpeg
把它移除了（报 `Unrecognized option 'vsync'`），我一度认为这个选项**是多余的** ——
理由是"抽帧已经在本地按 PTS 做了"。

**这个判断是错的，而且当时的验证方式本身有缺陷。** 去掉选项后我只核对了**帧数**
（CFR 视频 125 帧、VFR 视频 303 帧，与 `nb_frames` 一致）就下了结论。但那个"303"是
**程序自己截断出来的**：PTS 表有 303 项，读到第 303 帧就 break 了，根本不是 ffmpeg 输出的帧数。

单独跑一遍 ffmpeg 才看清真相：

```
ffmpeg -i clip.mp4 -f rawvideo -pix_fmt rgb24 -            → 681 帧（源视频只有 303 帧）
ffmpeg -i clip.mp4 -vsync 0 -f rawvideo -pix_fmt rgb24 -   → 303 帧 ✅
```

ffmpeg 把标称 60fps、实际 26.7fps 的录屏**上采样成了 681 帧**（复制帧去填时间轴）。
于是管道里第 N 帧不再是源视频第 N 帧，**帧号、时间点与画面完全对不上**——
第 20 帧本应是「迪卢克」，实际读到的却是「派蒙」，整片台词错位约 2.25 倍。

修法：保留这个选项，但**按 ffmpeg 版本二选一**（启动时探测一次）：

| ffmpeg 版本 | 选项 |
|---|---|
| ≥ 5.1 | `-fps_mode passthrough` |
| < 5.1 | `-vsync 0` |

探测方式是用 `ffmpeg <选项> -version` 看退出码，两个都不支持时退化为不加。

**教训**：验证"帧号对不对"必须比对**帧内容**，只对帧数是不够的——程序内部的截断逻辑
会掩盖 ffmpeg 的真实输出。

---

## 3. 已知限制

1. **没有方向分类模型**：90°/180° 倒置的文字读不出来（只做了反色重试）。
2. **没有超分预处理**：极小字号（<10px）的识别率会明显下降；需要的话可以补一个超分模型做预处理。
3. **没有段落级版面分析**：只做了"行分组"，段落缩进和层级结构不保留。
4. **单语言字典**：一个模型配一套字典，换语言要同时换模型和字典
   （模型来源见 [第三方组件来源.md](第三方组件来源.md)）。
5. **公式识别 / 表格识别 / 翻译**：不在本地链路内，本项目不做。

---

## 4. 数据与许可

* 代码：本目录下的示例代码可自由使用。
* 模型：`models/` 下的 ONNX 模型来自 **PaddleOCR 官方模型**（经 RapidAI/RapidOCR 转换为 ONNX），
  遵循 **Apache-2.0** 许可；下载脚本中的来源为 ModelScope `RapidAI/RapidOCR`。
* 样例图：`samples/ch_en_num.jpg` 为 RapidOCR 官方测试图；`samples/sample_ui.png` 由脚本生成，无版权问题。
* 本项目未使用任何闭源商业软件的代码或模型文件；模型全部取自公开仓库，
  逐个文件的来源、版本与校验值见 **[第三方组件来源.md](第三方组件来源.md)**。

---

> 本文档只讲**这个项目怎么用**。
> 调研结论、证据链、完整技术链路、参数含义与更早的实现记录，见
> **[PixPin技术路线.md](PixPin技术路线.md)**（归档快照，不再更新）。
