# -*- coding: utf-8 -*-
"""產生 Pico 2 跑分要用的 C 標頭。

跟 AMB82 那邊的 gen_bench_data.py 是姊妹腳本:同一個 .h5、同一張測試圖,
只是這邊走 TFLite int8,那邊走 acuity。這樣兩塊板子比的才是硬體,不是模型。

輸出:
  mnist_bench_input.h  —— 測試圖(int8 NHWC)、正解、PC float 機率、TFLite int8 期望輸出
  mnist_model_int8.h   —— int8 權重 + int32 bias + per-channel 重量化參數
  mnist_model_float.h  —— float32 權重(給「純軟體 naive」那組對照用)

量化數學的來源:TFLite 的 QuantizeMultiplier / PreprocessSoftmaxScaling。
CMSIS-NN 用的是同一套約定(mult 是 Q0.31,shift 是 2 的次方,正數代表左移),
所以照抄 TFLite 的公式就對得起來。
"""
import math
import os
import numpy as np
import tensorflow as tf

OUT = "/workspace/pico_out"
os.makedirs(OUT, exist_ok=True)
TFL = "/workspace/mnist_cnn_int8.tflite"
H5 = "/workspace/acuity_examples_c901149/Models/mnist_cnn/mnist_cnn.h5"
if not os.path.exists(H5):
    H5 = "/workspace/mnist_cnn.h5"
CH, NCLS = 3, 10


# ---------------- 量化參數換算(照 TFLite 的做法) ----------------

def quantize_multiplier(d):
    """實數乘數 d -> (Q0.31 乘數, 2 的次方 shift)。d = mult * 2^(shift-31)"""
    if d == 0.0:
        return 0, 0
    frac, exp = math.frexp(d)          # d = frac * 2^exp, 0.5 <= frac < 1
    q = int(round(frac * (1 << 31)))
    if q == (1 << 31):                 # frac 進位到 1.0 的邊界情況
        q //= 2
        exp += 1
    assert q <= (1 << 31) - 1
    return q, exp


def softmax_params(input_scale):
    """TFLite PreprocessSoftmaxScaling(beta=1) + CalculateInputRadius"""
    scaled_diff_int_bits = 5
    total_signed_bits = 31
    real = min(1.0 * input_scale * (1 << (31 - scaled_diff_int_bits)),
               (1 << 31) - 1.0)
    mult, shift = quantize_multiplier(real)
    assert shift >= 0, "softmax shift should be >= 0"
    max_input_rescaled = (1.0 * ((1 << scaled_diff_int_bits) - 1) *
                          (1 << (total_signed_bits - scaled_diff_int_bits)) /
                          (1 << shift))
    diff_min = -int(math.floor(max_input_rescaled))
    return mult, shift, diff_min


# ---------------- 讀 TFLite,把 tensor 依名字撿出來 ----------------

tfl = open(TFL, "rb").read()
it = tf.lite.Interpreter(model_content=tfl)
it.allocate_tensors()
det = {d["name"]: d for d in it.get_tensor_details()}


# TFLite 會把被融合掉的 op 串成一個名字,例如
#   sequential/conv2d/Relu;sequential/conv2d/BiasAdd;sequential/conv2d/Conv2D;...
# 所以「子字串」比對會同時打到權重 tensor 和 activation tensor。要用精確或前綴比對。

def pick(name):
    d = det.get(name)
    assert d is not None, "no tensor named %s" % name
    return d


def pick_prefix(prefix):
    hit = [d for n, d in det.items() if n.startswith(prefix)]
    assert len(hit) == 1, "%s matched %d tensors" % (prefix, len(hit))
    return hit[0]


def qp(d):
    q = d["quantization_parameters"]
    return np.array(q["scales"], dtype=np.float64), np.array(q["zero_points"])


t_in = it.get_input_details()[0]
t_out = it.get_output_details()[0]
S = "sequential/"
w_c1, b_c1 = pick(S + "conv2d/Conv2D"), pick(S + "conv2d/BiasAdd/ReadVariableOp")
w_c2, b_c2 = pick(S + "conv2d_1/Conv2D"), pick(S + "conv2d_1/BiasAdd/ReadVariableOp")
w_d1, b_d1 = pick(S + "dense/MatMul"), pick(S + "dense/BiasAdd/ReadVariableOp")
w_d2, b_d2 = pick(S + "dense_1/MatMul"), pick(S + "dense_1/BiasAdd/ReadVariableOp")
a_c1 = pick_prefix(S + "conv2d/Relu;")      # conv1 輸出(relu 已融合進 conv)
a_p1 = pick(S + "max_pooling2d/MaxPool")
a_c2 = pick_prefix(S + "conv2d_1/Relu;")
a_p2 = pick(S + "max_pooling2d_1/MaxPool")
a_d1 = pick_prefix(S + "dense/MatMul;")     # dense1 輸出
a_d2 = pick_prefix(S + "dense_1/MatMul;")   # logits

