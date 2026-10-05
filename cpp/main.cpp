// =============================================================================
//  ocr : 「文字识别 + 文字位置」命令行工具 (图片 / 视频 二合一)
//
//  引擎实现在 ocr_engine.h, 视频解码在 video_io.h。
//
//  用法:
//    ocr --image PATH [选项]     识别单张图片
//    ocr --video PATH [选项]     识别视频(逐帧或抽帧)
//
//  输入(二选一, 必填):
//    --image PATH          图片文件
//    --video PATH          视频文件
//
//  处理:
//    --roi x,y,w,h         感兴趣区域。四个值全 <=1 时视为相对比例(0~1),
//                          否则视为像素。默认整幅画面
//    --every-frames N      [视频] 每隔 N 帧取 1 帧 (默认 1, 即逐帧)
//    --every-seconds S     [视频] 每隔 S 秒取 1 帧 (按真实时间戳选取)
//    --max-frames N        [视频] 最多处理 N 个抽样帧 (调试用)
//
//  引擎:
//    --v5                  使用 PP-OCRv5 模型 (默认 PP-OCRv4)
//    --unclip RATIO        文本框外扩系数 (默认 2.0)
//    --threads N           推理线程数 (默认 4)
//
//  输出:
//    --min-score X         过滤掉置信度低于 X 的行 (默认 0, 不过滤)
//    --no-dedup            [视频] 关闭跨帧去重, 每帧全量输出
//    --json PATH           结果写入 JSON (逐行文字 + 置信度 + 四点坐标)
//    --visual PATH         图片模式: 输出画了识别框的 PNG 文件
//                          视频模式: 输出目录, 逐帧存画了识别框的 PNG
// =============================================================================
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "ocr_engine.h"
#include "video_io.h"
#include "dedup.h"

#include <vector>

#ifdef _WIN32
#include <direct.h>
#define MKDIR_ONE(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR_ONE(p) mkdir(p, 0755)
#endif

// ----------------------------- 选项 -----------------------------

struct Options {
    std::string image, video;                  // 输入, 二选一

    std::string roi_spec;                      // 原样保留, 便于回显
    double roi[4] = {0, 0, 0, 0};
    bool has_roi = false;
    bool roi_relative = false;                 // 由数值自动判定

    long long every_frames = 1;                // [视频]
    double every_seconds = 0.0;                // [视频]
    long long max_frames = 0;                  // [视频]
    bool set_every_frames = false, set_every_seconds = false, set_max_frames = false;

    bool v5 = false;                           // 引擎
    float unclip = 2.0f;
    int threads = 4;

    float min_score = 0.f;                     // 输出
    int min_chars = 2;                         // 输出: 丢掉去标点后不足这么多字的行
    std::string json_path, visual_path, save_jpg_path;
    bool dedup = false;                        // [视频] 跨帧去重(默认关, 加 --dedup 打开)
    bool only_text = false;                    // 输出: 只打印文本, 不带序号/坐标/置信度
};

static void print_usage(const char *prog) {
    std::printf(
        "用法:\n"
        "  %s --image PATH [选项]      识别单张图片\n"
        "  %s --video PATH [选项]      识别视频(逐帧或抽帧)\n"
        "\n"
        "输入(二选一, 必填):\n"
        "  --image PATH          图片文件\n"
        "  --video PATH          视频文件\n"
        "\n"
        "处理:\n"
        "  --roi x,y,w,h         感兴趣区域。四个值全 <=1 时视为相对比例(0~1),\n"
        "                        否则视为像素(默认整幅画面)\n"
        "  --every-frames N      [视频] 每隔 N 帧取 1 帧 (默认 1, 即逐帧)\n"
        "  --every-seconds S     [视频] 每隔 S 秒取 1 帧 (按真实时间戳选取)\n"
        "  --max-frames N        [视频] 最多处理 N 个抽样帧 (调试用)\n"
        "\n"
        "引擎:\n"
        "  --v5                  使用 PP-OCRv5 模型 (默认 PP-OCRv4)\n"
        "  --unclip RATIO        文本框外扩系数 (默认 2.0)\n"
        "  --threads N           推理线程数 (默认 4)\n"
        "\n"
        "输出:\n"
        "  --min-score X         过滤掉置信度低于 X 的行 (默认 0, 不过滤)\n"
        "  --min-chars N         丢掉\"去掉标点后不足 N 个字\"的行 (默认 2):\n"
        "                        可滤掉句首被切碎的短词、图标误识别; 设 1 表示不过滤\n"
        "  --only-text           只输出文本内容, 不打印序号/置信度/四点坐标:\n"
        "                        图片模式 -> 逐行打印文字\n"
        "                        视频模式 -> 帧号 + 时间 + 文字\n"
        "  --dedup               [视频] 开启跨帧去重(默认关闭):\n"
        "                        同一段文字只输出一次, 且不输出字幕\"还没打印完\"的中间版本\n"
        "  --json PATH           结果写入 JSON (逐行文字 + 置信度 + 四点坐标)\n"
        "  --visual PATH         图片模式: 输出画了识别框的 PNG 文件\n"
        "                        视频模式: 输出目录, 逐帧存画了识别框的 PNG\n"
        "                        (可视化不受去重影响, 每个采样帧都会存)\n"
        "  --save-jpg PATH       [视频] 给\"去重后仍然有字幕\"的画面存 JPG (需配合 --dedup):\n"
        "                        从原视频截取整帧(不是 ROI), 不画框;\n"
        "                        文件名为 分-秒-毫秒.jpg, 如 00-09-500.jpg\n"
        "\n"
        "  上面所有指定 PATH 的目录参数, 目录不存在时会自动创建。\n"
        "\n"
        "示例:\n"
        "  %s --image shot.png\n"
        "  %s --image shot.png --roi 0,0,0.5,1 --json out.json\n"
        "  %s --video clip.mp4 --every-seconds 0.5\n"
        "  %s --video clip.mp4 --roi 300,780,960,200 --visual out/\n",
        prog, prog, prog, prog, prog, prog);
}

