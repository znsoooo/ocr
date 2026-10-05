// =============================================================================
//  dedup.h : 视频逐帧识别的跨帧去重
//
//  视频里同一句字幕会跨很多帧出现, 而每一帧的识别结果并不完全相同:
//    · 检测框抖动 → 同一行的右侧被截在不同位置, 于是得到长短不一的版本
//    · 标点抖动   → `、` 与 `·` 互换、句末句号/省略号时有时无
//  逐帧全量输出会刷屏, 所以这里做两件事:
//    1) 去重      : 同一段文字只输出一次
//    2) 抑制中间态: 字幕"还没打印完整"的版本(最终版本的子集)不输出
//
//  判定"同一段"要同时满足:
//      位置重叠  +  文本相同/包含  +  时间邻近(距该段上一次被命中)
//
//  算法是单遍在线贪心, 只保留活跃段, 内存与视频长度无关。
//  纯后处理, 不接触 OCR 引擎; 图片模式不受影响。
// =============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace dedup {

// --------------------------------------------------------------- UTF-8 工具

// 按码点切分 (一个汉字 / 一个 ASCII 字符各算一个元素)
inline std::vector<std::string> utf8_chars(const std::string &s) {
    std::vector<std::string> out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        size_t n = 1;
        if (c >= 0xF0)      n = 4;
        else if (c >= 0xE0) n = 3;
        else if (c >= 0xC0) n = 2;
        if (i + n > s.size()) n = 1;
        out.push_back(s.substr(i, n));
        i += n;
    }
    return out;
}

// 归一化: 去掉空白与中英标点。
//   `、` / `·` 互换、句末标点增减、省略号丢失都是最常见的识别抖动,
//   去掉标点后这些差异直接消失, 而不同句子之间的区分度不受影响。
inline std::string normalize(const std::string &s) {
    static const std::vector<std::string> kPunct = utf8_chars(
        " \t\r\n,.;:!?\"'`()[]{}<>|/\\~-_=+*&^%$#@"
        "、。，；：！？…—·「」『』（）【】《》〈〉“”‘’～　");
    std::string out;
    for (const auto &ch : utf8_chars(s)) {
        bool drop = false;
        for (const auto &p : kPunct) if (p == ch) { drop = true; break; }
        if (!drop) out += ch;
    }
    return out;
}

// 最长公共子序列长度 (滚动数组, 文本很短, 够用)
inline size_t lcs_len(const std::vector<std::string> &a, const std::vector<std::string> &b) {
    const size_t n = a.size(), m = b.size();
    if (!n || !m) return 0;
    std::vector<size_t> prev(m + 1, 0), cur(m + 1, 0);
    for (size_t i = 1; i <= n; ++i) {
        for (size_t j = 1; j <= m; ++j) {
            cur[j] = (a[i - 1] == b[j - 1]) ? prev[j - 1] + 1
                                            : std::max(prev[j], cur[j - 1]);
        }
        prev.swap(cur);
        std::fill(cur.begin(), cur.end(), 0);
    }
    return prev[m];
}

// 归一化文本之间的相似度 (0~1)
inline double text_sim(const std::string &na, const std::string &nb) {
    if (na.empty() || nb.empty()) return na == nb ? 1.0 : 0.0;
    auto a = utf8_chars(na), b = utf8_chars(nb);
    return (double)(2 * lcs_len(a, b)) / (double)(a.size() + b.size());
}

// 两段文字是否属于"同一句"(输入须为已归一化的文本)
//   除了相似度, 还认"包含/扩展"关系 —— 字幕没打印完整时正是最终版本的子集,
//   此时按普通相似度算分会偏低(例: 20 字 vs 22 字 → 0.95 没问题,
//   但 2 字 vs 6 字只有 0.5), 所以短的那段只要几乎被覆盖, 就算同句。
//   注意 mn>=1: 逐字打印时会出现"这"→"这是"→"这是一"这种单字起步的扩展,
//   若要求短串至少 2 字, 第一个字就会被误判成独立的一段。
inline bool same_segment(const std::string &na, const std::string &nb, double sim_thresh) {
    if (na.empty() || nb.empty()) return na == nb;
    if (na == nb) return true;
    auto a = utf8_chars(na), b = utf8_chars(nb);
    const size_t l = lcs_len(a, b);
    const size_t mn = std::min(a.size(), b.size());
    if (mn >= 1 && (double)l / (double)mn >= 0.85) return true;      // 包含 / 扩展
    return (double)(2 * l) / (double)(a.size() + b.size()) >= sim_thresh;
}

