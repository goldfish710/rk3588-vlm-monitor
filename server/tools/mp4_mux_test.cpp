// =====================================================
// Mp4Muxer PC 单测工具（宿主 gcc 编译，验证 fMP4 分片 mux）
//
// 用法：
//   1. 生成测试流（h265 annex-B + aac 裸 AU）：
//      ffmpeg -y -f lavfi -i testsrc=size=640x360:rate=15 -f lavfi -i sine=frequency=440:sample_rate=16000 \
//             -t 10 -c:v libx265 -x265-params log-level=error -f h265 -c:a aac -f adts test_av.ts 2>/dev/null
//      # 拆成两个文件：
//      ffmpeg -y -i test_av.ts -c:v copy -an -f h265 test.h265
//      ffmpeg -y -i test_av.ts -vn -c:a copy -f adts test.aac   （ADTS→AU 在工具内处理或直接给 AU）
//   2. 编译：
//      g++ -std=c++11 -O2 -I../../TLServer -I../../TLServer/3rdparty/minimp4 \
//          mp4_mux_test.cpp ../../TLServer/Mp4Muxer.cpp ../../TLServer/minimp4_impl.cpp \
//          -o mp4_mux_test
//   3. 运行：./mp4_mux_test test.h265 test.aac out.mp4
//   4. 验证：ffprobe out.mp4 应为 hevc+aac 双流；ffplay out.mp4 可播
// =====================================================

#include "Mp4Muxer.h"
#include "minimp4.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// 读整个文件
static bool readFile(const std::string& path, std::vector<uint8_t>& out)
{
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) { fprintf(stderr, "打开失败: %s\n", path.c_str()); return false; }
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    out.resize((size_t)n);
    if (n > 0 && fread(out.data(), 1, (size_t)n, fp) != (size_t)n) { fclose(fp); return false; }
    fclose(fp);
    return true;
}

// 把 H.265 按帧切块：每遇到 VCL NAL(类型<=31) 起始码视为新帧开始
// 简化：按起始码切 NAL，VPS/SPS/PPS/SEI 归入下一帧
struct Frame { std::vector<uint8_t> data; bool idr; int64_t pts_ms; };

static std::vector<Frame> splitFrames(const std::vector<uint8_t>& src, int fps)
{
    std::vector<Frame> frames;
    size_t i = 0;
    Frame cur;
    cur.idr = false;
    int frame_idx = 0;

    while (i + 3 <= src.size()) {
        size_t sc_start = i;
        int sc_len = 0;
        if (i + 4 <= src.size() && src[i] == 0 && src[i+1] == 0 && src[i+2] == 0 && src[i+3] == 1) sc_len = 4;
        else if (src[i] == 0 && src[i+1] == 0 && src[i+2] == 1) sc_len = 3;
        if (sc_len == 0) { i++; continue; }

        size_t hdr = i + sc_len;
        int nal_type = (src[hdr] >> 1) & 0x3F;

        // VCL NAL 且当前帧已有数据 → 上一帧结束
        if (nal_type <= 31 && !cur.data.empty()) {
            cur.pts_ms = (int64_t)frames.size() * 1000 / fps;
            frames.push_back(cur);
            cur = Frame();
            cur.idr = false;
            frame_idx++;
        }

        // 找下一起始码
        size_t next = src.size();
        for (size_t j = hdr; j + 3 <= src.size(); j++) {
            if (j + 4 <= src.size() && src[j] == 0 && src[j+1] == 0 && src[j+2] == 0 && src[j+3] == 1) { next = j; break; }
            if (src[j] == 0 && src[j+1] == 0 && src[j+2] == 1) { next = j; break; }
        }
        cur.data.insert(cur.data.end(), src.begin() + sc_start, src.begin() + next);
        // IRAP 判断（16~23）
        if (nal_type >= 16 && nal_type <= 23) cur.idr = true;
        i = next;
    }
    if (!cur.data.empty()) {
        cur.pts_ms = (int64_t)frames.size() * 1000 / fps;
        frames.push_back(cur);
    }
    return frames;
}

