// =============================================================================
//  ocr_engine.h : OCR 引擎公共实现 (被 main.cpp 的图片/视频两种模式共用)
//
//  两阶段流水线:
//      [1] 文本检测 DBNet    : 图 -> 概率图 -> 连通域 -> 最小外接旋转矩形 -> 四点坐标
//      [2] 文本识别 CRNN+CTC : 每个框仿射裁剪 -> 定高归一化 -> 序列解码 -> 字符串
//
//  不依赖 OpenCV / PaddlePaddle, 只依赖 ONNX Runtime + 两个 stb 单头文件。
//
//  注意: stb_image / stb_image_write 的 IMPLEMENTATION 宏必须在包含本头文件
//        *之前* 于 .cpp 中定义, 避免同一程序里出现重复实现。
// =============================================================================
#pragma once
#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "stb_image.h"
#include "stb_image_write.h"

// ----------------------------- 可调超参 (对齐 PaddleOCR 默认值) -----------------------------
static const float DET_LIMIT_SIDE = 960.f;  // 检测输入长边上限
static const float DET_THRESH     = 0.3f;   // 概率图二值化阈值 det_db_thresh
static const float DET_BOX_THRESH = 0.6f;   // 文本框得分阈值 det_db_box_thresh
static const float DET_MIN_AREA   = 9.f;    // 最小文本框面积
static const int   REC_IMG_H      = 48;     // CRNN 输入高度
static const int   REC_MIN_W      = 16;
static const int   REC_MAX_W      = 1280;
static const float PI_F           = 3.14159265358979323846f;

// ----------------------------- 基础数据结构 -----------------------------
struct Image {
    int w = 0, h = 0;
    std::vector<unsigned char> px;                                  // RGB, 3 通道
    unsigned char       *at(int x, int y)       { return &px[(size_t)(y * w + x) * 3]; }
    const unsigned char *at(int x, int y) const { return &px[(size_t)(y * w + x) * 3]; }
};

struct Point { float x = 0, y = 0; };

// 旋转矩形: 四点顺序固定为 左上, 右上, 右下, 左下
struct Quad {
    Point p[4];
    float w() const { return std::hypot(p[1].x - p[0].x, p[1].y - p[0].y); }
    float h() const { return std::hypot(p[3].x - p[0].x, p[3].y - p[0].y); }
    float xmin() const { return std::min(std::min(p[0].x, p[1].x), std::min(p[2].x, p[3].x)); }
    float ymin() const { return std::min(std::min(p[0].y, p[1].y), std::min(p[2].y, p[3].y)); }
    float xmax() const { return std::max(std::max(p[0].x, p[1].x), std::max(p[2].x, p[3].x)); }
    float ymax() const { return std::max(std::max(p[0].y, p[1].y), std::max(p[2].y, p[3].y)); }
};

struct TextBox {
    Quad quad;
    std::string text;
    float score = 0.f;
};

inline float clampf(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }

inline bool file_exists(const std::string &p) {
    std::ifstream f(p, std::ios::binary);
    return f.good();
}

// 模型目录解析: 依次从 exe 目录、当前目录向上搜 models/, 返回第一个真正可用的。
// 「可用」= 该目录下同时存在当前模型版本所需的检测/识别模型与字典。
// 都找不到时返回空字符串, 由调用方给出报错。
inline std::string find_model_dir(char *argv0, bool v5) {
    const std::string det = v5 ? "det_v5.onnx" : "det_v4.onnx";
    const std::string rec = v5 ? "rec_v5.onnx" : "rec_v4.onnx";
    const std::string key = v5 ? "keys_v5.txt" : "keys_v4.txt";
    auto ok = [&](const std::string &d) {
        if (d.empty()) return false;
        return file_exists(d + "/" + det) && file_exists(d + "/" + rec) &&
               file_exists(d + "/" + key);
    };

    // 可执行文件所在目录 (argv0 里没有路径分隔符时按当前目录算)
    std::string exe_dir = ".";
    if (argv0) {
        std::string a = argv0;
        size_t pos = a.find_last_of("/\\");
        if (pos != std::string::npos && pos > 0) exe_dir = a.substr(0, pos);
    }

    // 从 exe 目录、当前目录分别向上找, 这样「源码目录里跑」「cpp/ 里跑」
    // 「cpp/build/ 里跑」「便携版目录里双击」都能自动命中。
    std::vector<std::string> cands;
    std::string d = exe_dir;
    for (int i = 0; i < 5; ++i) {
        cands.push_back(d + "/models");
        size_t p = d.find_last_of("/\\");
        if (p == std::string::npos || p == 0) break;
        d = d.substr(0, p);
    }
    d = ".";
    for (int i = 0; i < 4; ++i) {
        cands.push_back(d + "/models");
        d += "/..";
    }
    for (auto &c : cands) if (ok(c)) return c;
    return std::string();
}