// ----------------------------- ROI -----------------------------

// 四个值全部 <=1 (且非负) → 相对比例; 只要有一个 >1 → 像素
static bool roi_looks_relative(const double v[4]) {
    for (int i = 0; i < 4; ++i) if (v[i] > 1.0) return false;
    return true;
}

static bool parse_roi(const std::string &s, double out[4]) {
    return std::sscanf(s.c_str(), "%lf,%lf,%lf,%lf",
                       &out[0], &out[1], &out[2], &out[3]) == 4;
}

// 把 ROI 统一换算成像素坐标, 并夹紧到画面内
static void resolve_roi(const Options &o, int W, int H,
                        int &x, int &y, int &w, int &h) {
    if (!o.has_roi) { x = 0; y = 0; w = W; h = H; return; }
    if (o.roi_relative) {
        x = (int)std::lround(o.roi[0] * W);
        y = (int)std::lround(o.roi[1] * H);
        w = (int)std::lround(o.roi[2] * W);
        h = (int)std::lround(o.roi[3] * H);
    } else {
        x = (int)std::lround(o.roi[0]);
        y = (int)std::lround(o.roi[1]);
        w = (int)std::lround(o.roi[2]);
        h = (int)std::lround(o.roi[3]);
    }
    x = std::min(std::max(x, 0), W - 1);
    y = std::min(std::max(y, 0), H - 1);
    w = std::min(std::max(w, 1), W - x);
    h = std::min(std::max(h, 1), H - y);
}

// 从大图里裁出 ROI 子图 (逐行 memcpy, 比逐像素快得多)
static Image sub_image(const Image &src, int x, int y, int w, int h) {
    Image out;
    out.w = w;
    out.h = h;
    out.px.resize((size_t)w * h * 3);
    for (int r = 0; r < h; ++r) {
        const unsigned char *s = &src.px[((size_t)(y + r) * src.w + x) * 3];
        std::memcpy(&out.px[(size_t)r * w * 3], s, (size_t)w * 3);
    }
    return out;
}

// 去掉标点空白后还够长吗? 用来滤掉句首被切碎的短词、图标误识别之类。
// 实测一段 3 分 55 秒的游戏录屏: 1 个字的行只有 8 行, 全是噪声;
// 2 个字的行有 250 行, 其中 231 行是说话人名字「派蒙」—— 所以门槛放在 2 字。
static bool text_too_short(const std::string &text, int min_chars) {
    if (min_chars <= 1) return false;
    return (int)dedup::utf8_chars(dedup::normalize(text)).size() < min_chars;
}

// 取路径的父目录 (没有分隔符时返回空串)
static std::string parent_dir(const std::string &p) {
    const size_t pos = p.find_last_of("/\\");
    if (pos == std::string::npos) return std::string();
    if (pos == 0) return std::string("/");
    return p.substr(0, pos);
}

// 递归创建目录 (等价 mkdir -p)。已存在的层级会返回非 0, 忽略即可。
static void make_dirs(const std::string &path) {
    if (path.empty()) return;
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur += path[i];
        if ((path[i] == '/' || path[i] == '\\') && cur.size() > 1) MKDIR_ONE(cur.c_str());
    }
    MKDIR_ONE(path.c_str());
}

// 写文件前确保它的父目录存在
static void ensure_parent(const std::string &path) {
    const std::string d = parent_dir(path);
    if (!d.empty()) make_dirs(d);
}