// ------------------------------------------------------------------- 数据

// 送去重的一行 (四点坐标用整帧坐标系)
struct Item {
    std::string text;
    float score = 0.f;
    float x[4] = {0, 0, 0, 0};
    float y[4] = {0, 0, 0, 0};

    float bx0() const { return std::min(std::min(x[0], x[1]), std::min(x[2], x[3])); }
    float bx1() const { return std::max(std::max(x[0], x[1]), std::max(x[2], x[3])); }
    float by0() const { return std::min(std::min(y[0], y[1]), std::min(y[2], y[3])); }
    float by1() const { return std::max(std::max(y[0], y[1]), std::max(y[2], y[3])); }
    float cx()  const { return (bx0() + bx1()) * 0.5f; }
    float cy()  const { return (by0() + by1()) * 0.5f; }
    float h()   const { return by1() - by0(); }
};

// 应当输出到同一帧的一组行
struct Group {
    long long frame = 0;
    double time = 0.0;
    std::vector<Item> items;
};

// 位置重叠程度 (0 表示不算同一个"字幕槽位")
inline double pos_overlap(const Item &a, const Item &b) {
    const float x0 = std::max(a.bx0(), b.bx0()), x1 = std::min(a.bx1(), b.bx1());
    const float y0 = std::max(a.by0(), b.by0()), y1 = std::min(a.by1(), b.by1());
    const double inter = (x1 > x0 && y1 > y0) ? (double)(x1 - x0) * (y1 - y0) : 0.0;
    const double ar1 = (double)(a.bx1() - a.bx0()) * (a.by1() - a.by0());
    const double ar2 = (double)(b.bx1() - b.bx0()) * (b.by1() - b.by0());
    const double uni = ar1 + ar2 - inter;
    if (uni > 0.0 && inter / uni > 0.05) return inter / uni;
    // 补充判据: 字幕轻微漂移时 IoU 掉得很快, 中心距够近也算同一槽位。
    // 但必须限制两行的宽度差 —— 否则句首被单独切出来的短词(实测「哎，」宽 21px)
    // 会跑来认领整句(宽 452px)的槽位, 把真正的主句挤成"新的一句"。
    const float w1 = a.bx1() - a.bx0(), w2 = b.bx1() - b.bx0();
    const float wa = std::min(w1, w2), wb = std::max(w1, w2);
    const float hh = std::max(a.h(), b.h());
    const float dy = std::fabs(a.cy() - b.cy());
    const float dx = std::fabs(a.cx() - b.cx());
    if (hh > 0 && wa > 0.25f * wb && dy < 0.6f * hh && dx < 0.5f * wb) return 0.10;
    return 0.0;
}

// 哪个版本更完整:
//   1) 先比"有多少字"(归一化后长度) —— 内容更全
//   2) 再比原始长度              —— 标点更齐全(如「坏人」比「坏人多一个右引号)
//   3) 最后比置信度
inline bool more_complete(const Item &a, size_t a_norm, size_t a_raw,
                          const Item &b, size_t b_norm, size_t b_raw) {
    if (a_norm != b_norm) return a_norm > b_norm;
    if (a_raw  != b_raw)  return a_raw  > b_raw;
    return a.score > b.score;
}

// ------------------------------------------------------------------- 去重器

class LineDedup {
public:
    struct Config {
        double similarity = 0.75;   // 文本相似度阈值(实测: 同句 0.91~0.98, 异句 0.00~0.21)
        double stable_sec = 1.0;    // 内容保持不变超过这么久, 就认为"打印完了"
        double gap_sec    = 2.0;    // 槽位匹配窗口: 中间断多久之内仍复用同一个位置槽位
        double reset_ratio = 3.0;   // "断开"判定: 缺失超过这么多个采样点, 就算新的一段
    };