// ----------------------------- 图像 IO / 采样 -----------------------------
inline Image load_image(const std::string &path) {
    Image img;
    int n = 0;
    unsigned char *d = stbi_load(path.c_str(), &img.w, &img.h, &n, 3);
    if (!d) { std::fprintf(stderr, "无法读取图片: %s\n", path.c_str()); return img; }
    img.px.assign(d, d + (size_t)img.w * img.h * 3);
    stbi_image_free(d);
    return img;
}

// 返回是否写成功 (目录不存在/无权限时返回 false, 由调用方决定怎么提示)
inline bool save_image(const std::string &path, const Image &img) {
    return stbi_write_png(path.c_str(), img.w, img.h, 3, img.px.data(), img.w * 3) != 0;
}

inline Image resize_bilinear(const Image &src, int nw, int nh) {
    Image dst;
    dst.w = nw; dst.h = nh;
    dst.px.resize((size_t)nw * nh * 3);
    float sx = (float)src.w / nw, sy = (float)src.h / nh;
    for (int y = 0; y < nh; ++y) {
        float fy = (y + 0.5f) * sy - 0.5f;
        int iy = (int)std::floor(fy); float wy = fy - iy;
        int y0 = std::min(std::max(iy, 0), src.h - 1);
        int y1 = std::min(std::max(iy + 1, 0), src.h - 1);
        for (int x = 0; x < nw; ++x) {
            float fx = (x + 0.5f) * sx - 0.5f;
            int ix = (int)std::floor(fx); float wx = fx - ix;
            int x0 = std::min(std::max(ix, 0), src.w - 1);
            int x1 = std::min(std::max(ix + 1, 0), src.w - 1);
            unsigned char *o = dst.at(x, y);
            for (int c = 0; c < 3; ++c) {
                float v = src.at(x0, y0)[c] * (1 - wx) * (1 - wy) + src.at(x1, y0)[c] * wx * (1 - wy)
                        + src.at(x0, y1)[c] * (1 - wx) * wy     + src.at(x1, y1)[c] * wx * wy;
                o[c] = (unsigned char)clampf(v + 0.5f, 0.f, 255.f);
            }
        }
    }
    return dst;
}

inline unsigned char sample_bilinear(const Image &img, float x, float y, int c) {
    x = clampf(x, 0.f, (float)img.w - 1.f);
    y = clampf(y, 0.f, (float)img.h - 1.f);
    int x0 = (int)x, y0 = (int)y;
    int x1 = std::min(x0 + 1, img.w - 1), y1 = std::min(y0 + 1, img.h - 1);
    float wx = x - x0, wy = y - y0;
    float v = img.at(x0, y0)[c] * (1 - wx) * (1 - wy) + img.at(x1, y0)[c] * wx * (1 - wy)
            + img.at(x0, y1)[c] * (1 - wx) * wy     + img.at(x1, y1)[c] * wx * wy;
    return (unsigned char)clampf(v + 0.5f, 0.f, 255.f);
}