s_in, z_in = qp(t_in)
s_c1w, _ = qp(w_c1)
s_c1o, z_c1o = qp(a_c1)
s_c2w, _ = qp(w_c2)
s_c2o, z_c2o = qp(a_c2)
s_p2, z_p2 = qp(a_p2)
s_d1w, _ = qp(w_d1)
s_d1o, z_d1o = qp(a_d1)
s_d2w, _ = qp(w_d2)
s_d2o, z_d2o = qp(a_d2)

# maxpool 不改 scale,拿來驗證一下我們的理解沒錯
assert np.allclose(qp(a_p1)[0], s_c1o), "pool1 should not change scale"
assert np.allclose(s_p2, s_c2o), "pool2 should not change scale"

# ---------------- 測試圖:跟 AMB82 用同一張 ----------------
(xtr, _), (xte, yte) = tf.keras.datasets.mnist.load_data()
idx = int(np.argmax(yte == 7))
img = xte[idx]
label = int(yte[idx])

# TFLite 輸入是 NHWC interleaved(AMB82 是 planar,因為那邊 preproc node 要求)。
# 灰階三通道相同,所以是同一份影像資料,只有記憶體順序不同。
# int8 = round(pixel/255 / scale) + zp,而 scale 剛好 = 1/255 -> 等於 pixel - 128
x_int8 = np.repeat(img[:, :, None], CH, axis=2).astype(np.int32) + int(z_in[0])
assert x_int8.min() >= -128 and x_int8.max() <= 127
x_int8 = x_int8.astype(np.int8)

# TFLite 直譯器的 int8 答案 —— 裝置端應該逐 byte 相同,這是最強的驗證
it.set_tensor(t_in["index"], x_int8[None, ...])
it.invoke()
q_out = it.get_tensor(t_out["index"])[0].astype(np.int32)
s_o, z_o = qp(t_out)
prob_tfl = (q_out - z_o[0]) * s_o[0]
print("TFLite int8 raw =", list(q_out))
print("TFLite int8 top1 =", int(np.argmax(q_out)))

# PC 端 float 模型(跟 AMB82 對照用的那份)
m = tf.keras.models.load_model(H5)
xf = np.repeat(img[None, ..., None], CH, axis=-1).astype("float32") / 255.0
prob_f = m.predict(xf, verbose=0)[0]
print("float top1 =", int(np.argmax(prob_f)), " int8 prob =",
      " ".join("%.3f" % p for p in prob_tfl))


# ---------------- 產生 C ----------------

def carr(ctype, name, data, per_line=16, fmt="%d"):
    a = np.asarray(data).reshape(-1)
    out = ["static const %s %s[%d] = {" % (ctype, name, a.size)]
    for i in range(0, a.size, per_line):
        out.append("    " + " ".join((fmt + ",") % v for v in a[i:i + per_line]))
    out.append("};")
    return "\n".join(out)


def emit_layer_quant(f, tag, in_s, w_s, out_s, nch):
    """per-channel(或 per-tensor)重量化參數:mult / shift"""
    mults, shifts = [], []
    for c in range(nch):
        ws = w_s[c] if w_s.size > 1 else w_s[0]
        mm, ss = quantize_multiplier(float(in_s * ws / out_s))
        mults.append(mm)
        shifts.append(ss)
    f.write(carr("int32_t", "%s_mult" % tag, mults, 8) + "\n")
    f.write(carr("int32_t", "%s_shift" % tag, shifts, 8) + "\n\n")


w1 = it.get_tensor(w_c1["index"]).astype(np.int32)   # [16,3,3,3]
w2 = it.get_tensor(w_c2["index"]).astype(np.int32)   # [32,3,3,16]
d1 = it.get_tensor(w_d1["index"]).astype(np.int32)   # [64,1568]
d2 = it.get_tensor(w_d2["index"]).astype(np.int32)   # [10,64]

