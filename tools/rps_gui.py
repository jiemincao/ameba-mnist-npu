# -*- coding: utf-8 -*-
"""
rps_gui.py -- AMB82-MINI 剪刀石頭布的 PC 端。

板子負責「看」(相機 + NPU 推論),這支程式負責「玩」跟「記錄」。
兩邊靠 USB 序列埠講話,協定寫在 RpsGame/RpsGame.ino 的檔頭。

    python tools/rps_gui.py            # 自動找板子,找到多個會讓你選
    python tools/rps_gui.py COM5       # 直接指定

鍵盤:
    SPACE   開始一局(板子倒數三秒,然後判勝負)
    V       開/關即時預覽
    1 2 3 4 拍一張存成 rock / paper / scissors / none 的訓練資料
    B       連拍開關(用最後按過的 1/2/3 當標籤)
    C       清空本次對戰紀錄
    ESC     離開

為什麼要有 1/2/3:
    現在的模型是拿公開資料集(電腦算圖的手、純白背景)訓練的,
    跟你這顆鏡頭拍到的真實畫面差很多 —— 這叫 domain gap,
    是模型在真實場景失準最常見的原因。
    用這裡拍的照片重新訓練,落差就消失了。
    存檔位置: dataset/rock, dataset/paper, dataset/scissors, dataset/none
"""

import base64
import collections
import io
import os
import queue
import sys
import threading
import time
from pathlib import Path

import pygame
import serial
import serial.tools.list_ports

# ---------------------------------------------------------------- 設定
BAUD = 921600  # 改這裡的話,RpsGame.ino 的 SERIAL_BAUD 也要改成一樣

WIN_W, WIN_H = 980, 690
IMG_W, IMG_H = 480, 360

NAMES = ["ROCK", "PAPER", "SCISSORS"]
VERDICT = ["DRAW", "YOU WIN", "YOU LOSE"]

DATASET = Path(__file__).resolve().parent.parent / "dataset"

# 配色:深底,三個類別各自有顏色,勝負用另外一組
BG = (18, 20, 26)
PANEL = (28, 31, 40)
LINE = (52, 57, 70)
FG = (226, 230, 238)
DIM = (128, 136, 152)
CLS_COL = [(232, 168, 84), (96, 176, 232), (176, 132, 228)]  # rock / paper / scissors

# 收訓練資料用的類別,比 NAMES 多一個 none。
# none = 鏡頭前沒有手(空景、你的臉、牆、手放下去)。
# 沒有這一類的話,softmax 被迫在三個手勢裡硬選一個,永遠說不出「都不是」。
CAP_NAMES = NAMES + ["NONE"]
CAP_COL = CLS_COL + [(130, 140, 158)]
WIN_COL = (104, 204, 140)
LOSE_COL = (232, 100, 108)
DRAW_COL = (160, 168, 184)