// 把秒数格式化成 "分-秒-毫秒", 如 9.5s -> "00-09-500"
static std::string jpg_name_of(double seconds) {
    long long ms = (long long)std::llround(seconds * 1000.0);
    if (ms < 0) ms = 0;
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%02lld-%02lld-%03lld.jpg",
                  ms / 60000, (ms / 1000) % 60, ms % 1000);
    return std::string(buf);
}

// ----------------------------- 图片模式 -----------------------------

static int run_image(const Options &o, const std::string &model_dir) {
    Image img = load_image(o.image);
    if (img.w == 0) return 1;

    int rx, ry, rw, rh;
    resolve_roi(o, img.w, img.h, rx, ry, rw, rh);
    bool whole = (rx == 0 && ry == 0 && rw == img.w && rh == img.h);

    Image work = whole ? img : sub_image(img, rx, ry, rw, rh);

    OcrEngine engine(model_dir, o.v5, o.threads, o.unclip);
    auto t0 = std::chrono::steady_clock::now();
    std::vector<TextBox> items = engine.run(work);
    double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();

    // 坐标还原到整幅图坐标系 + 置信度过滤
    std::vector<TextBox> kept;
    kept.reserve(items.size());
    for (auto &it : items) {
        if (it.score < o.min_score) continue;
        if (text_too_short(it.text, o.min_chars)) continue;
        if (!whole) {
            for (int k = 0; k < 4; ++k) {
                it.quad.p[k].x += (float)rx;
                it.quad.p[k].y += (float)ry;
            }
        }
        kept.push_back(it);
    }

    std::printf("# %s  %dx%d", o.image.c_str(), img.w, img.h);
    if (!whole) std::printf("  感兴趣区域 (%d,%d) %dx%d", rx, ry, rw, rh);
    std::printf("  检测到 %d 行文字  耗时 %.0f ms (%s)\n",
                (int)kept.size(), ms, o.v5 ? "PP-OCRv5" : "PP-OCRv4");
    for (size_t i = 0; i < kept.size(); ++i) {
        if (o.only_text) {                      // 只打印文字本身
            std::printf("%s\n", kept[i].text.c_str());
            continue;
        }
        const auto &q = kept[i].quad;
        std::printf("[%02d] x=%4d y=%4d w=%4d h=%3d  score=%.2f  %s\n", (int)i + 1,
                    (int)q.xmin(), (int)q.ymin(), (int)(q.xmax() - q.xmin()),
                    (int)(q.ymax() - q.ymin()), kept[i].score, kept[i].text.c_str());
    }

    if (!o.visual_path.empty()) {
        Image vis = img;
        for (size_t i = 0; i < kept.size(); ++i) {
            const auto &q = kept[i].quad;
            for (int k = 0; k < 4; ++k)
                draw_line(vis, q.p[k].x, q.p[k].y, q.p[(k + 1) % 4].x, q.p[(k + 1) % 4].y,
                          255, 0, 0, 2);
            draw_number(vis, (int)i + 1, (int)q.xmin(), std::max((int)q.ymin() - 16, 0),
                        255, 96, 0);
        }
        ensure_parent(o.visual_path);
        if (save_image(o.visual_path, vis))
            std::printf("可视化    : %s\n", o.visual_path.c_str());
        else
            std::fprintf(stderr, "警告: 可视化图片写入失败: %s\n", o.visual_path.c_str());
    }

    if (!o.json_path.empty()) {
        ensure_parent(o.json_path);
        std::ofstream f(o.json_path, std::ios::binary);
        if (!f) {
            std::fprintf(stderr, "警告: 无法写入 JSON: %s\n", o.json_path.c_str());
        } else {
            f << "{\n  \"image\": \"" << json_escape(o.image) << "\",\n";
            f << "  \"size\": [" << img.w << ", " << img.h << "],\n";
            if (!whole) {
                f << "  \"roi\": [" << rx << ", " << ry << ", " << rw << ", " << rh << "],\n";
            }
            f << "  \"items\": [\n";
            for (size_t i = 0; i < kept.size(); ++i) {
                const auto &it = kept[i];
                f << "    {\"text\": \"" << json_escape(it.text) << "\", \"score\": " << it.score
                  << ", \"box\": [";
                for (int k = 0; k < 4; ++k)
                    f << "[" << (int)std::lround(it.quad.p[k].x) << ", "
                      << (int)std::lround(it.quad.p[k].y) << "]" << (k < 3 ? ", " : "");
                f << "]}" << (i + 1 < kept.size() ? "," : "") << "\n";
            }
            f << "  ]\n}\n";
            std::printf("JSON      : %s\n", o.json_path.c_str());
        }
    }
    return 0;
}

// ----------------------------- 视频模式 -----------------------------

