#!/usr/bin/env python3
import sys

class BitReader:
    def __init__(self, data):
        self.data = data
        self.bit_pos = 0

    def read_bits(self, n):
        val = 0
        for _ in range(n):
            byte_idx = self.bit_pos // 8
            bit_idx = 7 - (self.bit_pos % 8)
            bit = (self.data[byte_idx] >> bit_idx) & 1
            val = (val << 1) | bit
            self.bit_pos += 1
        return val

    def read_ue(self):
        zeros = 0
        while self.read_bits(1) == 0:
            zeros += 1
        if zeros == 0:
            return 0
        suffix = self.read_bits(zeros)
        return (1 << zeros) - 1 + suffix

    def read_se(self):
        ue = self.read_ue()
        if ue % 2 == 0:
            return ue // 2
        else:
            return -(ue // 2 + 1)

def set_bits(data, bit_start, value, n):
    # data 为 bytearray，bit_start 从 0 开始，MSB 优先
    for i in range(n):
        bit = (value >> (n - 1 - i)) & 1
        byte_idx = bit_start // 8
        bit_idx = 7 - (bit_start % 8)
        mask = 1 << bit_idx
        if bit:
            data[byte_idx] |= mask
        else:
            data[byte_idx] &= ~mask
        bit_start += 1

def parse_sps(sps_bytes):
    # 假设 sps_bytes 包含 NAL 头 (0x67) 但不包含起始码
    br = BitReader(sps_bytes)
    nal_byte = br.read_bits(8)
    assert (nal_byte & 0x1f) == 7, "不是 SPS"
    profile_idc = br.read_bits(8)
    br.read_bits(8)  # constraint flags
    br.read_bits(8)  # level_idc
    br.read_ue()     # sps_id

    # 高级 profile 处理（本 SPS 是 Baseline，跳过）
    if profile_idc in [100,110,122,244,44,83,86,118,128,138,139,134,135]:
        chroma_format_idc = br.read_ue()
        if chroma_format_idc == 3:
            br.read_bits(1)
        br.read_ue()
        br.read_ue()
        br.read_bits(1)
        if br.read_bits(1):  # scaling matrix present
            raise Exception("不支持缩放矩阵")

    br.read_ue()  # log2_max_frame_num_minus4
    pic_order_cnt_type = br.read_ue()
    if pic_order_cnt_type == 0:
        br.read_ue()
    elif pic_order_cnt_type == 1:
        br.read_bits(1)
        br.read_se()
        br.read_se()
        num_ref_frames = br.read_ue()
        for _ in range(num_ref_frames):
            br.read_se()

    br.read_ue()  # max_num_ref_frames
    br.read_bits(1)  # gaps_in_frame_num_value_allowed_flag
    br.read_ue()  # pic_width_in_mbs_minus1
    br.read_ue()  # pic_height_in_map_units_minus1
    frame_mbs_only_flag = br.read_bits(1)
    if not frame_mbs_only_flag:
        br.read_bits(1)
    br.read_bits(1)  # direct_8x8_inference_flag
    frame_cropping_flag = br.read_bits(1)
    if frame_cropping_flag:
        br.read_ue(); br.read_ue(); br.read_ue(); br.read_ue()

    vui_flag = br.read_bits(1)
    if not vui_flag:
        raise Exception("无 VUI")

    # VUI 解析
    aspect_ratio_flag = br.read_bits(1)
    if aspect_ratio_flag:
        aspect_ratio_idc = br.read_bits(8)
        if aspect_ratio_idc == 255:
            br.read_bits(16); br.read_bits(16)
    overscan_flag = br.read_bits(1)
    if overscan_flag:
        br.read_bits(1)
    video_signal_flag = br.read_bits(1)
    if video_signal_flag:
        br.read_bits(3); br.read_bits(1)
        colour_desc_flag = br.read_bits(1)
        if colour_desc_flag:
            br.read_bits(8); br.read_bits(8); br.read_bits(8)
    chroma_loc_flag = br.read_bits(1)
    if chroma_loc_flag:
        br.read_ue(); br.read_ue()
    timing_info_flag = br.read_bits(1)
    if not timing_info_flag:
        raise Exception("无 timing_info")

    timing_bit_start = br.bit_pos
    num_units_in_tick = br.read_bits(32)
    time_scale = br.read_bits(32)
    return timing_bit_start, num_units_in_tick, time_scale

def main():
    # 旧事件文件中的 SPS 裸数据（含 NAL 头 0x67，不含起始码）
    sps_hex = "6742c01f8d8d50501e90800000008000000f1e47844235"
    sps_bytes = bytes.fromhex(sps_hex)

    try:
        timing_start, num_units, time_scale = parse_sps(sps_bytes)
        print(f"原始 timing_info: bit_start={timing_start}, num_units={num_units}, time_scale={time_scale}")

        # 目标：30fps -> num_units=1, time_scale=30
        new_num_units = 1
        new_time_scale = 30

        # 修改字节（直接修改原始数据，假设无防竞争字节 0x03）
        data_mod = bytearray(sps_bytes)
        set_bits(data_mod, timing_start, new_num_units, 32)
        set_bits(data_mod, timing_start + 32, new_time_scale, 32)

        # 添加 4 字节起始码
        start_code = [0x00, 0x00, 0x00, 0x01]
        full_sps = start_code + list(data_mod)

        print("修改后的 SPS 完整字节（含起始码），复制到 C++ 代码中：")
        print(", ".join(f"0x{b:02x}" for b in full_sps))

        # 同时输出 PPS
        pps = [0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x31, 0xb2]
        print("\nPPS 完整字节（含起始码）：")
        print(", ".join(f"0x{b:02x}" for b in pps))

    except Exception as e:
        print("解析失败:", e)

if __name__ == "__main__":
    main()