# ---------------------------------------------------------------- 序列埠
class Board:
    """在背景執行緒讀序列埠,把解析好的事件丟進 queue。

    板子的 log 裡混著原廠 SDK 關不掉的訊息(Image Classification tick[0] 之類),
    所以只認 "RPS:" 開頭的行,其餘全部丟掉。
    """

    def __init__(self, port):
        self.ser = serial.Serial(port, BAUD, timeout=0.2)
        self.port = port
        self.events = queue.Queue()
        self.alive = True
        self._img_buf = None
        self._img_len = 0
        # 除錯用:不管是不是 RPS: 開頭都留最後幾行,還有各種計數。
        # 板子沒燒新韌體、或序列埠 baud 不對的時候,看這裡最快。
        self.raw = collections.deque(maxlen=14)
        self.n_lines = 0
        self.n_rps = 0
        self.n_img = 0
        self.n_bad = 0
        self._tensor_shape = None
        threading.Thread(target=self._reader, daemon=True).start()

    def send(self, ch):
        try:
            self.ser.write(ch.encode())
        except Exception as e:
            self.events.put(("err", "write failed: %s" % e))

    def close(self):
        self.alive = False
        time.sleep(0.25)
        try:
            self.ser.close()
        except Exception:
            pass

    def _reader(self):
        buf = b""
        while self.alive:
            try:
                chunk = self.ser.read(4096)
            except Exception as e:
                self.events.put(("err", "read failed: %s" % e))
                return
            if not chunk:
                continue
            buf += chunk
            # 原廠 log 有時只吐 \r 不吐 \n,兩行會黏在一起。\r 也當分行符號,
            # 否則我們的 RPS: 行會被前面那半截 log 蓋掉、認不出來。
            buf = buf.replace(b"\r", b"\n")
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                self._line(raw.decode("ascii", "ignore").strip())

    def _line(self, s):
        if not s:
            return
        self.n_lines += 1
        # 影像資料有上百行,全塞進 raw 會把有用的訊息沖掉,所以只留一行代表。
        if not s.startswith("RPS:D "):
            self.raw.append(s[:96])
        if not s.startswith("RPS:"):
            return  # 原廠 log,不理它
        self.n_rps += 1
        body = s[4:]
        head, _, rest = body.partition(" ")

        if head == "D":
            # 影像資料。板子不會在傳圖中間插別的東西,所以直接往後接。
            if self._img_buf is not None:
                try:
                    self._img_buf += base64.b64decode(rest)
                except Exception:
                    self.n_bad += 1
            return

        if head == "TIMG":
            # 模型真正吃到的 tensor。跟 IMG 共用同一個緩衝區,但另外記形狀。
            q = rest.split()
            self._img_buf = bytearray()
            self._img_len = int(q[3]) if len(q) > 3 and q[3].isdigit() else 0
            self._tensor_shape = tuple(int(x) for x in q[:3]) if len(q) > 3 else None
            return

        if head == "TEND":
            if self._img_buf is not None:
                data = bytes(self._img_buf)
                self._img_buf = None
                sh = self._tensor_shape
                self._tensor_shape = None
                self.events.put(("tensor", sh, data))
            return

        if head == "IMG":
            self._img_buf = bytearray()
            self._img_len = int(rest) if rest.isdigit() else 0
            return

        if head == "IMGEND":
            if self._img_buf is not None:
                data = bytes(self._img_buf)
                self._img_buf = None
                # 長度對不上就代表路上掉過 byte,但還是試著顯示 ——
                # JPEG 解碼器吃得下截斷的檔,看到半張圖比整張丟掉有用多了。
                if self._img_len and len(data) != self._img_len:
                    self.events.put(("err", "img %d/%d bytes, %d bad rows"
                                            % (len(data), self._img_len, self.n_bad)))
                if data:
                    self.n_img += 1
                    self.events.put(("img", data))
            return

        if head == "SCORE":
            p = rest.split()
            if len(p) == 5:                     # 四類分數 + top
                self.events.put(("score",
                                 [int(p[0]), int(p[1]), int(p[2]), int(p[3])],
                                 int(p[4])))
            return

        if head == "CNT":
            self.events.put(("cnt", int(rest)))
            return

        if head == "RESULT":
            p = [int(x) for x in rest.split()]
            if len(p) == 7:                     # you me 勝負 + 四類分數
                self.events.put(("result", p[0], p[1], p[2], p[3:7]))
            return

        if head == "READY":
            self.events.put(("ready",))
            return

        if head == "PONG":
            self.events.put(("pong",))
            return

        if head == "ERR":
            self.events.put(("err", rest))
            return