sm_mult, sm_shift, sm_diffmin = softmax_params(float(s_d2o[0]))
print("softmax: mult=%d shift=%d diff_min=%d" % (sm_mult, sm_shift, sm_diffmin))

with open(os.path.join(OUT, "mnist_bench_input.h"), "w") as f:
    f.write("// 自動產生,不要手改。來源:gen_pico_data.py\n")
    f.write("// MNIST test[%d],正解 = %d。跟 AMB82 的 mnist_bench_data.h 是同一張圖。\n\n"
            % (idx, label))
    f.write("#define BENCH_LABEL      %d\n" % label)
    f.write("#define BENCH_IN_W       28\n")
    f.write("#define BENCH_IN_H       28\n")
    f.write("#define BENCH_IN_C       %d\n" % CH)
    f.write("#define BENCH_NUM_CLASS  %d\n\n" % NCLS)
    f.write("// 28x28x3 NHWC interleaved int8 = pixel - 128\n")
    f.write(carr("int8_t", "g_input_int8", x_int8) + "\n\n")
    f.write("// TFLite 直譯器算出來的 int8 原始輸出 —— 裝置端應該一模一樣\n")
    f.write(carr("int8_t", "g_expect_q", q_out, 10) + "\n\n")
    f.write("// PC 端 float 模型的機率(千分比),跟 AMB82 報的是同一組\n")
    f.write(carr("int", "g_ref_permille",
                 [int(round(p * 1000)) for p in prob_f], 10) + "\n")

with open(os.path.join(OUT, "mnist_model_int8.h"), "w") as f:
    f.write("// 自動產生,不要手改。來源:gen_pico_data.py(TFLite 全整數 int8 量化)\n\n")
    f.write("#include <stdint.h>\n\n")
    f.write("// 各層 activation 的 zero point。CMSIS-NN 的 input_offset = -zp\n")
    f.write("#define ZP_INPUT   (%d)\n" % z_in[0])
    f.write("#define ZP_CONV1   (%d)\n" % z_c1o[0])
    f.write("#define ZP_CONV2   (%d)\n" % z_c2o[0])
    f.write("#define ZP_DENSE1  (%d)\n" % z_d1o[0])
    f.write("#define ZP_LOGIT   (%d)\n\n" % z_d2o[0])
    f.write("// softmax(TFLite PreprocessSoftmaxScaling, beta=1)\n")
    f.write("// 輸出固定 scale 1/256、zp -128,所以機率 = (q + 128) / 256\n")
    f.write("#define SM_MULT      (%d)\n" % sm_mult)
    f.write("#define SM_SHIFT     (%d)\n" % sm_shift)
    f.write("#define SM_DIFF_MIN  (%d)\n\n" % sm_diffmin)
    for tag, ws, wt, bt, ins, outs, nch in [
        ("conv1", s_c1w, w1, b_c1, s_in[0], s_c1o[0], 16),
        ("conv2", s_c2w, w2, b_c2, s_c1o[0], s_c2o[0], 32),
        ("dense1", s_d1w, d1, b_d1, s_p2[0], s_d1o[0], 64),
        ("dense2", s_d2w, d2, b_d2, s_d1o[0], s_d2o[0], 10),
    ]:
        f.write("// ---- %s ----\n" % tag)
        f.write(carr("int8_t", "%s_w" % tag, wt) + "\n")
        f.write(carr("int32_t", "%s_b" % tag,
                     it.get_tensor(bt["index"]).astype(np.int32), 8) + "\n")
        emit_layer_quant(f, tag, ins, ws, outs, nch)

with open(os.path.join(OUT, "mnist_model_float.h"), "w") as f:
    f.write("// 自動產生,不要手改。來源:gen_pico_data.py(原始 float32 權重)\n")
    f.write("// 只給 BENCH_MODE_FLOAT 用:重現完全沒優化的純軟體那個數字。\n\n")
    for l in m.layers:
        w = l.get_weights()
        if not w:
            continue
        f.write("// %s %s\n" % (l.__class__.__name__, l.name))
        f.write(carr("float", "f_%s_w" % l.name, w[0], 8, "%.9gf") + "\n")
        f.write(carr("float", "f_%s_b" % l.name, w[1], 8, "%.9gf") + "\n\n")

for fn in sorted(os.listdir(OUT)):
    print("wrote", fn, os.path.getsize(os.path.join(OUT, fn)), "bytes")