    LineDedup() = default;
    explicit LineDedup(double sample_interval) { set_interval(sample_interval); }
    LineDedup(double sample_interval, const Config &c) : c_(c) { set_interval(sample_interval); }

    void set_interval(double s) {
        interval_ = s > 0 ? s : 0.5;
        // 抽帧越稀疏, 这些阈值都要按比例放大, 否则同一句会被采样抖动切成好几段。
        stable_ = std::max(c_.stable_sec, 2.0 * interval_);
        gap_    = std::max(c_.gap_sec,    3.0 * interval_);
        reset_  = c_.reset_ratio * interval_;
    }

    // 送入一帧, 返回本帧应当打印的内容(可能为空, 也可能含多条不同帧号的记录)
    std::vector<Group> feed(long long frame, double ts, const std::vector<Item> &items);

    // 视频结束: 把还在等待确认的段全部吐出
    std::vector<Group> flush();

    // 被抑制掉的行数(重复 + 中间态), 仅用于汇总显示
    long long suppressed() const { return suppressed_; }

private:
    struct Slot {
        Item last;                          // 最近一次出现的行
        std::string last_norm;

        bool has_seg = false;               // 当前正在积累的段
        Item best;                          // 本段见过的最完整版本
        std::string best_norm;
        size_t best_len = 0;                // best 的归一化长度(有多少"字")
        size_t best_raw = 0;                // best 的原始字节长度(标点是否齐全)
        long long best_frame = 0;
        double best_time = 0.0;
        double changed_at = 0.0;            // best 最后一次更新的时刻(用于稳定判定)

        bool has_emitted = false;           // 本槽位最近输出过的内容
        std::string emitted_norm;

        double last_seen = 0.0;
        bool alive = false;
    };

    void emit(std::vector<Group> &out, Slot &s) {
        if (!s.has_seg) return;
        bool merged = false;
        for (auto &g : out) {
            if (g.frame == s.best_frame) { g.items.push_back(s.best); merged = true; break; }
        }
        if (!merged) {
            Group g;
            g.frame = s.best_frame;
            g.time = s.best_time;
            g.items.push_back(s.best);
            out.push_back(g);
        }
        s.has_emitted = true;
        s.emitted_norm = s.best_norm;
        s.has_seg = false;
    }

    Config c_{};
    double interval_ = 0.5;
    double stable_ = 1.0;                   // 内容保持多久算"打印完"
    double gap_ = 2.0;                      // 槽位匹配窗口
    double reset_ = 1.5;                    // "断开"判定窗口
    std::vector<Slot> slots_;
    long long suppressed_ = 0;
};

