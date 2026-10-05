// =============================================================================
//  video_io.h : 视频探测与逐帧解码 (仅在 --video 模式下使用)
//
//  解码方式: 调用 ffmpeg 把视频解成裸 RGB 帧, 通过管道读入 —— 不引入 ffmpeg
//  开发库依赖, 任何 ffmpeg 版本都能用。
//
//  ffmpeg / ffprobe 查找顺序:
//      1) 本程序所在目录
//      2) 系统 PATH
//  两者都找不到时由调用方给出明确报错。
//
//  平台差异(都是踩过的坑, 详见 README 踩坑记录):
//   · Windows cmd 对「以引号开头、且后面还有第二组引号」的命令行有歧义解析 ——
//     它会把第一个引号到最后一个引号之间的内容全当成程序名。所以程序名能不加
//     引号就不加; 路径带空格时先试 8.3 短路径把空格消掉。
//   · glibc 的 popen 只接受 "r"/"w" 这类模式, 传 "rb" 会直接返回 NULL;
//     而 MSVC 的 _popen 恰恰必须用 "rb" 才是二进制模式。必须按平台分开。
// =============================================================================
#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>   // GetShortPathNameA
#include <fcntl.h>
#include <io.h>
#define POPEN_CMD _popen
#define PCLOSE_CMD _pclose
#define PIPE_READ_BIN "rb"     // MSVC 的 _popen 需要 b 才算二进制
#else
#define POPEN_CMD popen
#define PCLOSE_CMD pclose
// 注意: glibc 的 popen 只接受 "r"/"w"/"re"/"we", 传 "rb" 会直接返回 NULL
#define PIPE_READ_BIN "r"
#endif

inline bool path_has_space(const std::string &s) {
    return s.find_first_of(" \t") != std::string::npos;
}

inline std::string exe_dir_of(const char *argv0) {
    if (!argv0) return ".";
    std::string a = argv0;
    size_t pos = a.find_last_of("/\\");
    if (pos == std::string::npos || pos == 0) return ".";
    return a.substr(0, pos);
}

#ifdef _WIN32
inline std::string to_short_path(const std::string &p) {
    char buf[2048];
    DWORD n = GetShortPathNameA(p.c_str(), buf, (DWORD)sizeof(buf));
    if (n > 0 && n < sizeof(buf)) {
        std::string s(buf, n);
        if (!path_has_space(s)) return s;          // 只有真的消掉空格才用
    }
    return p;
}
// need_wrap = true 表示调用方要把整条命令再套一层引号
// （cmd /c ""prog" args" 的惯用写法，用于实在躲不掉空格的情况）
inline std::string quote_program(const std::string &prog, bool &need_wrap) {
    need_wrap = false;
    if (!path_has_space(prog)) return prog;        // 裸名字 / 无空格路径：一律不加引号
    std::string sp = to_short_path(prog);
    if (!path_has_space(sp)) return sp;
    need_wrap = true;
    return "\"" + prog + "\"";
}
inline std::string quote_arg(const std::string &a) {
    if (!path_has_space(a) && a.find('"') == std::string::npos) return a;
    return "\"" + a + "\"";
}
#else
inline std::string quote_program(const std::string &prog, bool &need_wrap) {
    need_wrap = false;
    return "'" + prog + "'";
}
inline std::string quote_arg(const std::string &a) {
    std::string o = "'";                      // POSIX: 单引号包裹最稳
    for (char c : a) { if (c == '\'') o += "'\\''"; else o += c; }
    return o + "'";
}
#endif

// cmd 找不到命令时会往 stderr 打一行 "Can't recognize ..." (Windows) / "not found"
// (POSIX), 很难看, 而且我们自己会给出更清楚的解释, 所以把它的 stderr 静音掉。
inline std::string stderr_null_suffix() {
#ifdef _WIN32
    return " 2>NUL";
#else
    return " 2>/dev/null";
#endif
}

// 执行命令并抓取其 stdout (用于 ffprobe 这种文本输出)
inline std::string run_capture(const std::string &cmd) {
    FILE *p = POPEN_CMD(cmd.c_str(), "r");
    if (!p) return "";
    std::string out;
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), p)) out += buf;
    PCLOSE_CMD(p);
    return out;
}