// ----------------------------- 最小外接旋转矩形 -----------------------------
inline std::vector<Point> convex_hull(std::vector<Point> pts) {
    std::sort(pts.begin(), pts.end(), [](const Point &a, const Point &b) {
        return a.x < b.x || (a.x == b.x && a.y < b.y);
    });
    int n = (int)pts.size();
    if (n < 3) return pts;
    std::vector<Point> h((size_t)2 * n);
    int k = 0;
    auto cross = [](const Point &o, const Point &a, const Point &b) {
        return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
    };
    for (int i = 0; i < n; ++i) {                                    // 下凸包
        while (k >= 2 && cross(h[k - 2], h[k - 1], pts[i]) <= 0) --k;
        h[k++] = pts[i];
    }
    for (int i = n - 2, t = k + 1; i >= 0; --i) {                    // 上凸包
        while (k >= t && cross(h[k - 2], h[k - 1], pts[i]) <= 0) --k;
        h[k++] = pts[i];
    }
    h.resize(k - 1);
    return h;
}

// 旋转卡壳: 枚举凸包每条边作为基准方向, 取面积最小的外接矩形
inline Quad min_area_rect(const std::vector<Point> &pts) {
    std::vector<Point> hull = convex_hull(pts);
    Quad q{};
    if (hull.size() < 3) {                                           // 退化: 退化为轴对齐框
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (auto &p : pts) {
            x0 = std::min(x0, p.x); y0 = std::min(y0, p.y);
            x1 = std::max(x1, p.x); y1 = std::max(y1, p.y);
        }
        q.p[0] = {x0, y0}; q.p[1] = {x1, y0}; q.p[2] = {x1, y1}; q.p[3] = {x0, y1};
        return q;
    }
    float best_area = 1e18f, best_ang = 0.f, bcx = 0, bcy = 0, bw = 0, bh = 0;
    int m = (int)hull.size();
    for (int i = 0; i < m; ++i) {
        Point a = hull[i], b = hull[(i + 1) % m];
        float ang = std::atan2(b.y - a.y, b.x - a.x);
        while (ang > PI_F / 2) ang -= PI_F;                          // 归一到 (-90°, 90°]
        while (ang <= -PI_F / 2) ang += PI_F;
        float ca = std::cos(-ang), sa = std::sin(-ang);
        float x0 = 1e18f, y0 = 1e18f, x1 = -1e18f, y1 = -1e18f;
        for (auto &p : hull) {
            float rx = p.x * ca - p.y * sa, ry = p.x * sa + p.y * ca;
            x0 = std::min(x0, rx); x1 = std::max(x1, rx);
            y0 = std::min(y0, ry); y1 = std::max(y1, ry);
        }
        float area = (x1 - x0) * (y1 - y0);
        if (area < best_area) {
            best_area = area; best_ang = ang; bw = x1 - x0; bh = y1 - y0;
            float mx = (x0 + x1) / 2, my = (y0 + y1) / 2;            // 中心旋转回原坐标系
            float cb = std::cos(ang), sb = std::sin(ang);
            bcx = mx * cb - my * sb; bcy = mx * sb + my * cb;
        }
    }
    // 参考边恰好竖直时 (ang = 90°), 换算成等价的水平参考, 保证局部 x 轴朝右、
    // 局部 y 轴朝下 —— 否则算出的四点顺序会上下翻转, 裁剪图变成 180° 倒影。
    if (best_ang > PI_F / 2 - 1e-5f) {
        best_ang -= PI_F / 2;
        std::swap(bw, bh);
    }
    float c = std::cos(best_ang), s = std::sin(best_ang);
    auto corner = [&](float dx, float dy) {
        return Point{bcx + dx * c - dy * s, bcy + dx * s + dy * c};
    };
    q.p[0] = corner(-bw / 2, -bh / 2);                               // 左上
    q.p[1] = corner(bw / 2, -bh / 2);                                // 右上
    q.p[2] = corner(bw / 2, bh / 2);                                 // 右下
    q.p[3] = corner(-bw / 2, bh / 2);                                // 左下
    return q;
}

