#!/bin/bash
# 自己下 pegasus quantize,補上官方 pegasus_quantize.sh 漏掉的 --iterations。
# 沒有 --iterations 時預設只跑 1 個 iteration,dataset.txt 裡的 200 張校正圖只會用到第 1 張。
set -e
DIR=${DIR:-/workspace/acuity_examples_c901149/Models/mnist_cnn}
NAME=${NAME:-mnist_cnn}
cd "$DIR"
ITER=${ITER:-200}
ALGO=${ALGO:-moving_average}

rm -f ${NAME}_uint8.quantize

python3 "$ACUITY_PATH/pegasus.py" quantize \
    --model           ${NAME}.json \
    --model-data      ${NAME}.data \
    --device          CPU \
    --with-input-meta ${NAME}_inputmeta.yml \
    --batch-size      1 \
    --iterations      "$ITER" \
    --algorithm       "$ALGO" \
    --compute-entropy \
    --rebuild \
    --model-quantize  ${NAME}_uint8.quantize \
    --quantizer       asymmetric_affine \
    --qtype           uint8

ls -l ${NAME}_uint8.quantize