def pick_port(argv):
    """沒指定就自己找。

    AMB82-MINI 板上是 CH340 USB-Serial,描述字串裡會有 "CH340",
    所以先照這個猜;猜不到才列出來問。
    提示訊息一律用英文 —— Windows 主控台預設是 cp950,
    印中文會變亂碼,而 GUI 本體的字也都是英文。
    """
    if len(argv) > 1:
        return argv[1]
    ports = list(serial.tools.list_ports.comports())
    if not ports:
        print("No serial port found. Is the board plugged in?")
        sys.exit(1)

    ch340 = [p for p in ports if "ch340" in (p.description or "").lower()]
    if len(ch340) == 1:
        print("Using %s (%s)" % (ch340[0].device, ch340[0].description))
        return ch340[0].device
    if len(ports) == 1:
        print("Using %s (%s)" % (ports[0].device, ports[0].description))
        return ports[0].device

    print("Multiple serial ports:")
    for i, p in enumerate(ports):
        print("  %d) %-8s %s" % (i, p.device, p.description))
    while True:
        s = input("Pick a number: ").strip()
        if s.isdigit() and 0 <= int(s) < len(ports):
            return ports[int(s)].device


# ---------------------------------------------------------------- 畫面元件
def font(size, bold=False):
    return pygame.font.SysFont("consolas,menlo,dejavusansmono,couriernew", size, bold=bold)


def text(surf, s, x, y, f, col=FG, right=False, center=False):
    img = f.render(s, True, col)
    r = img.get_rect()
    if right:
        r.topright = (x, y)
    elif center:
        r.midtop = (x, y)
    else:
        r.topleft = (x, y)
    surf.blit(img, r)
    return r


def panel(surf, rect, fill=PANEL):
    pygame.draw.rect(surf, fill, rect, border_radius=8)
    pygame.draw.rect(surf, LINE, rect, width=1, border_radius=8)


def button(surf, rect, label, f, mouse, accent=FG, on=False, hint=None, primary=False):
    """畫一顆按鈕,回傳它的 rect(之後拿來做點擊判定)。

    on=True 代表這顆是「開著」的切換鈕(預覽、連拍),用填色表示。
    """
    hot = rect.collidepoint(mouse)
    if on:
        fill, edge, fg = accent, accent, BG
    elif hot:
        fill, edge, fg = (44, 49, 62), accent, accent
    elif primary:
        # 主按鈕就算沒被滑鼠碰到也要看得出來是主角
        fill, edge, fg = PANEL, accent, accent
    else:
        fill, edge, fg = PANEL, LINE, FG
    pygame.draw.rect(surf, fill, rect, border_radius=8)
    pygame.draw.rect(surf, edge, rect, width=1, border_radius=8)
    img = f.render(label, True, fg)
    surf.blit(img, img.get_rect(center=(rect.centerx, rect.centery - (7 if hint else 0))))
    if hint:
        text(surf, hint, rect.centerx, rect.centery + 4, HINT_FONT[0], fg if on else DIM, center=True)
    return rect


HINT_FONT = [None]    # main() 建好字型後填進來


