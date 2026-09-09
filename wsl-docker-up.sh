#!/bin/sh
# 在 WSL Ubuntu 裡把 Docker daemon 叫起來(不需要 Docker Desktop,不會觸發 Windows UAC)
#
# 用法(從 Windows 這邊):
#   wsl -d Ubuntu -e sh /mnt/d/workdir/ameba/wsl-docker-up.sh
#
# WSL 沒開 systemd(PID 1 是 init(Ubuntu)),所以 systemctl 沒用,要手動跑 dockerd。
# daemon 會跨 wsl session 存活,只有整個 distro 被關掉(wsl --shutdown / 重開機)才需要再跑一次。

if docker info >/dev/null 2>&1; then
    echo "dockerd 已在跑:$(docker --version)"
    exit 0
fi

echo "啟動 dockerd..."
sudo sh -c "nohup dockerd >/var/log/dockerd.log 2>&1 &"

if timeout 45 sh -c 'until docker info >/dev/null 2>&1; do sleep 2; done'; then
    echo "OK:$(docker --version)"
else
    echo "失敗,看 log:"
    tail -30 /var/log/dockerd.log
    exit 1
fi