// 解析 ADTS 头（7 字节），输出裸 AU 序列
static std::vector<std::vector<uint8_t>> splitAdts(const std::vector<uint8_t>& src, int* out_rate)
{
    std::vector<std::vector<uint8_t>> aus;
    size_t i = 0;
    *out_rate = 16000;
    while (i + 7 <= src.size()) {
        if (src[i] != 0xFF || (src[i+1] & 0xF6) != 0xF0) { i++; continue; }  // syncword
        size_t frame_len = ((src[i+3] & 0x03) << 11) | (src[i+4] << 3) | ((src[i+5] >> 5) & 0x07);
        if (frame_len < 7 || i + frame_len > src.size()) break;
        aus.push_back(std::vector<uint8_t>(src.begin() + i + 7, src.begin() + i + frame_len));
        i += frame_len;
    }
    return aus;
}

int main(int argc, char** argv)
{
    if (argc != 4) {
        printf("用法: %s <test.h265> <test.aac(adts)> <out.mp4>\n", argv[0]);
        return 1;
    }
    std::vector<uint8_t> h265, aac;
    if (!readFile(argv[1], h265) || !readFile(argv[2], aac)) return 1;

    int aac_rate = 16000;
    std::vector<std::vector<uint8_t>> aus = splitAdts(aac, &aac_rate);
    std::vector<Frame> frames = splitFrames(h265, 15);
    printf("视频帧=%zu 音频AU=%zu\n", frames.size(), aus.size());

    // 从第一帧提取 VPS/SPS/PPS（简化：按 NAL 类型扫描）
    std::vector<uint8_t> vps, sps, pps;
    for (auto& f : frames) {
        size_t i = 0;
        while (i + 3 <= f.data.size()) {
            int sc = (i + 4 <= f.data.size() && f.data[i] == 0 && f.data[i+1] == 0 && f.data[i+2] == 0 && f.data[i+3] == 1) ? 4 :
                     ((f.data[i] == 0 && f.data[i+1] == 0 && f.data[i+2] == 1) ? 3 : 0);
            if (!sc) { i++; continue; }
            int t = (f.data[i+sc] >> 1) & 0x3F;
            size_t j = i + sc;
            while (j + 3 <= f.data.size()) {
                if (j + 4 <= f.data.size() && f.data[j] == 0 && f.data[j+1] == 0 && f.data[j+2] == 0 && f.data[j+3] == 1) break;
                if (f.data[j] == 0 && f.data[j+1] == 0 && f.data[j+2] == 1) break;
                j++;
            }
            std::vector<uint8_t>* dst = (t == 32) ? &vps : (t == 33) ? &sps : (t == 34) ? &pps : nullptr;
            if (dst) dst->insert(dst->end(), f.data.begin() + i + sc, f.data.begin() + j);
            i = j;
        }
        if (!vps.empty() && !sps.empty() && !pps.empty()) break;
    }
    printf("VPS=%zuB SPS=%zuB PPS=%zuB\n", vps.size(), sps.size(), pps.size());

    // 16kHz mono AAC-LC 的 ASC：AOT=2(LC), freqIdx=8(16000), chanCfg=1
    // → 00010 1000 0001 补齐 = 0x14 0x08
    // 注意：真实系统里 ASC 由 fdk-aac confBuf 提供，此处测试用标准值
    std::vector<uint8_t> asc = {0x14, 0x08};

    Mp4Muxer mux;
    Mp4Muxer::Config cfg;
    cfg.width = 640; cfg.height = 360;
    cfg.vps = vps; cfg.sps = sps; cfg.pps = pps;
    cfg.asc = asc;
    cfg.audio_enable = !aus.empty();

    std::string err;
    if (!mux.open(argv[3], cfg, &err)) {
        fprintf(stderr, "mux open 失败: %s\n", err.c_str());
        return 1;
    }

    // 按时间戳交错写入（视频 15fps，音频 15.6 帧/s）
    size_t vi = 0, ai = 0;
    int64_t video_ms = 0, audio_ms = 0;
    while (vi < frames.size() || ai < aus.size()) {
        if (ai >= aus.size() || (vi < frames.size() && frames[vi].pts_ms <= audio_ms)) {
            mux.writeVideo(frames[vi].data.data(), frames[vi].data.size(),
                           frames[vi].idr, frames[vi].pts_ms * 90);
            video_ms = frames[vi].pts_ms;
            vi++;
        } else {
            mux.writeAudio(aus[ai].data(), aus[ai].size());
            audio_ms += 64;
            ai++;
        }
    }

    mux.close();
    printf("完成: %s\n", argv[3]);
    printf("验证: ffprobe %s 应为 hevc+aac 双流; ffplay %s 可播\n", argv[3], argv[3]);
    return 0;
}
