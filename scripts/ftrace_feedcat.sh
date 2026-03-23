#!/bin/sh
# ftrace_feedcat.sh
#
# 基于 Ftrace 的喂食器驱动性能追踪脚本
# 用法：
#   chmod +x ftrace_feedcat.sh
#   ./ftrace_feedcat.sh i2c        # 追踪 AHT30 I2C 总线传输耗时
#   ./ftrace_feedcat.sh weight     # 追踪 HX711 weight_get 采样耗时
#   ./ftrace_feedcat.sh motor      # 追踪 motor_feed 全流程耗时
#   ./ftrace_feedcat.sh ir         # 追踪红外 ir_obstacle_check 防抖延迟
#   ./ftrace_feedcat.sh all        # 追踪全部驱动函数
#   ./ftrace_feedcat.sh stop       # 停止追踪，还原现场
#
# 依赖：内核需开启 CONFIG_FTRACE, CONFIG_FUNCTION_GRAPH_TRACER

TRACE_ROOT=/sys/kernel/debug/tracing
DURATION=${DURATION:-5}

die() { echo "[ftrace] ERROR: $*"; exit 1; }

check_root() {
    [ "$(id -u)" -eq 0 ] || die "需要 root 权限，请用 sudo 运行"
}

check_ftrace() {
    [ -d "$TRACE_ROOT" ] || die "Ftrace 目录不存在，请确认内核已启用 CONFIG_FTRACE"
    grep -q "function_graph" ${TRACE_ROOT}/available_tracers 2>/dev/null || \
        die "function_graph tracer 不可用"
}

trace_reset() {
    echo 0 > ${TRACE_ROOT}/tracing_on
    echo "" > ${TRACE_ROOT}/set_ftrace_filter 2>/dev/null || true
    echo "nop" > ${TRACE_ROOT}/current_tracer
    echo "" > ${TRACE_ROOT}/trace
    echo "[ftrace] 追踪已停止，现场已还原"
}

trace_run() {
    local FILTER="$1"
    local DESC="$2"

    echo "[ftrace] === ${DESC} ==="
    echo "[ftrace] 过滤函数: ${FILTER}"

    # 重置
    echo 0 > ${TRACE_ROOT}/tracing_on
    echo "" > ${TRACE_ROOT}/trace

    # 设置 function_graph tracer
    echo "function_graph" > ${TRACE_ROOT}/current_tracer
    echo "${FILTER}" > ${TRACE_ROOT}/set_ftrace_filter 2>/dev/null || \
        echo "[ftrace] 警告: 部分函数可能未导出，忽略"

    echo 1 > ${TRACE_ROOT}/tracing_on
    echo "[ftrace] 抓取 ${DURATION} 秒..."
    sleep ${DURATION}
    echo 0 > ${TRACE_ROOT}/tracing_on

    echo "[ftrace] ====== 原始追踪输出 ======"
    cat ${TRACE_ROOT}/trace | grep -v '^#' | head -100
    echo "[ftrace] ============================="

    # 解析耗时统计
    echo "[ftrace] --- 耗时统计 (us) ---"
    for fn in $(echo "$FILTER" | tr ' ' '\n'); do
        local DATA
        DATA=$(cat ${TRACE_ROOT}/trace | grep "${fn}()" | \
            grep -oP '[0-9]+\.[0-9]+\s+us' | grep -oP '[0-9]+\.[0-9]+')
        if [ -z "$DATA" ]; then
            echo "  ${fn}: 无数据（函数未被调用或未导出）"
            continue
        fi
        local COUNT MIN MAX SUM
        COUNT=$(echo "$DATA" | wc -l)
        MIN=$(echo "$DATA" | sort -n | head -1)
        MAX=$(echo "$DATA" | sort -n | tail -1)
        SUM=$(echo "$DATA" | awk '{s+=$1} END{print s}')
        AVG=$(echo "$SUM $COUNT" | awk '{printf "%.2f", $1/$2}')
        printf "  %-30s 调用:%d  min:%.2fus  avg:%sus  max:%.2fus\n" \
               "${fn}" "$COUNT" "$MIN" "$AVG" "$MAX"
    done
}

# ==================== 各追踪模式 ====================

mode_i2c() {
    trace_run "i2c_transfer" "AHT30 I2C 总线传输耗时追踪"
}

mode_weight() {
    trace_run "weight_get" "HX711 weight_get 采样耗时追踪"
}

mode_motor() {
    trace_run "motor_feed" "L298N motor_feed 喂食全流程耗时追踪"
}

mode_ir() {
    trace_run "ir_obstacle_check" "TCRT5000 ir_obstacle_check 防抖延迟追踪"
}

mode_all() {
    trace_run "i2c_transfer weight_get motor_feed ir_obstacle_check" \
              "全驱动函数耗时追踪"
}

# ==================== 主入口 ====================

check_root
check_ftrace

case "${1:-help}" in
    i2c)    mode_i2c    ;;
    weight) mode_weight ;;
    motor)  mode_motor  ;;
    ir)     mode_ir     ;;
    all)    mode_all    ;;
    stop)   trace_reset ;;
    *)
        cat <<'EOF'
用法: ./ftrace_feedcat.sh <mode>

  i2c     追踪 AHT30 I2C 总线每次传输耗时（验证 mdelay(80) 实际延迟抖动）
  weight  追踪 HX711 weight_get 完整采样耗时（GPIO 位翻转 + 计算）
  motor   追踪 motor_feed 喂食全流程耗时（启动到停止）
  ir      追踪 ir_obstacle_check 防抖路径耗时
  all     同时追踪以上全部函数
  stop    停止追踪，还原 ftrace 现场

环境变量：
  DURATION=<秒>  追踪持续时间（默认 5 秒）
  示例: DURATION=10 ./ftrace_feedcat.sh all
EOF
        exit 1
        ;;
esac