# ---------------------------------------------------------------- 主程式
def main():
    port = pick_port(sys.argv)
    try:
        board = Board(port)
    except Exception as e:
        print("Cannot open %s: %s" % (port, e))
        print("Is the Arduino IDE Serial Monitor still open? Only one program can hold the port.")
        sys.exit(1)

    pygame.init()
    pygame.display.set_caption("AMB82 Rock Paper Scissors  --  %s @ %d" % (port, BAUD))
    screen = pygame.display.set_mode((WIN_W, WIN_H))
    clock = pygame.time.Clock()

    f_big = font(56, True)
    f_mid = font(28, True)
    f_sm = font(18)
    f_tiny = font(15)
    HINT_FONT[0] = font(13)

    # ---- 狀態 ----
    scores = [0, 0, 0, 0]
    live_top = -1
    frame = None  # pygame.Surface,目前顯示的影像
    last_jpeg = None  # 原始 bytes,存檔用
    countdown = None  # 3/2/1/0 或 None
    result = None  # (you, board, verdict, [s0,s1,s2,s3])
    miss = None  # 這一局判不出來的原因,要跟 result 一樣大聲地講出來
    tally = [0, 0, 0]  # 平手 / 你贏 / 你輸
    preview = False
    burst = False
    label = None  # 目前的收集標籤 0/1/2
    pending_save = None  # 下一張圖要存成哪個標籤
    saved = [0, 0, 0, 0]
    status = "connecting..."
    ready = False
    last_img_t = 0.0
    debug = False
    showing_tensor = False

    board.send("P")

    def counts_on_disk():
        for i, n in enumerate(CAP_NAMES):
            d = DATASET / n.lower()
            saved[i] = len(list(d.glob("*.jpg"))) if d.is_dir() else 0

    counts_on_disk()

    def save(data, idx):
        d = DATASET / CAP_NAMES[idx].lower()
        d.mkdir(parents=True, exist_ok=True)
        fn = d / ("%s_%s.jpg" % (CAP_NAMES[idx].lower(), time.strftime("%Y%m%d_%H%M%S_") + ("%03d" % (time.time() % 1 * 1000))))
        fn.write_bytes(data)
        saved[idx] += 1

    def capture(idx):
        nonlocal pending_save, label, preview
        label = idx
        pending_save = idx
        if preview:
            preview = False
            board.send("X")
        board.send("S")

    def act(name):
        """按鈕跟鍵盤走同一條路,行為才不會兩邊不一致。"""
        nonlocal preview, burst, result, tally, debug, status, running
        if name == "tensor":
            board.send("T")
            return
        if name == "play":
            if preview:
                preview = False
                board.send("X")
            burst = False
            result = None
            board.send("G")
        elif name == "preview":
            preview = not preview
            burst = False
            board.send("S" if preview else "X")
        elif name.startswith("cap"):
            burst = False
            capture(int(name[3]))
        elif name == "burst":
            if label is None:
                status = "pick ROCK / PAPER / SCISSORS / NONE first"
            else:
                burst = not burst
                if burst:
                    capture(label)
        elif name == "clear":
            tally = [0, 0, 0]
            result = None
        elif name == "debug":
            debug = not debug
        elif name == "quit":
            running = False

    hits = []    # 每一幀重建:[(rect, 動作名稱)]。pygame 的 Rect 不能當 dict key。

    running = True
    while running:
        # -------------------------------------------------- 事件:板子
        while True:
            try:
                ev = board.events.get_nowait()
            except queue.Empty:
                break
            kind = ev[0]
            if kind == "score":
                scores, live_top = ev[1], ev[2]
            elif kind == "cnt":
                countdown = ev[1]
                if countdown == 3:
                    result = None
                    miss = None
            elif kind == "result":
                result = (ev[1], ev[2], ev[3], ev[4])
                miss = None
                tally[ev[3]] += 1
                countdown = None
            elif kind == "img":
                showing_tensor = False
                last_jpeg = ev[1]
                last_img_t = time.time()
                try:
                    frame = pygame.image.load(io.BytesIO(last_jpeg))
                except Exception as e:
                    status = "bad jpeg: %s" % e
                else:
                    if pending_save is not None:
                        save(last_jpeg, pending_save)
                        pending_save = None
                        if burst and label is not None:
                            capture(label)
                if preview:
                    board.send("S")  # 預覽 = 自己一張接一張要
            elif kind == "tensor":
                sh, data = ev[1], ev[2]
                if not sh:
                    status = "tensor: bad header"
                else:
                    w, h, c = sh
                    need = w * h * c
                    if len(data) < need:
                        status = "tensor short %d/%d" % (len(data), need)
                    else:
                        # planar -> interleaved。SDK 的 img_rgb2gray() 是照
                        # R 整張 / G 整張 / B 整張 在讀的,所以這裡也照那個排法還原。
                        plane = w * h
                        out = bytearray(need)
                        for ch in range(c):
                            base = ch * plane
                            out[ch::c] = data[base:base + plane]
                        surf = pygame.image.frombuffer(bytes(out), (w, h), "RGB")
                        frame = pygame.transform.scale(surf, (IMG_W, IMG_H))
                        showing_tensor = True
                        last_img_t = time.time()
                        status = "model input %dx%dx%d" % (w, h, c)

            elif kind == "ready":
                ready = True
                status = "board ready"
            elif kind == "pong":
                ready = True
                status = "connected"
            elif kind == "err":
                # 板子這一局沒判出來(沒有手 / 信心不足 / 完全沒結果)。
                # 一定要把 countdown 清掉 —— 它是 busy 旗標的來源,
                # 不清的話 START ROUND 會永遠卡在 "PLAYING..."。
                status = ev[1]
                countdown = None
                result = None
                miss = ev[1]

        # ---------------------------------------------- 看門狗:preview / burst
        # preview 和 burst 都是「自餵迴圈」:收到圖才會去要下一張。
        # 中間掉一張(板子漏收指令、或 X 緊接著 S 撞在一起),整條鏈就永遠停住,
        # 從畫面上看就像按鈕沒反應。超時就自己補送一次 S 把鏈接回去。
        now = time.time()
        if (last_img_t and now - last_img_t > 1.5
                and (preview or (burst and label is not None))):
            last_img_t = now
            board.send("S")
            status = "resync"

        # -------------------------------------------------- 事件:鍵盤滑鼠
        KEYMAP = {
            pygame.K_ESCAPE: "quit",
            pygame.K_SPACE: "play",
            pygame.K_v: "preview",
            pygame.K_1: "cap0",
            pygame.K_2: "cap1",
            pygame.K_3: "cap2",
            pygame.K_4: "cap3",
            pygame.K_b: "burst",
            pygame.K_c: "clear",
            pygame.K_d: "debug",
            pygame.K_t: "tensor",
        }
        for e in pygame.event.get():
            if e.type == pygame.QUIT:
                running = False
            elif e.type == pygame.KEYDOWN and e.key in KEYMAP:
                act(KEYMAP[e.key])
            elif e.type == pygame.MOUSEBUTTONDOWN and e.button == 1:
                for r, name in hits:
                    if r.collidepoint(e.pos):
                        act(name)
                        break

        # -------------------------------------------------- 畫面
        mouse = pygame.mouse.get_pos()
        hits = []
        screen.fill(BG)

        # 標題列
        text(screen, "ROCK  PAPER  SCISSORS", 24, 18, f_mid)
        dot = WIN_COL if ready else DIM
        pygame.draw.circle(screen, dot, (WIN_W - 30, 30), 6)
        text(screen, status, WIN_W - 46, 22, f_tiny, DIM, right=True)

        # ---- 左:相機影像 ----
        img_rect = pygame.Rect(24, 64, IMG_W, IMG_H)
        panel(screen, img_rect, (10, 11, 15))
        if frame is not None:
            sc = pygame.transform.smoothscale(frame, (IMG_W - 2, IMG_H - 2))
            screen.blit(sc, (img_rect.x + 1, img_rect.y + 1))
        else:
            text(screen, "no image yet", img_rect.centerx, img_rect.centery - 22, f_sm, DIM, center=True)
            text(screen, "click START ROUND, or PREVIEW for a live view",
                 img_rect.centerx, img_rect.centery + 4, f_tiny, (90, 98, 114), center=True)

        # 倒數蓋在影像上
        if countdown is not None:
            s = pygame.Surface((IMG_W, IMG_H), pygame.SRCALPHA)
            s.fill((0, 0, 0, 150))
            screen.blit(s, img_rect.topleft)
            big = f_big.render("GO!" if countdown == 0 else str(countdown), True, FG)
            screen.blit(big, big.get_rect(center=img_rect.center))

        age = time.time() - last_img_t if last_img_t else 0
        tag = "MODEL INPUT" if showing_tensor else (
            "PREVIEW" if preview else ("BURST %s" % CAP_NAMES[label] if burst and label is not None else "snapshot"))
        text(screen, "%s   %dx%d   %s" % (tag, IMG_W, IMG_H, ("%.1fs ago" % age) if last_img_t else "-"),
             24, img_rect.bottom + 8, f_tiny, DIM)

        # ---- 右:即時機率 ----
        rx, rw = 528, WIN_W - 528 - 24
        bar_top = 64
        text(screen, "LIVE", rx, bar_top, f_sm, DIM)
        for i in range(4):
            y = bar_top + 28 + i * 40
            text(screen, CAP_NAMES[i], rx, y + 2, f_sm, FG if i == live_top else DIM)
            bx, bw = rx + 110, rw - 160
            pygame.draw.rect(screen, (40, 44, 55), (bx, y, bw, 18), border_radius=4)
            w = int(bw * scores[i] / 100.0)
            if w > 0:
                pygame.draw.rect(screen, CAP_COL[i], (bx, y, w, 18), border_radius=4)
            text(screen, "%3d%%" % scores[i], rx + rw, y + 2, f_sm, FG if i == live_top else DIM, right=True)

        # ---- 右:這一局的結果 ----
        res_rect = pygame.Rect(rx, 210, rw, 190)
        panel(screen, res_rect)
        if result is None and miss is not None:
            hint = {"no hand":        "畫面裡沒有手",
                    "low confidence": "看到了,但沒把握",
                    "no result":      "這一局沒收到任何辨識結果"}.get(miss, miss)
            text(screen, "NO CALL", res_rect.centerx, res_rect.y + 16, f_mid,
                 (232, 168, 84), center=True)
            text(screen, miss, res_rect.centerx, res_rect.y + 74, f_sm, FG, center=True)
            text(screen, hint, res_rect.centerx, res_rect.y + 100, f_tiny, DIM, center=True)
            text(screen, "top %s %d%%  (%d/%d/%d/%d)"
                 % (CAP_NAMES[live_top] if live_top >= 0 else "-",
                    scores[live_top] if live_top >= 0 else 0,
                    scores[0], scores[1], scores[2], scores[3]),
                 res_rect.centerx, res_rect.y + 148, f_tiny, (90, 98, 114), center=True)
        elif result is None:
            text(screen, "click START ROUND", res_rect.centerx, res_rect.y + 70, f_sm, DIM, center=True)
            text(screen, "3-2-1, show your hand", res_rect.centerx, res_rect.y + 95, f_tiny, (90, 98, 114), center=True)
        else:
            you, bd, vd, sn = result
            col = [DRAW_COL, WIN_COL, LOSE_COL][vd]
            text(screen, VERDICT[vd], res_rect.centerx, res_rect.y + 16, f_mid, col, center=True)
            text(screen, "YOU", res_rect.x + 24, res_rect.y + 70, f_sm, DIM)
            text(screen, NAMES[you], res_rect.x + 24, res_rect.y + 92, f_mid, CLS_COL[you])
            text(screen, "BOARD", res_rect.right - 24, res_rect.y + 70, f_sm, DIM, right=True)
            text(screen, NAMES[bd], res_rect.right - 24, res_rect.y + 92, f_mid, CLS_COL[bd], right=True)
            conf = sn[you]
            text(screen, "confidence %d%%  (%d/%d/%d/%d)" % (conf, sn[0], sn[1], sn[2], sn[3]),
                 res_rect.centerx, res_rect.y + 148, f_tiny,
                 DIM if conf >= 80 else LOSE_COL, center=True)

        # ---- 右:累計 ----
        text(screen, "SESSION", rx, 418, f_sm, DIM)
        for i, (lab, col) in enumerate(zip(["WIN", "LOSE", "DRAW"], [WIN_COL, LOSE_COL, DRAW_COL])):
            n = tally[[1, 2, 0][i]]
            x = rx + i * (rw // 3)
            text(screen, str(n), x, 442, f_mid, col)
            text(screen, lab, x, 478, f_tiny, DIM)

        # ---- 按鈕 ----
        # 主按鈕放在影像正下方,因為那是你看著的地方。
        busy = countdown is not None
        hits.append((button(screen, pygame.Rect(24, 454, IMG_W, 58),
                    "PLAYING..." if busy else "START ROUND", f_mid, mouse,
                    accent=WIN_COL, hint="SPACE", primary=True), "play"))

        # 收集訓練資料:按一下拍一張存進對應的資料夾
        cap_y, cap_w = 524, (IMG_W - 21) // 4
        for i in range(4):
            r = pygame.Rect(24 + i * (cap_w + 7), cap_y, cap_w, 44)
            hits.append((button(screen, r, CAP_NAMES[i], f_sm, mouse, accent=CAP_COL[i],
                        on=(burst and label == i), hint="save  %d" % saved[i]), "cap%d" % i))

        hits.append((button(screen, pygame.Rect(rx, 512, 208, 44), "PREVIEW", f_sm, mouse,
                    accent=(140, 180, 255), on=preview, hint="V"), "preview"))
        hits.append((button(screen, pygame.Rect(rx + 220, 512, 208, 44), "BURST", f_sm, mouse,
                    accent=(232, 168, 84), on=burst, hint="B"), "burst"))
        hits.append((button(screen, pygame.Rect(rx, 568, 134, 36), "CLEAR", f_tiny, mouse), "clear"))
        hits.append((button(screen, pygame.Rect(rx + 146, 568, 134, 36), "MODEL IN", f_tiny, mouse,
                            hint="T", on=showing_tensor), "tensor"))
        hits.append((button(screen, pygame.Rect(rx + 292, 568, 136, 36), "DEBUG", f_tiny, mouse,
                    on=debug), "debug"))

        # ---- 底:資料集 ----
        pygame.draw.line(screen, LINE, (24, 588), (IMG_W + 24, 588))
        text(screen, "dataset/  " + "  ".join("%s %d" % (CAP_NAMES[i].lower(), saved[i]) for i in range(4)),
             24, 600, f_tiny, DIM)
        text(screen, "collect real photos from this camera, then retrain --",
             24, 624, f_tiny, (90, 98, 114))
        text(screen, "that is what fixes the accuracy",
             24, 646, f_tiny, (90, 98, 114))

        # ---- 除錯面板 (D) ----
        # 序列埠到底有沒有東西進來、進來的是不是我們的協定 —— 看這裡最快。
        #   lines 一直是 0        -> 埠不對,或板子沒在跑
        #   lines 在跳但 RPS: 是 0 -> 板子跑的是舊韌體(或 baud 不對,那會看到亂碼)
        #   RPS: 在跳但 img 是 0   -> 協定通了,只是還沒按 V / SPACE
        if debug:
            dbg = pygame.Rect(24, 64, WIN_W - 48, WIN_H - 88)
            s = pygame.Surface(dbg.size, pygame.SRCALPHA)
            s.fill((8, 9, 12, 242))
            screen.blit(s, dbg.topleft)
            pygame.draw.rect(screen, LINE, dbg, width=1, border_radius=8)
            text(screen, "SERIAL DEBUG   %s @ %d" % (port, BAUD), dbg.x + 16, dbg.y + 12, f_sm, FG)
            text(screen, "lines %d    RPS: %d    images %d    bad b64 rows %d"
                 % (board.n_lines, board.n_rps, board.n_img, board.n_bad),
                 dbg.x + 16, dbg.y + 38, f_tiny, DIM)
            text(screen, "last lines from the board (RPS:D image rows are hidden):",
                 dbg.x + 16, dbg.y + 66, f_tiny, (90, 98, 114))
            for i, ln in enumerate(list(board.raw)):
                text(screen, ln, dbg.x + 16, dbg.y + 90 + i * 20, f_tiny,
                     FG if ln.startswith("RPS:") else DIM)
            text(screen, "click DEBUG again (or press D) to close", dbg.x + 16, dbg.bottom - 26, f_tiny, (90, 98, 114))
            del hits[:]    # 面板蓋住畫面時,底下的按鈕不該還能被點到
            hits.append((button(screen, pygame.Rect(dbg.right - 140, dbg.bottom - 48, 116, 34),
                        "CLOSE", f_tiny, mouse), "debug"))

        pygame.display.flip()
        clock.tick(60)

    board.send("X")
    board.close()
    pygame.quit()


if __name__ == "__main__":
    main()