// 拼一条命令：程序名按平台规则处理，调用方给出的参数已自行 quote_arg 过
inline std::string build_cmd(const std::string &prog, const std::string &args,
                             bool &need_wrap, bool mute_stderr) {
    std::string cmd = quote_program(prog, need_wrap) + " " + args;
    if (need_wrap) return "\"" + cmd + "\"";
    if (mute_stderr) cmd += stderr_null_suffix();
    return cmd;
}

// 找到 ffmpeg / ffprobe: 先看本程序同目录, 再交给系统 PATH。
// 返回空字符串表示两处都没有。
inline std::string find_program(const std::string &name, const char *argv0) {
    std::string dir = exe_dir_of(argv0);
#ifdef _WIN32
    const char *suffixes[] = {".exe", ".bat", ""};
#else
    const char *suffixes[] = {"", ".exe"};
#endif
    for (const char *sfx : suffixes) {
        std::string p = dir + "/" + name + sfx;
        std::FILE *f = std::fopen(p.c_str(), "rb");
        if (f) { std::fclose(f); return p; }
    }
    // 本目录没有 → 用裸名字交给 PATH, 再用 `-version` 探一次确认真的能用
    bool wrap = false;
    std::string out = run_capture(build_cmd(name, "-version", wrap, true));
    return out.empty() ? std::string() : name;
}

// 探测 ffmpeg 是否接受某个选项 (用 `ffmpeg <选项> -version`, 看退出码)
inline bool ffmpeg_accepts(const std::string &ffmpeg, const std::string &opt) {
    bool wrap = false;
    std::string cmd = build_cmd(ffmpeg, "-hide_banner " + opt + " -version", wrap, true);
    FILE *p = POPEN_CMD(cmd.c_str(), "r");
    if (!p) return false;
    char buf[256];
    std::string out;
    while (std::fgets(buf, sizeof(buf), p)) out += buf;
    int rc = PCLOSE_CMD(p);
    return rc == 0 && !out.empty();
}

// 「原样输出每一帧, 不做帧率转换」的选项。**这个不能省** ——
// 不加它, ffmpeg 会把变帧率(VFR)视频按标称帧率上采样(复制帧): 实测一个
// 标称 60fps / 实际 26.7fps 的录屏被扩成 681 帧(源只有 303 帧), 于是管道里
// 第 N 帧不再是源视频第 N 帧, 帧号和时间戳会整体错位, 输出的"时间点"完全对不上画面。
//   ffmpeg >= 5.1 : -fps_mode passthrough   (新版移除了 -vsync)
//   ffmpeg <  5.1 : -vsync 0                (等价写法)
inline std::string detect_passthrough_opt(const std::string &ffmpeg) {
    if (ffmpeg_accepts(ffmpeg, "-fps_mode passthrough")) return "-fps_mode passthrough";
    if (ffmpeg_accepts(ffmpeg, "-vsync 0"))              return "-vsync 0";
    return std::string();                     // 两者都不支持: 只能接受可能的错位
}

// ----------------------------- 视频信息 -----------------------------

struct VideoInfo {
    int w = 0, h = 0;
    double fps = 0.0;           // 实际使用的帧率 (优先取平均帧率)
    double r_fps = 0.0;         // 容器标称帧率 r_frame_rate
    double avg_fps = 0.0;       // 平均帧率 avg_frame_rate
    double duration = 0.0;
    long long reported_frames = -1;
    bool cfr = false;           // 是否可判定为恒定帧率(判定见 classify_frame_rate)
};

// 解析 "30000/1001" 这类帧率写法
inline double parse_rate(const std::string &s) {
    size_t slash = s.find('/');
    if (slash == std::string::npos) return std::atof(s.c_str());
    double num = std::atof(s.substr(0, slash).c_str());
    double den = std::atof(s.substr(slash + 1).c_str());
    return den != 0.0 ? num / den : 0.0;
}

