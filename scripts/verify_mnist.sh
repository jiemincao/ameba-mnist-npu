#!/bin/bash
# 跑滿 200 張校正圖的 float / uint8 推論,官方 pegasus_inference.sh 一樣寫死 --iterations 1。
set -e
DIR=${DIR:-/workspace/acuity_examples_c901149/Models/mnist_cnn}
NAME=${NAME:-mnist_cnn}
cd "$DIR"
ITER=${ITER:-200}

rm -rf inf
for D in float uint8; do
    if [ "$D" = "float" ]; then
        EXTRA="--dtype float32"
    else
        EXTRA="--dtype quantized --model-quantize ${NAME}_uint8.quantize"
    fi
    echo "=================== inference: $D ==================="
    python3 "$ACUITY_PATH/pegasus.py" inference \
        --model             ${NAME}.json \
        --model-data        ${NAME}.data \
        $EXTRA \
        --iterations        "$ITER" \
        --batch-size        1 \
        --device            CPU \
        --output-dir        "./inf/$D" \
        --postprocess-file  ${NAME}_postprocess_file.yml \
        --with-input-meta   ${NAME}_inputmeta.yml >"/tmp/inf_$D.log" 2>&1 \
        || { echo "FAILED"; tail -30 "/tmp/inf_$D.log"; exit 1; }
    grep -c "Error(0)" "/tmp/inf_$D.log" >/dev/null && echo "$D done"
done

echo "=================== dumped files ==================="
find ./inf -type f | head -8
find ./inf -type f | wc -l