// PaddleOCR 的 box_score_fast: 取最小外接矩形内的平均概率
inline bool point_in_quad(const Point *p, float x, float y) {
    int sign = 0;
    for (int i = 0; i < 4; ++i) {
        const Point &a = p[i], &b = p[(i + 1) % 4];
        float cr = (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
        int s = cr > 0 ? 1 : (cr < 0 ? -1 : 0);
        if (s) {
            if (sign && s != sign) return false;
            sign = s;
        }
    }
    return true;
}

inline float box_score_fast(const float *prob, int pw, int ph, const Quad &q) {
    int x0 = std::max(0, (int)std::floor(q.xmin()));
    int x1 = std::min(pw, (int)std::ceil(q.xmax()) + 1);
    int y0 = std::max(0, (int)std::floor(q.ymin()));
    int y1 = std::min(ph, (int)std::ceil(q.ymax()) + 1);
    double sum = 0.0;
    int cnt = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            if (point_in_quad(q.p, x + 0.5f, y + 0.5f)) { sum += prob[(size_t)y * pw + x]; ++cnt; }
    return cnt ? (float)(sum / cnt) : 0.f;
}

// ----------------------------- OCR 引擎 -----------------------------
class OcrEngine {
public:
    OcrEngine(const std::string &model_dir, bool v5, int threads, float unclip)
        : env_(ORT_LOGGING_LEVEL_WARNING, "ocr"), unclip_(unclip) {
        Ort::SessionOptions so;
        so.SetIntraOpNumThreads(threads);
        so.SetGraphOptimizationLevel(ORT_ENABLE_ALL);
        det_ = create_session(join(model_dir, v5 ? "det_v5.onnx" : "det_v4.onnx"), so);
        rec_ = create_session(join(model_dir, v5 ? "rec_v5.onnx" : "rec_v4.onnx"), so);

        std::ifstream f(join(model_dir, v5 ? "keys_v5.txt" : "keys_v4.txt"), std::ios::binary);
        if (!f) throw std::runtime_error("缺少字典文件, 请先执行 models/download_models.py");
        std::string line;
        chars_.push_back("");                                        // 0 = CTC blank
        while (std::getline(f, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            if (!line.empty()) chars_.push_back(line);
        }
        chars_.push_back(" ");                                       // 末位 = 空格
        det_in_ = name_of(det_, true);   det_out_ = name_of(det_, false);
        rec_in_ = name_of(rec_, true);   rec_out_ = name_of(rec_, false);
    }

    std::vector<TextBox> run(const Image &img) {
        std::vector<Quad> boxes = detect(img);
        std::vector<TextBox> out;
        for (auto &q : boxes) {
            Image crop = crop_quad(img, q, REC_IMG_H);
            if (crop.w <= 0) continue;
            auto r1 = recognize(crop);
            std::string best_text = r1.first;
            float best_score = r1.second;
            float mean = 0.f;                                        // 深色底(标题栏/终端)反色重试
            for (auto v : crop.px) mean += v;
            mean /= (float)crop.px.size();
            if (mean < 110.f) {
                Image inv = crop;
                for (auto &v : inv.px) v = 255 - v;
                auto r2 = recognize(inv);
                if (r2.second > best_score) { best_text = r2.first; best_score = r2.second; }
            }
            if (best_text.empty()) continue;
            TextBox tb; tb.quad = q; tb.text = best_text; tb.score = best_score;
            out.push_back(tb);
        }
        return order_by_lines(out);
    }

private:
    Ort::Env env_{nullptr};
    Ort::Session det_{nullptr}, rec_{nullptr};
    std::vector<std::string> chars_;
    float unclip_ = 2.f;
    std::string det_in_, det_out_, rec_in_, rec_out_;

    static std::string join(const std::string &d, const std::string &f) {
#ifdef _WIN32
        return d + "\\" + f;
#else
        return d + "/" + f;
#endif
    }
    // Windows 下 ORT 需要宽字符路径
    Ort::Session create_session(const std::string &path, Ort::SessionOptions &so) {
#ifdef _WIN32
        std::wstring wp(path.begin(), path.end());
        return Ort::Session(env_, wp.c_str(), so);
#else
        return Ort::Session(env_, path.c_str(), so);
#endif
    }
    static std::string name_of(Ort::Session &s, bool input) {
        Ort::AllocatorWithDefaultOptions alloc;
        Ort::AllocatedStringPtr p = input ? s.GetInputNameAllocated(0, alloc)
                                          : s.GetOutputNameAllocated(0, alloc);
        return std::string(p.get());
    }
    static std::vector<int64_t> shape4(int64_t a, int64_t b, int64_t c, int64_t d) {
        return {a, b, c, d};
    }

    // ========== 第一阶段: 文本检测 (DBNet) ==========
    std::vector<Quad> detect(const Image &img) {
        float ratio = std::min(1.f, DET_LIMIT_SIDE / (float)std::max(img.w, img.h));
        int nw = std::max(32, (int)std::lround(img.w * ratio / 32) * 32);
        int nh = std::max(32, (int)std::lround(img.h * ratio / 32) * 32);
        Image resized = resize_bilinear(img, nw, nh);

        // 归一化: RGB, (x/255 - mean) / std, NCHW
        const float mean[3] = {0.485f, 0.456f, 0.406f};
        const float stdv[3] = {0.229f, 0.224f, 0.225f};
        std::vector<float> blob((size_t)3 * nh * nw);
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x) {
                const unsigned char *p = resized.at(x, y);
                for (int c = 0; c < 3; ++c)
                    blob[(size_t)c * nh * nw + (size_t)y * nw + x] = (p[c] / 255.f - mean[c]) / stdv[c];
            }

        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        auto in_shape = shape4(1, 3, nh, nw);
        Ort::Value in = Ort::Value::CreateTensor<float>(mem, blob.data(), blob.size(),
                                                        in_shape.data(), in_shape.size());
        const char *names_in[]  = {det_in_.c_str()};
        const char *names_out[] = {det_out_.c_str()};
        auto outs = det_.Run(Ort::RunOptions{nullptr}, names_in, &in, 1, names_out, 1);
        auto oshape = outs[0].GetTensorTypeAndShapeInfo().GetShape();
        const float *prob = outs[0].GetTensorData<float>();
        int ph = (int)oshape[2], pw = (int)oshape[3];
        float sx = (float)img.w / pw, sy = (float)img.h / ph;

        // 二值化 -> 2x2 膨胀(同 PaddleOCR) -> 8 邻域连通域
        std::vector<unsigned char> mask((size_t)pw * ph, 0);
        for (size_t i = 0; i < mask.size(); ++i) mask[i] = prob[i] > DET_THRESH ? 1 : 0;
        std::vector<unsigned char> dil = mask;
        for (int y = 0; y < ph; ++y)
            for (int x = 0; x < pw; ++x)
                if (mask[(size_t)y * pw + x])
                    for (int dy = 0; dy < 2; ++dy)
                        for (int dx = 0; dx < 2; ++dx)
                            if (y + dy < ph && x + dx < pw) dil[(size_t)(y + dy) * pw + x + dx] = 1;

        std::vector<int> label((size_t)pw * ph, -1);
        std::vector<std::vector<Point>> comps;
        std::vector<int> stack;
        for (int y = 0; y < ph; ++y)
            for (int x = 0; x < pw; ++x) {
                size_t idx = (size_t)y * pw + x;
                if (!dil[idx] || label[idx] >= 0) continue;
                int id = (int)comps.size();
                comps.emplace_back();
                stack.clear(); stack.push_back((int)idx); label[idx] = id;
                while (!stack.empty()) {
                    int cur = stack.back(); stack.pop_back();
                    int cx = cur % pw, cy = cur / pw;
                    comps[id].push_back({(float)cx + 0.5f, (float)cy + 0.5f});
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            int nx = cx + dx, ny = cy + dy;
                            if (nx < 0 || ny < 0 || nx >= pw || ny >= ph) continue;
                            size_t ni = (size_t)ny * pw + nx;
                            if (dil[ni] && label[ni] < 0) { label[ni] = id; stack.push_back((int)ni); }
                        }
                }
            }

        // 每个连通域 -> 最小外接旋转矩形 -> 得分过滤 -> 外扩 -> 映射回原图坐标
        std::vector<Quad> boxes;
        for (auto &comp : comps) {
            if (comp.size() < 4) continue;
            Quad q = min_area_rect(comp);
            float qw = q.w(), qh = q.h();
            if (qw * qh < DET_MIN_AREA) continue;
            if (box_score_fast(prob, pw, ph, q) < DET_BOX_THRESH) continue;  // det_db_box_thresh

            float d = qw * qh * unclip_ / (2.f * (qw + qh) + 1e-6f);   // det_db_unclip_ratio
            Point c{(q.p[0].x + q.p[2].x) / 2, (q.p[0].y + q.p[2].y) / 2};
            Point ux{(q.p[1].x - q.p[0].x) / qw, (q.p[1].y - q.p[0].y) / qw};
            Point uy{(q.p[3].x - q.p[0].x) / qh, (q.p[3].y - q.p[0].y) / qh};
            float ex = qw / 2 + d, ey = qh / 2 + d;
            const float sxn[4] = {-1, 1, 1, -1}, syn[4] = {-1, -1, 1, 1};
            for (int i = 0; i < 4; ++i) {
                q.p[i].x = c.x + ux.x * ex * sxn[i] + uy.x * ey * syn[i];
                q.p[i].y = c.y + ux.y * ex * sxn[i] + uy.y * ey * syn[i];
            }
            for (int i = 0; i < 4; ++i) {                              // 缩放到原图坐标系
                q.p[i].x = clampf(q.p[i].x * sx, 0.f, (float)img.w - 1);
                q.p[i].y = clampf(q.p[i].y * sy, 0.f, (float)img.h - 1);
            }
            boxes.push_back(q);
        }
        return boxes;
    }

    // ========== 第二阶段: 文本识别 (CRNN + CTC) ==========
    std::pair<std::string, float> recognize(const Image &crop) {
        std::vector<float> blob((size_t)3 * crop.h * crop.w);
        for (int y = 0; y < crop.h; ++y)
            for (int x = 0; x < crop.w; ++x) {
                const unsigned char *p = crop.at(x, y);
                for (int c = 0; c < 3; ++c)
                    blob[(size_t)c * crop.h * crop.w + (size_t)y * crop.w + x] = (p[c] / 255.f - 0.5f) / 0.5f;
            }
        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        auto in_shape = shape4(1, 3, crop.h, crop.w);
        Ort::Value in = Ort::Value::CreateTensor<float>(mem, blob.data(), blob.size(),
                                                        in_shape.data(), in_shape.size());
        const char *names_in[]  = {rec_in_.c_str()};
        const char *names_out[] = {rec_out_.c_str()};
        auto outs = rec_.Run(Ort::RunOptions{nullptr}, names_in, &in, 1, names_out, 1);
        auto oshape = outs[0].GetTensorTypeAndShapeInfo().GetShape();
        int T = (int)oshape[1], C = (int)oshape[2];
        if (oshape.size() == 4) { T = (int)(oshape[1] * oshape[2]); C = (int)oshape[3]; }
        const float *logits = outs[0].GetTensorData<float>();

        std::string text; float conf = 0.f; int cnt = 0, prev = -1;
        for (int t = 0; t < T; ++t) {                     // 贪心 CTC: 去 blank + 相邻去重
            const float *row = logits + (size_t)t * C;
            int arg = 0; float best = row[0];
            for (int c = 1; c < C; ++c) if (row[c] > best) { best = row[c]; arg = c; }
            if (arg != 0 && arg != prev) {
                if (arg < (int)chars_.size()) text += chars_[arg];
                conf += best; ++cnt;
            }
            prev = arg;
        }
        return {text, cnt ? conf / cnt : 0.f};
    }

    // 把倾斜文本框摆正, 并直接采样到识别所需尺寸 (只做一次重采样)
    static Image crop_quad(const Image &img, const Quad &q, int out_h) {
        float w = q.w(), h = q.h();
        if (w < 1.f || h < 1.f) return Image{};
        int cw = std::min(std::max((int)std::lround((float)out_h * w / h), REC_MIN_W), REC_MAX_W);
        Image dst; dst.w = cw; dst.h = out_h;
        dst.px.resize((size_t)cw * out_h * 3);
        for (int y = 0; y < out_h; ++y) {
            float v = (y + 0.5f) / out_h;
            for (int x = 0; x < cw; ++x) {
                float u = (x + 0.5f) / cw;
                // 旋转矩形是平行四边形, 仿射映射即可精确对应
                float srcx = q.p[0].x + (q.p[1].x - q.p[0].x) * u + (q.p[3].x - q.p[0].x) * v;
                float srcy = q.p[0].y + (q.p[1].y - q.p[0].y) * u + (q.p[3].y - q.p[0].y) * v;
                unsigned char *o = dst.at(x, y);
                for (int c = 0; c < 3; ++c) o[c] = sample_bilinear(img, srcx, srcy, c);
            }
        }
        return dst;
    }

    // 按"纵向重叠"聚行, 行内从左到右 —— 自动排版
    static std::vector<TextBox> order_by_lines(std::vector<TextBox> boxes) {
        std::sort(boxes.begin(), boxes.end(), [](const TextBox &a, const TextBox &b) {
            return a.quad.ymin() < b.quad.ymin();
        });
        std::vector<std::vector<TextBox>> lines;
        for (auto &b : boxes) {
            bool placed = false;
            for (auto &ln : lines) {
                float ly0 = 1e9f, ly1 = -1e9f;
                for (auto &t : ln) {
                    ly0 = std::min(ly0, t.quad.ymin());
                    ly1 = std::max(ly1, t.quad.ymax());
                }
                float bh = b.quad.ymax() - b.quad.ymin(), lh = ly1 - ly0;
                float ov = std::min(b.quad.ymax(), ly1) - std::max(b.quad.ymin(), ly0);
                if (ov > 0.5f * std::min(bh, lh)) { ln.push_back(b); placed = true; break; }
            }
            if (!placed) lines.push_back({b});
        }
        std::sort(lines.begin(), lines.end(),
                  [](const std::vector<TextBox> &a, const std::vector<TextBox> &b) {
                      float ay = 1e9f, by = 1e9f;
                      for (auto &t : a) ay = std::min(ay, t.quad.ymin());
                      for (auto &t : b) by = std::min(by, t.quad.ymin());
                      return ay < by;
                  });
        std::vector<TextBox> out;
        for (auto &ln : lines) {
            std::sort(ln.begin(), ln.end(), [](const TextBox &a, const TextBox &b) {
                return a.quad.xmin() < b.quad.xmin();
            });
            for (auto &t : ln) out.push_back(t);
        }
        return out;
    }
};