static int run_video(const Options &o, const std::string &model_dir, const char *argv0) {
    // ffmpeg / ffprobe: 先看本程序同目录, 再看 PATH。都没有就直接报错。
    std::string ffmpeg  = find_program("ffmpeg",  argv0);
    std::string ffprobe = find_program("ffprobe", argv0);
    if (ffmpeg.empty() || ffprobe.empty()) {
        const char *missing = ffmpeg.empty() ? "ffmpeg" : "ffprobe";
        std::fprintf(stderr,
            "找不到 %s。\n"
            "  识别视频需要 ffmpeg 和 ffprobe。请把它们放到本程序同一目录下,\n"
            "  或加入系统 PATH 后重试。\n", missing);
        return 2;
    }

    // ---------- 1. 探测视频 ----------
    VideoInfo vi;
    if (!probe_video(ffprobe, o.video, vi)) {
        std::fprintf(stderr,
            "无法读取视频信息: %s\n"
            "  请确认文件存在且是 ffmpeg 能解析的视频。\n", o.video.c_str());
        return 2;
    }

    // ---------- 2. 换算感兴趣区域 ----------
    int rx, ry, rw, rh;
    resolve_roi(o, vi.w, vi.h, rx, ry, rw, rh);
    rw &= ~1;
    rh &= ~1;                                   // 偶数宽高对编码器/滤镜更友好
    bool whole = (rx == 0 && ry == 0 && rw == vi.w && rh == vi.h);

    // ---------- 3. 取得时间基准, 决定抽哪些帧 ----------
    // 恒定帧率(CFR)视频每帧间隔相同, 时间点可以直接由「帧号 ÷ 帧率」算出,
    // 于是**不必扫全片 PTS**; 变帧率(VFR)视频间隔不均匀, 必须取逐帧真实时间戳。
    // 三条路径都会打印出来, 便于确认这次到底有没有完整扫描一遍 PTS。
    enum class TimeBase { CfrDerived, PtsScanned, PtsEstimated };
    TimeBase tbase = TimeBase::PtsScanned;
    std::vector<double> pts;
    if (vi.cfr && vi.fps > 0 && vi.reported_frames > 0) {
        tbase = TimeBase::CfrDerived;
        pts.reserve((size_t)vi.reported_frames);
        for (long long i = 0; i < vi.reported_frames; ++i)
            pts.push_back((double)i / vi.fps);
    } else {
        pts = probe_frame_times(ffprobe, o.video);
        if (pts.empty() && vi.fps > 0 && vi.reported_frames > 0) {
            tbase = TimeBase::PtsEstimated;      // 容器读不出逐帧 PTS 时的兜底
            for (long long i = 0; i < vi.reported_frames; ++i)
                pts.push_back((double)i / vi.fps);
        }
    }
    bool pts_valid = !pts.empty();
    std::vector<char> keep(pts.size(), 0);      // keep[i] = 第 i 帧是否要识别
    bool by_seconds = o.every_seconds > 0.0;
    long long step = o.every_frames;
    if (by_seconds) {
        // 按时间网格选帧: 贪心取「距上一个入选帧至少间隔 S 秒」的下一帧,
        // 这样即使中间帧率抖动, 抽样间隔也稳定在 S 秒附近。
        double next = 0.0;
        for (size_t i = 0; i < pts.size(); ++i) {
            if (pts[i] + 1e-6 >= next) { keep[i] = 1; next = pts[i] + o.every_seconds; }
        }
    } else {
        for (size_t i = 0; i < pts.size(); i += (size_t)step) keep[i] = 1;
    }
    long long planned = 0;
    for (char c : keep) planned += c;

    // ---------- 4. 初始化引擎 ----------
    OcrEngine engine(model_dir, o.v5, o.threads, o.unclip);

    // 跨帧去重: 时间阈值按实际采样间隔换算, 这样逐帧和抽帧用同一套判定
    double sample_interval = by_seconds ? o.every_seconds
                           : (vi.fps > 0 ? (double)step / vi.fps : 0.5);
    if (sample_interval <= 0.0) sample_interval = 0.5;
    dedup::LineDedup dedup_engine(sample_interval);

    // ---------- 5. 组装 ffmpeg 命令 ----------
    // 只让 ffmpeg 做 crop(在源头上把 ROI 裁掉, 管道里只传 ROI 像素)。
    // 抽帧不交给 ffmpeg 的 select 滤镜, 而是把每一帧都读进来、在本地按真实 PTS
    // 决定要不要识别 —— 因为 select 只能按「每 N 帧」这类规则选, 表达不了
    // 「第 1.37 秒那一帧」这种时间网格; 丢弃帧的 fread 开销实测可忽略。
    std::string vf;
    if (!whole) {
        vf = "crop=" + std::to_string(rw) + ":" + std::to_string(rh) + ":" +
             std::to_string(rx) + ":" + std::to_string(ry);
    }
    bool need_wrap = false;
    std::string args = "-v error -nostdin -an -sn -i " + quote_arg(o.video);
    if (!vf.empty()) args += " -vf " + quote_arg(vf);
    // 必须让 ffmpeg 原样吐帧, 否则 VFR 视频会被上采样, 帧号与画面错位(见 video_io.h)
    std::string passthrough = detect_passthrough_opt(ffmpeg);
    if (!passthrough.empty()) args += " " + passthrough;
    args += " -f rawvideo -pix_fmt rgb24 -";
    // --max-frames 会提前关闭管道, ffmpeg 会因 EPIPE 往 stderr 刷一屏报错,
    // 这种情况把它的 stderr 丢掉, 免得看起来像失败。
    std::string cmd = build_cmd(ffmpeg, args, need_wrap, o.max_frames > 0);

    // ---------- 6. 打印头部信息 ----------
    std::printf("视频      : %s\n", o.video.c_str());
    std::printf("画面      : %dx%d", vi.w, vi.h);
    if (vi.fps > 0) std::printf("  平均 %.3f fps", vi.fps);
    if (vi.duration > 0) std::printf("  时长 %s", fmt_time(vi.duration).c_str());
    if (vi.reported_frames > 0) std::printf("  %lld 帧", vi.reported_frames);
    if (pts_valid) std::printf("  最后一帧 PTS %.3fs", pts.back());
    std::printf("\n");
    // 时间基准: 明确告知本次是否完整扫描了全片 PTS
    switch (tbase) {
    case TimeBase::CfrDerived:
        std::printf("时间基准  : 恒定帧率 %.3f fps -> 跳过 PTS 扫描, 时间点由帧号推算\n", vi.fps);
        break;
    case TimeBase::PtsScanned:
        std::printf("时间基准  : 变帧率 (标称 %.3f fps / 平均 %.3f fps)"
                    " -> 已完整扫描全片 PTS, %zu 帧\n", vi.r_fps, vi.avg_fps, pts.size());
        break;
    case TimeBase::PtsEstimated:
        std::printf("时间基准  : 读不到逐帧 PTS -> 时间点按平均帧率 %.3f fps 估算\n", vi.fps);
        break;
    }
    if (o.has_roi) {
        std::printf("感兴趣区域: (%d,%d) %dx%d  [%s]\n", rx, ry, rw, rh,
                    o.roi_relative ? "相对比例" : "像素");
    } else {
        std::printf("感兴趣区域: 整帧\n");
    }
    if (by_seconds)
        std::printf("抽帧间隔  : 每 %.3f 秒 1 帧 (%s, 共 %lld 帧)\n", o.every_seconds,
                    tbase == TimeBase::CfrDerived ? "按帧号推算时间选取" : "按真实时间戳选取",
                    planned);
    else if (step > 1)
        std::printf("抽帧间隔  : 每 %lld 帧取 1 帧 (共 %lld 帧)\n", step, planned);
    else
        std::printf("抽帧间隔  : 逐帧 (共 %lld 帧)\n", planned);
    std::printf("模型      : %s\n", o.v5 ? "PP-OCRv5" : "PP-OCRv4");
    if (!whole)
        std::printf("坐标说明  : 输出的四点坐标已加回 ROI 偏移, 即相对原帧左上角\n");
    if (o.dedup)
        std::printf("跨帧去重  : 开启 (同一段文字只输出一次, 跳过未打印完整的中间版本)\n");
    else
        std::printf("跨帧去重  : 关闭 (默认; 想合并重复字幕请加 --dedup)\n");
    std::printf("---------------------------------------------------------------\n");
    std::fflush(stdout);

    // ---------- 7. 打开管道 ----------
    FILE *pipe = POPEN_CMD(cmd.c_str(), PIPE_READ_BIN);
    if (!pipe) {
        std::fprintf(stderr, "无法启动 ffmpeg。请确认 ffmpeg 可用 (本目录或 PATH)。\n");
        return 2;
    }
#ifdef _WIN32
    // Windows 的 _popen 默认文本模式, 会把 \n 翻译掉并破坏二进制帧数据
    _setmode(_fileno(pipe), _O_BINARY);
#endif

    FILE *json = nullptr;
    if (!o.json_path.empty()) {
        ensure_parent(o.json_path);
        json = std::fopen(o.json_path.c_str(), "wb");
        if (json) {
            std::fprintf(json, "{\n  \"video\": \"%s\",\n", json_escape(o.video).c_str());
            std::fprintf(json, "  \"size\": [%d, %d],\n  \"fps\": %.6f,\n", vi.w, vi.h, vi.fps);
            std::fprintf(json, "  \"roi\": [%d, %d, %d, %d],\n", rx, ry, rw, rh);
            if (by_seconds)
                std::fprintf(json, "  \"every_seconds\": %.6f,\n", o.every_seconds);
            else
                std::fprintf(json, "  \"every_frames\": %lld,\n", step);
            std::fprintf(json, "  \"sampled\": %lld,\n  \"frames\": [\n", planned);
        } else {
            std::fprintf(stderr, "警告: 无法写入 JSON: %s\n", o.json_path.c_str());
        }
    }

    // ---------- 8. 逐帧读取 + 识别 ----------
    Image frame;
    frame.w = rw;
    frame.h = rh;
    frame.px.resize((size_t)rw * rh * 3);

    long long frame_no = 0, sampled = 0, frames_with_text = 0, total_lines = 0;
    auto t0 = std::chrono::steady_clock::now();
    bool first_json = true;
    std::vector<std::pair<long long, double>> shots;   // --save-jpg: 待截图的 (帧号, 时间)

    // 输出一组行(控制台 + JSON)。去重后由 LineDedup 决定"哪些行、归到哪一帧"输出。
    auto emit_group = [&](const dedup::Group &g) {
        ++frames_with_text;
        total_lines += (long long)g.items.size();

        std::printf("[帧 %06lld] t=%s  %zu 行\n", g.frame, fmt_time(g.time).c_str(),
                    g.items.size());
        for (size_t j = 0; j < g.items.size(); ++j) {
            const dedup::Item &it = g.items[j];
            if (o.only_text) {                  // 只打印文字本身
                std::printf("    %s\n", it.text.c_str());
                continue;
            }
            std::printf("    #%zu  %.3f  (%.0f,%.0f) (%.0f,%.0f) (%.0f,%.0f) (%.0f,%.0f)  %s\n",
                        j + 1, it.score,
                        it.x[0], it.y[0], it.x[1], it.y[1],
                        it.x[2], it.y[2], it.x[3], it.y[3],
                        it.text.c_str());
        }
        std::fflush(stdout);

        if (json) {
            if (!first_json) std::fprintf(json, ",\n");
            first_json = false;
            std::fprintf(json, "    {\"frame\": %lld, \"time\": %.6f, \"lines\": [", g.frame, g.time);
            for (size_t j = 0; j < g.items.size(); ++j) {
                const dedup::Item &it = g.items[j];
                std::fprintf(json, "%s{\"text\": \"%s\", \"score\": %.6f, \"box\": [",
                             j ? ", " : "", json_escape(it.text).c_str(), it.score);
                for (int k = 0; k < 4; ++k)
                    std::fprintf(json, "[%d, %d]%s", (int)std::lround(it.x[k]),
                                 (int)std::lround(it.y[k]), k < 3 ? ", " : "");
                std::fprintf(json, "]}");
            }
            std::fprintf(json, "]}");
        }

        if (!o.save_jpg_path.empty()) shots.emplace_back(g.frame, g.time);
    };

    // 同一帧的多个位置可能在不同轮次才结算(例如台词先被下一句打断、说话人名字稍后),
    // 先攒一下再打印, 免得同一个帧号出现两次帧头。
    std::vector<dedup::Group> pend;
    auto flush_pend = [&]() {
        for (const auto &g : pend) emit_group(g);
        pend.clear();
    };
    auto push_group = [&](dedup::Group g) {
        if (!pend.empty() && pend.front().frame != g.frame) flush_pend();
        bool merged = false;
        for (auto &x : pend) {
            if (x.frame == g.frame) {
                x.items.insert(x.items.end(), g.items.begin(), g.items.end());
                merged = true;
                break;
            }
        }
        if (!merged) pend.push_back(std::move(g));
    };

    while (true) {
        size_t got = std::fread(frame.px.data(), 1, frame.px.size(), pipe);
        if (got < frame.px.size()) break;              // 流结束 / 不完整帧

        bool select_all = keep.empty();                // PTS 完全拿不到时才走这里
        bool want = select_all ? true : ((size_t)frame_no < keep.size() && keep[frame_no]);
        if (!select_all && (size_t)frame_no >= keep.size()) break;   // PTS 表之外的帧
        if (!want) { ++frame_no; continue; }

        // 时间点一律用真实 PTS (VFR 视频里「帧号 / 平均帧率」会偏掉)
        double ts = (size_t)frame_no < pts.size() ? pts[frame_no]
                    : (vi.fps > 0 ? (double)frame_no / vi.fps : 0.0);
        std::vector<TextBox> items = engine.run(frame);

        // 置信度过滤 + ROI 坐标还原到整帧坐标系
        std::vector<TextBox> kept;
        for (auto &it : items) {
            if (it.score < o.min_score) continue;
            if (text_too_short(it.text, o.min_chars)) continue;
            if (!whole) {
                for (int k = 0; k < 4; ++k) {
                    it.quad.p[k].x += (float)rx;
                    it.quad.p[k].y += (float)ry;
                }
            }
            kept.push_back(it);
        }

        ++sampled;

        // 跨帧去重: 同一段文字只输出一次, 且跳过"字幕还没打印完整"的中间版本。
        // 输出了哪些行、算在哪一帧, 全部由 LineDedup 决定(可能比当前帧滞后一两帧)。
        if (!kept.empty()) {
            std::vector<dedup::Item> ditems;
            ditems.reserve(kept.size());
            for (const auto &it : kept) {
                dedup::Item d;
                d.text  = it.text;
                d.score = it.score;
                for (int k = 0; k < 4; ++k) { d.x[k] = it.quad.p[k].x; d.y[k] = it.quad.p[k].y; }
                ditems.push_back(std::move(d));
            }

            if (o.dedup) {
                for (auto &g : dedup_engine.feed(frame_no, ts, ditems)) push_group(std::move(g));
            } else {
                dedup::Group g;
                g.frame = frame_no;
                g.time  = ts;
                g.items = std::move(ditems);
                emit_group(g);
            }
        }

        // 可视化按原始采样帧保存, 不受去重影响(便于逐帧核对识别框)
        if (!o.visual_path.empty() && !kept.empty()) {
            Image vis = frame;
            for (size_t j = 0; j < kept.size(); ++j) {
                const Quad &q = kept[j].quad;
                for (int k = 0; k < 4; ++k) {
                    // 画回 ROI 局部坐标(帧图本身就只有 ROI 那块)
                    draw_line(vis, q.p[k].x - rx, q.p[k].y - ry,
                              q.p[(k + 1) % 4].x - rx, q.p[(k + 1) % 4].y - ry, 255, 0, 0, 2);
                }
            }
            char fn[1024];
            std::snprintf(fn, sizeof(fn), "%s/frame_%06lld.png", o.visual_path.c_str(), frame_no);
            if (!save_image(fn, vis))
                std::fprintf(stderr, "警告: 可视化帧写入失败: %s\n", fn);
        }

        ++frame_no;
        if (o.max_frames > 0 && sampled >= o.max_frames) break;
    }

    // 视频结束: 把还在等待确认的段吐出来
    if (o.dedup) {
        for (auto &g : dedup_engine.flush()) push_group(std::move(g));
        flush_pend();
    }

    int rc = PCLOSE_CMD(pipe);
    if (sampled == 0) {
        std::fprintf(stderr,
            "\n没有读到任何帧。可能原因:\n"
            "  1) 视频无法解码\n"
            "  2) 抽帧间隔大于视频总帧数\n");
        if (rc != 0) std::fprintf(stderr, "  ffmpeg 退出码: %d\n", rc);
    }

    double elapsed = std::chrono::duration<double>(
                         std::chrono::steady_clock::now() - t0).count();

    if (json) {
        std::fprintf(json, "\n  ]\n}\n");
        std::fclose(json);
    }

    // --save-jpg: 把"去重后仍然有字幕"的那一帧从**原视频**整帧截出来。
    // 必须重新调一次 ffmpeg —— 管道里只传了 ROI 那块像素, 拿不到整帧。
    // 截图耗时不计入上面的识别耗时, 所以放在 elapsed 算完之后。
    int saved = 0;
    if (!o.save_jpg_path.empty()) {
        make_dirs(o.save_jpg_path);
        for (const auto &s : shots) {
            const std::string out = o.save_jpg_path + "/" + jpg_name_of(s.second);
            char ss[64];
            std::snprintf(ss, sizeof(ss), "%.3f", s.second);
            const std::string args = "-v error -nostdin -an -sn -ss " + std::string(ss)
                                   + " -i " + quote_arg(o.video)
                                   + " -frames:v 1 -q:v 2 -y " + quote_arg(out);
            bool wrap = false;
            run_capture(build_cmd(ffmpeg, args, wrap, true));
            if (file_exists(out)) ++saved;
            else std::fprintf(stderr, "警告: 截图失败: %s\n", out.c_str());
        }
    }

    std::printf("---------------------------------------------------------------\n");
    std::printf("抽样帧数  : %lld\n", sampled);
    std::printf("输出帧数  : %lld\n", frames_with_text);
    std::printf("文字总行数: %lld\n", total_lines);
    if (o.dedup)
        std::printf("去重抑制  : %lld 行 (重复出现, 以及字幕未打印完整的中间版本)\n",
                    dedup_engine.suppressed());
    std::printf("耗时      : %.1f s", elapsed);
    if (sampled > 0) std::printf("  (平均 %.1f ms/帧)", elapsed * 1000.0 / (double)sampled);
    std::printf("\n");
    if (!o.json_path.empty())   std::printf("JSON      : %s\n", o.json_path.c_str());
    if (!o.visual_path.empty()) std::printf("可视化目录: %s\n", o.visual_path.c_str());
    if (!o.save_jpg_path.empty())
        std::printf("截图目录  : %s  (%d/%zu 张)\n", o.save_jpg_path.c_str(),
                    saved, shots.size());
    return 0;
}

