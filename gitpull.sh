#!/bin/bash

# git_rebase_retry.sh - 自动重试 git pull --rebase 直到成功
# 用法: ./git_rebase_retry.sh [最大重试次数] [重试间隔秒数]

# =============================================================================
# 配置参数
# =============================================================================

# 默认最大重试次数（0表示无限重试）
MAX_RETRIES=${1:-10}
# 默认重试间隔（秒）
RETRY_INTERVAL=${2:-5}
# 当前重试次数
CURRENT_ATTEMPT=0
# 成功标志
SUCCESS=false

# =============================================================================
# 颜色定义（用于输出）
# =============================================================================
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # 无色

# =============================================================================
# 功能函数
# =============================================================================

# 彩色输出函数
print_color() {
    local color=$1
    local message=$2
    echo -e "${color}${message}${NC}"
}

# 显示帮助信息
show_help() {
    echo "用法: $0 [最大重试次数] [重试间隔秒数]"
    echo ""
    echo "参数:"
    echo "  最大重试次数   最大重试次数 (默认: 10, 0=无限重试)"
    echo "  重试间隔秒数   每次重试之间的等待秒数 (默认: 5)"
    echo ""
    echo "示例:"
    echo "  $0           # 使用默认设置 (10次重试，间隔5秒)"
    echo "  $0 5 2       # 最多重试5次，每次间隔2秒"
    echo "  $0 0 10      # 无限重试，每次间隔10秒"
}

# 检查当前目录是否为git仓库
check_git_repo() {
    if ! git rev-parse --git-dir > /dev/null 2>&1; then
        print_color "$RED" "错误: 当前目录不是 Git 仓库！"
        exit 1
    fi
}

# 检查远程仓库是否可访问
check_remote_accessible() {
    local remote_url
    remote_url=$(git remote get-url origin 2>/dev/null)

    if [ -z "$remote_url" ]; then
        print_color "$YELLOW" "警告: 未找到远程仓库 'origin'"
        return 1
    fi

    print_color "$CYAN" "远程仓库: $remote_url"
    return 0
}

# 执行 git pull --rebase 并处理结果
execute_git_pull_rebase() {
    local attempt=$1
    local max_attempts=$2

    print_color "$BLUE" "尝试 $attempt/$max_attempts: 执行 git pull --rebase..."

    # 执行 git pull --rebase
    if git pull --rebase; then
        print_color "$GREEN" "✅ git pull --rebase 成功完成！"
        return 0
    else
        local error_code=$?
        print_color "$RED" "❌ git pull --rebase 失败 (退出码: $error_code)"

        # 显示具体的错误信息
        case $error_code in
            1)
                print_color "$YELLOW" "💡 可能需要手动解决冲突，请检查以上错误信息"
                ;;
            128)
                print_color "$YELLOW" "💡 Git 仓库可能存在问题，请检查仓库状态"
                ;;
            *)
                print_color "$YELLOW" "💡 可能是网络问题或权限问题"
                ;;
        esac

        return 1
    fi
}

# 显示重试进度
show_progress() {
    local current=$1
    local total=$2
    local interval=$3

    if [ "$total" -eq 0 ]; then
        print_color "$CYAN" "等待 ${interval} 秒后重试... (已尝试: $current 次, Ctrl+C 终止)"
    else
        local remaining=$((total - current))
        print_color "$CYAN" "等待 ${interval} 秒后重试... (进度: $current/$total, 剩余: $remaining 次)"
    fi

    # 显示倒计时
    for i in $(seq $interval -1 1); do
        printf "\r继续倒计时: %2d 秒" $i
        sleep 1
    done
    printf "\r                              \r" # 清空倒计时行
}

# 清理函数（用于信号处理）
cleanup() {
    print_color "$YELLOW" "脚本被用户中断"
    exit 1
}

# =============================================================================
# 主程序
# =============================================================================

# 设置信号处理
trap cleanup INT TERM

# 显示脚本头信息
echo "=================================================="
print_color "$CYAN" "Git Pull Rebase 自动重试脚本"
print_color "$CYAN" "开始时间: $(date)"
echo "=================================================="
echo ""

# 检查参数是否为帮助请求
if [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
    show_help
    exit 0
fi

# 验证输入参数
if ! [[ "$MAX_RETRIES" =~ ^[0-9]+$ ]]; then
    print_color "$RED" "错误: 最大重试次数必须是数字"
    exit 1
fi

if ! [[ "$RETRY_INTERVAL" =~ ^[0-9]+$ ]]; then
    print_color "$RED" "错误: 重试间隔必须是数字"
    exit 1
fi

# 显示配置信息
if [ "$MAX_RETRIES" -eq 0 ]; then
    print_color "$YELLOW" "配置: 无限重试模式，间隔: ${RETRY_INTERVAL} 秒"
else
    print_color "$YELLOW" "配置: 最大重试次数: ${MAX_RETRIES}，间隔: ${RETRY_INTERVAL} 秒"
fi
echo ""

# 检查是否在git仓库中
check_git_repo

# 显示当前分支信息
CURRENT_BRANCH=$(git branch --show-current)
print_color "$CYAN" "当前分支: $CURRENT_BRANCH"

# 检查远程仓库
check_remote_accessible

echo "开始执行 git pull --rebase..."
echo ""

# 主重试循环
while true; do
    CURRENT_ATTEMPT=$((CURRENT_ATTEMPT + 1))

    # 检查是否超过最大重试次数（非无限模式时）
    if [ "$MAX_RETRIES" -ne 0 ] && [ "$CURRENT_ATTEMPT" -gt "$MAX_RETRIES" ]; then
        print_color "$RED" "❌ 已达到最大重试次数 ($MAX_RETRIES)，放弃重试"
        exit 1
    fi

    # 执行 git pull --rebase
    if execute_git_pull_rebase "$CURRENT_ATTEMPT" "$MAX_RETRIES"; then
        SUCCESS=true
        break
    fi

    # 如果不是最后一次尝试，显示等待信息
    if [ "$MAX_RETRIES" -eq 0 ] || [ "$CURRENT_ATTEMPT" -lt "$MAX_RETRIES" ]; then
        echo ""
        show_progress "$CURRENT_ATTEMPT" "$MAX_RETRIES" "$RETRY_INTERVAL"
        echo ""
    fi
done

# 成功完成
echo ""
echo "=================================================="
print_color "$GREEN" "✅ 成功完成 git pull --rebase！"
print_color "$GREEN" "完成时间: $(date)"
print_color "$GREEN" "总尝试次数: $CURRENT_ATTEMPT"
echo "=================================================="

exit 0