// ----------------------------- 可视化: 画框 + 序号 -----------------------------
inline void put_pixel(Image &img, int x, int y, unsigned char r, unsigned char g, unsigned char b) {
    if (x < 0 || y < 0 || x >= img.w || y >= img.h) return;
    unsigned char *p = img.at(x, y);
    p[0] = r; p[1] = g; p[2] = b;
}
inline void draw_line(Image &img, float x0, float y0, float x1, float y1,
                      unsigned char r, unsigned char g, unsigned char b, int t = 2) {
    int n = (int)(std::max(std::fabs(x1 - x0), std::fabs(y1 - y0)) * 2) + 1;
    for (int i = 0; i <= n; ++i) {
        float u = (float)i / n;
        int x = (int)std::lround(x0 + (x1 - x0) * u), y = (int)std::lround(y0 + (y1 - y0) * u);
        for (int dy = 0; dy < t; ++dy)
            for (int dx = 0; dx < t; ++dx) put_pixel(img, x + dx, y + dy, r, g, b);
    }
}
// 5x7 点阵数字, 用来在图上标序号
static const unsigned char DIGITS[10][7] = {
    {0x1E,0x11,0x13,0x15,0x19,0x11,0x0E}, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
};
inline void draw_number(Image &img, int n, int x, int y,
                        unsigned char r, unsigned char g, unsigned char b) {
    std::string s = std::to_string(n);
    for (char ch : s) {
        int d = ch - '0';
        for (int row = 0; row < 7; ++row)
            for (int col = 0; col < 5; ++col)
                if (DIGITS[d][row] & (1 << (4 - col)))
                    for (int dy = 0; dy < 2; ++dy)
                        for (int dx = 0; dx < 2; ++dx)
                            put_pixel(img, x + col * 2 + dx, y + row * 2 + dy, r, g, b);
        x += 12;
    }
}

inline std::string json_escape(const std::string &s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}

