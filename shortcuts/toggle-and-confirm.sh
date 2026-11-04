#!/bin/sh
BTMIX='/Applications/声桥.app/Contents/MacOS/btmixctl'
field() {
    printf '%s' "$1" | /usr/bin/plutil -extract "$2" raw -o - - 2>/dev/null
}
if [ ! -x "$BTMIX" ]; then
    printf '未能切换\nMac 上未找到声桥 1.0.3，请先安装应用。\n'
    exit 0
fi
changed=$("$BTMIX" toggle 2>&1)
changed_code=$?
if [ "$changed_code" -ne 0 ]; then
    message=$(field "$changed" message)
    [ -n "$message" ] || message="$changed"
    printf '切换失败\n%s\n' "$message"
    exit 0
fi
expected=$(field "$changed" requested)
actual=$("$BTMIX" status 2>&1)
actual_code=$?
if [ "$actual_code" -ne 0 ] || [ "$(field "$actual" ok)" != true ]; then
    message=$(field "$actual" message)
    [ -n "$message" ] || message="$actual"
    printf '切换后未能核对状态\n%s\n' "$message"
    exit 0
fi
state=$(field "$actual" state)
receiving=$(field "$actual" receiving)
ready=$(field "$actual" ready)
requested=$(field "$actual" requested)
if [ "$expected" = true ] && [ "$requested" = true ] && [ "$state" = on ] && [ "$receiving" = true ] && [ "$ready" = true ]; then
    printf '音频接收已开启\n已核对：Mac Switch Speaker 已就绪，可连接 Switch 或电脑播放。\n'
elif [ "$expected" = false ] && [ "$requested" = false ] && [ "$state" = off ] && [ "$receiving" = false ] && [ "$ready" = false ]; then
    printf '音频接收已关闭\n已核对：接收已停止，系统蓝牙已恢复到接收前的状态。\n'
else
    printf '状态未确认\n当前状态：%s。可能有另一处开关操作，请稍后查询，勿连续重复切换。\n' "$state"
fi
