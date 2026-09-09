# 信 B：線上轉檔器的兩個 bug（技術回報）

**這封是信 A 之後才寄的第二封。** 兩件事分開，權限問題才不會被技術內容淹掉。
->  信 A 見 [realtek-email-A-access.md](realtek-email-A-access.md)

寄之前要先做完：
1. 在 Colab 跑 notebook 第 12 節，拿到 `mnist_gray96.zip` / `mnist_gray28.zip` + 兩份 calib zip
2. 把這兩組上傳到線上轉檔器（CNN-GRAY / UINT8 / scale 0.00392156），存下失敗的 log
3. （可選但很加分）上傳 `mnist_amb82_mbv2_pt.zip`，看 PyTorch ONNX opset 11 過不過

送出前要填的 `[[...]]` 共 3 處。
建議另開新信（不要接在信 A 後面），主旨如下。

附件：
- `cnn-rgb_import.txt`（CNN-RGB 失敗 log，在 `conversion-logs-cnn-rgb-mbv2-h5/`）
- CNN-GRAY 的失敗 log（跑完上面第 2 步才有）
- `mnist_gray96.zip` + `calib_gray96.zip`
- `mnist_gray28.zip` + `calib_gray28.zip`
- `mnist_amb82_mbv2_pt.zip`（PyTorch ONNX opset 11）

---

Subject: AMB82-MINI — two reproducible failures in the online AI Model Conversion service (logs + minimal models attached)

Hi AmebaAIoT Team,

This is a separate, technical follow-up to my offline toolkit access request — no
action needed on access here.

I have been trying to convert a custom model for AMB82-MINI through the online AI
Model Conversion service, and I would like to report two distinct, reproducible
failures. Both come with the service's own logs and with minimal test models
attached, so they should be quick to reproduce on your side. I hope the detail is
useful.

---

## Issue 1 — CNN-RGB: acuity import fails during shape inference on a dynamic Reshape

**What I uploaded:** a MobileNetV2-based classifier (10 classes, MNIST digits),
input 96x96x3, exported from Keras as `.h5`.
Settings: Model `CNN-RGB`, Quantize Type `UINT8`, reverse_channel `false`,
scale `0.00392156`.

**Result:** conversion fails. From `cnn-rgb_import.txt`:

```
=========== Converting cnn_rgb ONNX model ===========
python3 .../pegasus.py import onnx --model cnn_rgb.onnx \
        --output-model cnn_rgb.json --output-data cnn_rgb.data
I Current ONNX Model use ir_version 7 opset_version 18
...
File ".../acuitylib/onnx_ir/onnx_numpy_backend/shape_inference.py", line 65, in infer_shape
File ".../acuitylib/onnx_ir/onnx_numpy_backend/smart_graph_engine.py", line 70, in smart_onnx_scanner
File ".../acuitylib/onnx_ir/onnx_numpy_backend/smart_node.py", line 48, in calc_and_assign_smart_info
File ".../acuitylib/onnx_ir/onnx_numpy_backend/smart_toolkit.py", line 1018, in reshape_shape
    reshaped = np.reshape(np.ones(in_shape.shape), new_shape)
ValueError: cannot reshape array of size 1 into shape (1,1280)
 import model ERROR !
```

The subsequent `quantize` and `export` failures
(`FileNotFoundError: 'cnn_rgb.json'`, `Can not find cnn_rgb_uint8.quantize`)
are just cascading from this — `import` never produced its outputs.

**My analysis:** `1280` is MobileNetV2's feature dimension. The failing node is
the flatten/reshape that follows global average pooling. In the ONNX graph its
target shape is computed dynamically (`Shape -> Gather -> Concat -> Reshape`), and
acuity 5.21.1's shape inference cannot constant-fold it — `in_shape` arrives with
size 1, so reshaping to `(1,1280)` raises. In other words this looks like a
limitation on **dynamic-shape Reshape nodes**, not something specific to my weights.

