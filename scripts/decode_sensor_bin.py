#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
decode_sensor_bin.py  --  解析 AmebaPro2 的 voe_bin/sensor_*.bin

這些檔案不是純資料表,是「編譯好的 sensor driver 模組」:
    0x00..0x1F   檔頭
    0x20..0x4B   位址欄位 + 保留
    0x4C..       .data 區,開頭就是 sensor 暫存器初始化表
    (表結束後)    ARM Thumb-2 機器碼 (在 VOE 協處理器上跑)

用法:
    python scripts/decode_sensor_bin.py <sensor_xxx.bin>
    python scripts/decode_sensor_bin.py <bin> --csv out.csv
"""
import sys, struct, os

MAGIC = bytes([0xEA, 0x07, 0x06, 0x1D])
DATA_OFF  = 0x28          # 兩個位址欄之後,.data 區的起點

# JXF37 (SOI JX-F37) 暫存器註解
#   ok   = 有 datasheet / 開源 driver 佐證,且與本檔案數值相符
#   ?    = 推測,僅供參考
JXF37 = {
    0x00: ("類比增益 gain", "ok"),
    0x01: ("曝光時間 低位元組", "ok"),
    0x02: ("曝光時間 高位元組", "ok"),
    0x0A: ("晶片 ID 高位元組 (0x0F)", "ok"),
    0x0B: ("晶片 ID 低位元組 (0x37)", "ok"),
    0x0D: ("PLL 相關", "?"),
    0x0E: ("PLL / clock", "?"),
    0x0F: ("PLL / clock", "?"),
    0x10: ("PLL / clock", "?"),
    0x11: ("PLL / clock", "?"),
    0x12: ("模式控制 0x40=standby 0x00=串流", "ok"),
    0x13: ("輸出格式", "?"),
    0x1F: ("暫存器更新 / group hold", "?"),
    0x20: ("HTS 每行長度 低位元組", "ok"),
    0x21: ("HTS 每行長度 高位元組", "ok"),
    0x22: ("VTS 每幀行數 低位元組", "ok"),
    0x23: ("VTS 每幀行數 高位元組", "ok"),
    0x24: ("影像寬度 相關", "?"),
    0x25: ("影像高度 相關", "?"),
    0x48: ("內部時序", "?"),
}

def u16(b, o):  return struct.unpack_from("<H", b, o)[0]
def u32(b, o):  return struct.unpack_from("<I", b, o)[0]

def parse_header(b):
    if b[:4] != MAGIC:
        raise SystemExit("magic 不符,前 4 bytes = %s" % b[:4].hex())
    ver = b[4:0x18].split(b"\x00")[0].decode("ascii", "replace")
    return {
        "magic":      b[:4].hex(),
        "version":    ver,
        "payload_sz": u32(b, 0x1C),
        "ptr_a":      u32(b, 0x20),
        "ptr_b":      u32(b, 0x24),
    }

def parse_table(b, start=DATA_OFF):
    """
    記錄格式 = (u16 暫存器位址 LE, u16 值 LE),一路讀到不像為止。
    位址寬度隨 sensor 不同:F37/F35 是 8-bit(0x12),IMX307 是 16-bit(0x3005)。
    值一律是 8-bit。
    """
    off = start
    while off + 4 <= len(b) and u16(b, off) == 0 and u16(b, off + 2) == 0:
        off += 4                              # 跳過表前的補零
    tbl_start, regs = off, []
    while off + 4 <= len(b):
        a, v = u16(b, off), u16(b, off + 2)
        if v > 0xFF:                          # 值必須是 8-bit,否則已經不是表了
            break
        regs.append((off, a, v))
        off += 4
    return tbl_start, regs, off

def find_code(b, after):
    """往後找第一個看起來像 Thumb 函式開頭的位置(push {..,lr} 或 ldr r?,[pc,#..])"""
    for off in range(after, min(after + 0x100, len(b) - 1), 2):
        w = u16(b, off)
        if (w & 0xFE00) == 0xB400 and (w & 0x0100):      # push {..., lr}
            return off
        if (w & 0xF800) == 0x4800:                       # ldr rX, [pc, #imm]
            return off
    return None

def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    path = sys.argv[1]
    csv  = None
    if "--csv" in sys.argv:
        csv = sys.argv[sys.argv.index("--csv") + 1]
    b = open(path, "rb").read()
    h = parse_header(b)
    name = os.path.basename(path)
    is_f37 = "f37" in name.lower()

    print("=" * 72)
    print("檔案        : %s   (%d bytes)" % (name, len(b)))
    print("magic       : %s" % h["magic"])
    print("版本字串    : %s" % h["version"])
    print("payload 長度: 0x%X (%d)   +檔頭 0x20 = %d  %s"
          % (h["payload_sz"], h["payload_sz"], h["payload_sz"] + 0x20,
             "✓ 對得上檔案大小" if h["payload_sz"] + 0x20 == len(b) else "✗ 對不上"))
    print("0x20 位址欄 : 0x%08X" % h["ptr_a"])
    print("0x24 位址欄 : 0x%08X" % h["ptr_b"])
    print("            (這兩個指向模組內部,載入基底 0x70000000;確切用途未證實)")
    print("=" * 72)

    tbl_start, regs, end = parse_table(b)
    code = find_code(b, end)
    awide = max((a for _, a, _ in regs), default=0)
    print("暫存器表    : 0x%X .. 0x%X   共 %d 筆   位址寬度 %s"
          % (tbl_start, end, len(regs), "16-bit" if awide > 0xFF else "8-bit"))
    print("表後資料    : 0x%X .. %s" % (end, ("0x%X" % code) if code else "?"))
    if code:
        print("Thumb 程式碼: 0x%X 起 (載入後位址 0x%08X)" % (code, 0x70000000 + code))
    print("-" * 72)
    print("%-5s %-8s %-6s %-6s  %s" % ("#", "檔案位移", "暫存器", "值", "說明"))
    print("-" * 72)
    rows = []
    for i, (off, a, v) in enumerate(regs):
        note = ""
        if is_f37 and a in JXF37:
            txt, conf = JXF37[a]
            note = txt if conf == "ok" else txt + "  (推測)"
        afmt = "0x%04X" % a if awide > 0xFF else "0x%02X" % a
        print("%-5d 0x%04X   %-6s  0x%02X    %s" % (i, off, afmt, v, note))
        rows.append((i, off, a, v, note))

    # 把幾個成對的 16-bit 欄位算出來
    d = {a: v for _, a, v in regs}
    print("-" * 72)
    if 0x22 in d and 0x23 in d:
        vts = (d[0x23] << 8) | d[0x22]
        print("VTS (每幀行數) = 0x%02X%02X = %d" % (d[0x23], d[0x22], vts))
    if 0x20 in d and 0x21 in d:
        hts = (d[0x21] << 8) | d[0x20]
        print("HTS (每行長度) = 0x%02X%02X = %d" % (d[0x21], d[0x20], hts))
    if 0x12 in d:
        first12 = next((v for _, a, v in regs if a == 0x12), None)
        last12  = [v for _, a, v in regs if a == 0x12][-1]
        print("reg 0x12 首次寫入 = 0x%02X (%s)，最後一次 = 0x%02X (%s)"
              % (first12, "standby" if first12 == 0x40 else "?",
                 last12,  "開始串流" if last12 == 0x00 else "?"))
    print("=" * 72)

    if csv:
        with open(csv, "w", encoding="utf-8") as f:
            f.write("index,file_offset,reg,value,note\n")
            for i, off, a, v, note in rows:
                f.write("%d,0x%04X,0x%02X,0x%02X,%s\n" % (i, off, a, v, note))
        print("CSV 已寫出: %s" % csv)

if __name__ == "__main__":
    main()