// ----------------------------- main -----------------------------

int main(int argc, char **argv) {
    if (argc < 2) { print_usage(argv[0]); return 1; }

    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return std::string(i + 1 < argc ? argv[++i] : ""); };
        if (a == "--image")              o.image = next();
        else if (a == "--video")         o.video = next();
        else if (a == "--roi") {
            o.roi_spec = next();
            o.has_roi = parse_roi(o.roi_spec, o.roi);
            if (!o.has_roi) {
                std::fprintf(stderr, "参数 --roi 格式应为 x,y,w,h, 例如 100,200,300,150\n");
                return 1;
            }
            o.roi_relative = roi_looks_relative(o.roi);
            for (int k = 0; k < 4; ++k) {
                if (o.roi[k] < 0) {
                    std::fprintf(stderr, "参数 --roi 的四个值不能为负。\n");
                    return 1;
                }
            }
            if (o.roi[2] <= 0 || o.roi[3] <= 0) {
                std::fprintf(stderr, "参数 --roi 的宽高必须大于 0。\n");
                return 1;
            }
        }
        else if (a == "--every-frames")  { o.every_frames  = std::atoll(next().c_str()); o.set_every_frames  = true; }
        else if (a == "--every-seconds") { o.every_seconds = std::atof(next().c_str());  o.set_every_seconds = true; }
        else if (a == "--max-frames")    { o.max_frames    = std::atoll(next().c_str()); o.set_max_frames    = true; }
        else if (a == "--v5")            o.v5 = true;
        else if (a == "--unclip")        o.unclip = (float)std::atof(next().c_str());
        else if (a == "--threads")       o.threads = std::atoi(next().c_str());
        else if (a == "--min-score")     o.min_score = (float)std::atof(next().c_str());
        else if (a == "--min-chars")     o.min_chars = std::atoi(next().c_str());
        else if (a == "--only-text")     o.only_text = true;
        else if (a == "--dedup")         o.dedup = true;
        else if (a == "--json")          o.json_path = next();
        else if (a == "--visual")        o.visual_path = next();
        else if (a == "--save-jpg")      o.save_jpg_path = next();
        else if (a == "-h" || a == "--help") { print_usage(argv[0]); return 0; }
        else {
            std::fprintf(stderr, "未知参数: %s\n\n", a.c_str());
            print_usage(argv[0]);
            return 1;
        }
    }

    // ---------- 输入校验 ----------
    if (o.image.empty() && o.video.empty()) {
        std::fprintf(stderr, "必须指定 --image 或 --video 之一。\n\n");
        print_usage(argv[0]);
        return 1;
    }
    if (!o.image.empty() && !o.video.empty()) {
        std::fprintf(stderr, "--image 与 --video 只能二选一。\n");
        return 1;
    }
    bool is_video = !o.video.empty();
    if (!is_video) {
        // 视频独有的参数用在图片模式上, 直接报错, 免得静默忽略让人困惑
        const char *bad = o.set_every_frames ? "--every-frames"
                        : o.set_every_seconds ? "--every-seconds"
                        : o.set_max_frames ? "--max-frames" : nullptr;
        if (bad) {
            std::fprintf(stderr, "%s 只对 --video 模式有效。\n", bad);
            return 1;
        }
    }
    if (o.every_frames < 1) o.every_frames = 1;
    if (o.threads < 1)      o.threads = 1;

    // ---------- 定位模型目录 (不再支持手工指定, 找不到直接报错) ----------
    std::string model_dir = find_model_dir(argv[0], o.v5);
    if (model_dir.empty()) {
        std::fprintf(stderr,
            "找不到模型目录。\n"
            "  程序会在自身目录及其上级目录中查找 models/, 其中需要包含:\n"
            "    %s   %s   %s\n"
            "  请确认 models/ 与程序放在一起, 或位于其上级目录。\n",
            o.v5 ? "det_v5.onnx" : "det_v4.onnx",
            o.v5 ? "rec_v5.onnx" : "rec_v4.onnx",
            o.v5 ? "keys_v5.txt" : "keys_v4.txt");
        return 1;
    }

    // ---------- 视频模式: 可视化输出是目录, 不存在则建 ----------
    if (is_video && !o.visual_path.empty()) {
        make_dirs(o.visual_path);
    }

    try {
        return is_video ? run_video(o, model_dir, argv[0]) : run_image(o, model_dir);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "运行失败: %s\n", e.what());
        return 3;
    }
}