**A related observation:** although I uploaded a `.h5`, the import log shows the
service runs the **ONNX** importer at `opset_version 18`. So the h5 path is
internally `h5 -> tf2onnx (opset 18) -> acuity import onnx`. That intermediate
conversion is not visible or controllable from the upload form, and opset 18 seems
newer than what acuity 5.21.1 targets. This may be worth documenting, since it
affects which Keras patterns can survive the round trip.

**Questions:**

1. Does the acuity importer support Reshape nodes whose shape input is computed
   at runtime? If not, is there a recommended way to express
   global-average-pooling → flatten so that it imports cleanly?
2. Which ONNX opset does the acuity 5.21.1 importer officially target? Is the
   opset 18 produced by the internal tf2onnx step intended?
3. Is uploading `.onnx` directly (bypassing tf2onnx) the preferred path for
   custom topologies? I have attached `mnist_amb82_mbv2_pt.zip`, the same
   architecture trained in PyTorch and exported with the TorchScript exporter at
   `opset_version=11`, `do_constant_folding=True`, fixed batch size 1, no
   `dynamic_axes`. [[如果你已經上傳過這個,把結果寫在這裡:成功拿到 .nb / 或貼失敗的 _import.txt 摘要]]
4. I noticed the online service runs acuity **5.21.1**, while the offline toolkit
   installation guide ships acuity **6.18.8**. Is the shape-inference limitation in
   Issue 1 already fixed in 6.18.8? If so, that alone would explain why the offline
   toolkit is the recommended path for custom models, and it would be very helpful
   to say so on the online conversion page.

---

## Issue 2 — CNN-GRAY: conversion fails on the `inputmeta.yml` template

**What I uploaded:** a deliberately minimal single-channel CNN
(3 conv layers + global max pooling + dense, 10 classes). No data augmentation
layers, no `Rescaling` layer, no global average pooling, and no dynamic shapes —
specifically so that Issue 1's root cause cannot apply here.

I uploaded **two variants with identical architecture, differing only in input
resolution**, to rule out an input-size restriction:

| File | Input | Result |
|---|---|---|
| `mnist_gray96.zip` | 96x96x1 | fails |
| `mnist_gray28.zip` | 28x28x1 | fails |

Settings for both: Model `CNN-GRAY`, Quantize Type `UINT8`, scale `0.00392156`.
Calibration images are 8-bit single-channel JPEGs (PIL mode `"L"`), 10 images,
one per digit.

**Result:** [[貼 CNN-GRAY 的 _import.txt 關鍵幾行 —— 特別是 inputmeta.yml / preproc_type / YAML parse 的錯誤訊息]]

Both sizes fail in the same way, which suggests the problem is in the CNN-GRAY
code path itself rather than in my models or the input dimensions. I believe I
reported a version of this previously; this is a cleaner, minimal reproduction.

**Questions:**

5. Is the CNN-GRAY path currently expected to work? If it is known-broken, could
   the upload form say so, so users do not spend time on it?
6. The documentation states the input size range for RGB
   ("Pro2 RGB w/h value from maximum to minimum is 1280x704 to 96x96, recommended
   to be multiples of 32"). **What is the corresponding range for CNN-GRAY?**
   I could not find it documented anywhere.

---

## What I am trying to do (context)

I am benchmarking the same MNIST digit classifier on AMB82-MINI versus a
Raspberry Pi Pico 2, to compare NPU inference against a pure-CPU MCU. For that I
need the *same* model on both sides, so I need a custom model conversion to
succeed — the built-in models do not help here.

If the correct answer is "custom topologies are only supported through the offline
toolkit", that is completely fine to hear — it would just have saved me a lot of
time to know it earlier, and it is a good reason to prioritise my pending access
request. A note on the online conversion page about which
frameworks and layer patterns are actually supported would be very helpful for
other users.

Thanks again for the offline toolkit access, and for your time.

Best regards,
[[你的名字]]
Rafael Micro
jiemin.cao@rafaelmicro.com