// 判断能否用「帧号 ÷ 帧率」代替真实时间戳。
// 恒定帧率(CFR)视频每帧间隔相同, 时间点可以直接由帧号算出, 于是**不必扫全片 PTS**;
// 变帧率(VFR)视频间隔不均匀, 必须扫。判定要求两个信号都成立才敢走捷径:
//   信号一: 标称帧率 r_frame_rate 与平均帧率 avg_frame_rate 一致(相对差 < 0.5%)
//   信号二: (若时长与总帧数都拿得到) 总帧数 ÷ 时长 也要对得上平均帧率(相对差 < 1%)
// 任一信号可疑就判定为 VFR —— 宁可多扫一遍, 也不让时间点不准。
inline void classify_frame_rate(VideoInfo &vi) {
    vi.cfr = false;
    if (vi.r_fps <= 0.0 || vi.avg_fps <= 0.0) return;
    double diff = std::fabs(vi.r_fps - vi.avg_fps) / std::max(vi.r_fps, vi.avg_fps);
    if (diff > 0.005) return;
    if (vi.duration > 0.0 && vi.reported_frames > 0) {
        double implied = (double)vi.reported_frames / vi.duration;
        if (std::fabs(implied - vi.avg_fps) / vi.avg_fps > 0.01) return;
    }
    vi.cfr = true;
}

inline bool probe_video(const std::string &ffprobe, const std::string &path, VideoInfo &vi) {
    bool wrap = false;
    std::string args = "-v error -select_streams v:0 "
        "-show_entries stream=width,height,r_frame_rate,avg_frame_rate,duration,nb_frames "
        "-of default=noprint_wrappers=1 " + quote_arg(path);
    std::string out = run_capture(build_cmd(ffprobe, args, wrap, true));
    if (out.empty()) return false;

    std::string r_rate, a_rate;
    size_t pos = 0;
    while (pos < out.size()) {
        size_t eol = out.find('\n', pos);
        if (eol == std::string::npos) eol = out.size();
        std::string line = out.substr(pos, eol - pos);
        pos = eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
            line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (v == "N/A" || v.empty()) continue;
        if (k == "width")               vi.w = std::atoi(v.c_str());
        else if (k == "height")         vi.h = std::atoi(v.c_str());
        else if (k == "r_frame_rate")   r_rate = v;
        else if (k == "avg_frame_rate") a_rate = v;
        else if (k == "duration")       vi.duration = std::atof(v.c_str());
        else if (k == "nb_frames")      vi.reported_frames = std::atoll(v.c_str());
    }
    vi.r_fps   = parse_rate(r_rate);
    vi.avg_fps = parse_rate(a_rate);
    vi.fps = vi.avg_fps > 0.0 ? vi.avg_fps : vi.r_fps;
    classify_frame_rate(vi);
    return vi.w > 0 && vi.h > 0;
}

// 取出每一帧的真实显示时间(PTS, 秒)。
// 变帧率(VFR)视频里「帧号 / 平均帧率」会明显偏掉(实测有 0.3 秒级误差),
// 所以时间点必须用 PTS, 抽帧也要按 PTS 选, 不能简单按「每 N 帧」。
inline std::vector<double> probe_frame_times(const std::string &ffprobe,
                                             const std::string &path) {
    bool wrap = false;
    std::string args = "-v error -select_streams v:0 "
        "-show_entries frame=best_effort_timestamp_time -of csv=p=0 " + quote_arg(path);
    std::string out = run_capture(build_cmd(ffprobe, args, wrap, true));

    std::vector<double> pts;
    size_t pos = 0;
    while (pos < out.size()) {
        size_t eol = out.find('\n', pos);
        if (eol == std::string::npos) eol = out.size();
        std::string line = out.substr(pos, eol - pos);
        pos = eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line == "N/A") continue;
        pts.push_back(std::atof(line.c_str()));
    }
    return pts;
}

// 把秒格式化成 时:分:秒.毫秒 (便于看长视频)
inline std::string fmt_time(double sec) {
    if (sec < 0) return "?";
    int ms = (int)std::lround(sec * 1000.0);
    int h = ms / 3600000, m = (ms / 60000) % 60, s = (ms / 1000) % 60, msec = ms % 1000;
    char buf[32];
    if (h > 0) std::snprintf(buf, sizeof(buf), "%d:%02d:%02d.%03d", h, m, s, msec);
    else       std::snprintf(buf, sizeof(buf), "%02d:%02d.%03d", m, s, msec);
    return buf;
}
