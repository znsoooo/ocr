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
//    --json PATH           结果写入 JSON (逐行文字 + 置信度 + 四点坐标)
//    --visual PATH         图片模式: 输出画了识别框的 PNG 文件
//                          视频模式: 输出目录, 逐帧存画了识别框的 PNG
// =============================================================================
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "ocr_engine.h"
#include "video_io.h"

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
    std::string json_path, visual_path;
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
        "  --json PATH           结果写入 JSON (逐行文字 + 置信度 + 四点坐标)\n"
        "  --visual PATH         图片模式: 输出画了识别框的 PNG 文件\n"
        "                        视频模式: 输出目录, 逐帧存画了识别框的 PNG\n"
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
        if (save_image(o.visual_path, vis))
            std::printf("可视化    : %s\n", o.visual_path.c_str());
        else
            std::fprintf(stderr, "警告: 可视化图片写入失败: %s\n", o.visual_path.c_str());
    }

    if (!o.json_path.empty()) {
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

    // ---------- 3. 读取每帧真实时间, 决定抽哪些帧 ----------
    std::vector<double> pts = probe_frame_times(ffprobe, o.video);
    bool pts_valid = !pts.empty();
    if (!pts_valid && vi.fps > 0 && vi.reported_frames > 0) {
        // 极端情况(容器读不出 PTS)下退化为按平均帧率估算
        for (long long i = 0; i < vi.reported_frames; ++i)
            pts.push_back((double)i / vi.fps);
        std::fprintf(stderr, "提示: 未能读到逐帧 PTS, 时间点按平均帧率 %g 估算。\n", vi.fps);
    }
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
    if (o.has_roi) {
        std::printf("感兴趣区域: (%d,%d) %dx%d  [%s]\n", rx, ry, rw, rh,
                    o.roi_relative ? "相对比例" : "像素");
    } else {
        std::printf("感兴趣区域: 整帧\n");
    }
    if (by_seconds)
        std::printf("抽帧间隔  : 每 %.3f 秒 1 帧 (按真实时间戳选取, 共 %lld 帧)\n",
                    o.every_seconds, planned);
    else if (step > 1)
        std::printf("抽帧间隔  : 每 %lld 帧取 1 帧 (共 %lld 帧)\n", step, planned);
    else
        std::printf("抽帧间隔  : 逐帧 (共 %lld 帧)\n", planned);
    std::printf("模型      : %s\n", o.v5 ? "PP-OCRv5" : "PP-OCRv4");
    if (!whole)
        std::printf("坐标说明  : 输出的四点坐标已加回 ROI 偏移, 即相对原帧左上角\n");
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
            if (!whole) {
                for (int k = 0; k < 4; ++k) {
                    it.quad.p[k].x += (float)rx;
                    it.quad.p[k].y += (float)ry;
                }
            }
            kept.push_back(it);
        }

        ++sampled;
        if (!kept.empty()) { ++frames_with_text; total_lines += (long long)kept.size(); }

        // 只输出识别到文字的帧 (空帧仅计入汇总)
        if (!kept.empty()) {
            std::printf("[帧 %06lld] t=%s  %zu 行\n", frame_no, fmt_time(ts).c_str(),
                        kept.size());
            for (size_t j = 0; j < kept.size(); ++j) {
                const Quad &q = kept[j].quad;
                std::printf("    #%zu  %.3f  (%.0f,%.0f) (%.0f,%.0f) (%.0f,%.0f) (%.0f,%.0f)  %s\n",
                            j + 1, kept[j].score,
                            q.p[0].x, q.p[0].y, q.p[1].x, q.p[1].y,
                            q.p[2].x, q.p[2].y, q.p[3].x, q.p[3].y,
                            kept[j].text.c_str());
            }
            std::fflush(stdout);
        }

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

        if (json) {
            if (!first_json) std::fprintf(json, ",\n");
            first_json = false;
            std::fprintf(json, "    {\"frame\": %lld, \"time\": %.6f, \"lines\": [", frame_no, ts);
            for (size_t j = 0; j < kept.size(); ++j) {
                const Quad &q = kept[j].quad;
                std::fprintf(json, "%s{\"text\": \"%s\", \"score\": %.6f, \"box\": [",
                             j ? ", " : "", json_escape(kept[j].text).c_str(), kept[j].score);
                for (int k = 0; k < 4; ++k)
                    std::fprintf(json, "[%d, %d]%s", (int)std::lround(q.p[k].x),
                                 (int)std::lround(q.p[k].y), k < 3 ? ", " : "");
                std::fprintf(json, "]}");
            }
            std::fprintf(json, "]}");
        }

        ++frame_no;
        if (o.max_frames > 0 && sampled >= o.max_frames) break;
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

    std::printf("---------------------------------------------------------------\n");
    std::printf("抽样帧数  : %lld\n", sampled);
    std::printf("含文字帧  : %lld\n", frames_with_text);
    std::printf("文字总行数: %lld\n", total_lines);
    std::printf("耗时      : %.1f s", elapsed);
    if (sampled > 0) std::printf("  (平均 %.1f ms/帧)", elapsed * 1000.0 / (double)sampled);
    std::printf("\n");
    if (!o.json_path.empty())   std::printf("JSON      : %s\n", o.json_path.c_str());
    if (!o.visual_path.empty()) std::printf("可视化目录: %s\n", o.visual_path.c_str());
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
        else if (a == "--json")          o.json_path = next();
        else if (a == "--visual")        o.visual_path = next();
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
        if (!file_exists(o.visual_path)) { MKDIR_ONE(o.visual_path.c_str()); }
    }

    try {
        return is_video ? run_video(o, model_dir, argv[0]) : run_image(o, model_dir);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "运行失败: %s\n", e.what());
        return 3;
    }
}