inline std::vector<Group> LineDedup::feed(long long frame, double ts,
                                          const std::vector<Item> &items) {
    std::vector<Group> out;

    std::vector<std::string> normed(items.size());
    for (size_t i = 0; i < items.size(); ++i) normed[i] = normalize(items[i].text);

    // ---- 1. 匹配: 按位置把本帧的行分派给活跃槽位 ----
    // 这里**只看位置, 不看文本** —— 同一个物理位置(字幕槽位)在时间上会先后
    // 承载很多句不同的话, 换句时必须复用同一个槽位, 否则会不停新建槽位、
    // 并且在旧槽位还活着的时候匹配错。
    //
    // 分配算法是"全局贪心": 先把所有(行, 槽位)候选按重叠程度排序, 再依次认领。
    // 不能按"行的先后顺序"分配 —— 一帧里如果有行是从别处切出来的碎片,
    // 它排前面时会先抢走主句的槽位, 主句反而落空、被当成新的一段重复输出。
    std::vector<int> assign(items.size(), -1);
    std::vector<char> taken(slots_.size(), 0);
    struct Cand { double ov; size_t item; size_t slot; };
    std::vector<Cand> cands;
    for (size_t i = 0; i < items.size(); ++i) {
        if (normed[i].empty()) continue;
        for (size_t k = 0; k < slots_.size(); ++k) {
            const Slot &s = slots_[k];
            if (!s.alive) continue;
            if (ts - s.last_seen > gap_) continue;                  // 太久没出现 → 不是同一个槽位
            const double ov = pos_overlap(items[i], s.last);
            if (ov > 0.0) cands.push_back({ov, i, k});
        }
    }
    std::sort(cands.begin(), cands.end(),
              [](const Cand &a, const Cand &b) { return a.ov > b.ov; });
    for (const auto &c : cands) {
        if (assign[c.item] >= 0 || taken[c.slot]) continue;
        assign[c.item] = (int)c.slot;
        taken[c.slot] = 1;
    }

    // ---- 2. 归入槽位, 或为没匹配上的行新建槽位 ----
    for (size_t i = 0; i < items.size(); ++i) {
        if (normed[i].empty()) continue;
        const size_t nlen = normed[i].size();
        const size_t rlen = items[i].text.size();

        if (assign[i] < 0) {
            Slot s;
            s.alive = true;
            s.last = items[i];
            s.last_norm = normed[i];
            s.last_seen = ts;
            s.has_seg = true;
            s.best = items[i];
            s.best_norm = normed[i];
            s.best_len = nlen;
            s.best_raw = rlen;
            s.best_frame = frame;
            s.best_time = ts;
            s.changed_at = ts;
            slots_.push_back(s);
            continue;
        }

        Slot &s = slots_[assign[i]];
        const double prev_seen = s.last_seen;        // 该位置上一次被命中的时刻
        s.last = items[i];
        s.last_seen = ts;

        if (s.has_seg && same_segment(normed[i], s.best_norm, c_.similarity)) {
            // (1) 同一段的延续: 只更新更完整的版本, 不输出(抑制中间态)
            if (more_complete(items[i], nlen, rlen, s.best, s.best_len, s.best_raw)) {
                s.best = items[i];
                s.best_norm = normed[i];
                s.best_len = nlen;
                s.best_raw = rlen;
                s.best_frame = frame;
                s.best_time = ts;
                s.changed_at = ts;
            }
            s.last_norm = normed[i];
            ++suppressed_;
        } else if (!s.has_seg && s.has_emitted &&
                   ts - prev_seen <= reset_ &&          // 中间隔了太多采样点 → 是重新出现, 不算重复
                   same_segment(normed[i], s.emitted_norm, c_.similarity)) {
            // (2) 刚输出过的内容又出现 → 纯粹重复。
            //     判据用"缺了几个采样点"而不是"缺了多久": 抽帧越密, 同样长的空档意味着
            //     越多的采样点缺失。这样 --every-seconds 0.5 下漏检一帧(约 1 秒)仍算重复,
            //     而逐帧下字幕真的消失 1 秒(25 个采样点)就算重新出现。
            ++suppressed_;
        } else {
            // (3) 换句了: 先把上一段交出去, 再开新段。
            //     这里同时清掉 has_emitted —— 否则同一句话隔了一段之后再出现
            //     (例: 说话人 A → B → A) 会被当成"重复"而漏掉。
            if (s.has_seg) emit(out, s);
            s.has_seg = true;
            s.has_emitted = false;
            s.best = items[i];
            s.best_norm = normed[i];
            s.best_len = nlen;
            s.best_raw = rlen;
            s.best_frame = frame;
            s.best_time = ts;
            s.changed_at = ts;
            s.last_norm = normed[i];
        }
    }

    // ---- 3. 内容已稳定足够久 → 输出 ----
    for (auto &s : slots_) {
        if (!s.alive || !s.has_seg) continue;
        if (ts - s.changed_at >= stable_) emit(out, s);
    }

    // ---- 4. 槽位超时(该位置不再出现) → 输出残留段并回收 ----
    for (auto &s : slots_) {
        if (!s.alive) continue;
        if (ts - s.last_seen > gap_) {
            if (s.has_seg) emit(out, s);
            s.alive = false;
        }
    }

    std::sort(out.begin(), out.end(),
              [](const Group &a, const Group &b) { return a.frame < b.frame; });
    return out;
}

inline std::vector<Group> LineDedup::flush() {
    std::vector<Group> out;
    for (auto &s : slots_) {
        if (s.alive && s.has_seg) emit(out, s);
        s.alive = false;
    }
    std::sort(out.begin(), out.end(),
              [](const Group &a, const Group &b) { return a.frame < b.frame; });
    return out;
}

}  // namespace dedup
