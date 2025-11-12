#!/bin/bash

# =============================================================================
# Linux MAC Address Reader
# 功能：读取和显示Linux系统的MAC地址信息
# 作者：Assistant
# 版本：1.0
# =============================================================================

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
PURPLE='\033[0;35m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# 全局变量
SCRIPT_NAME="mac_reader.sh"
VERSION="1.0"
SHOW_COLORS=true

# =============================================================================
# 功能函数
# =============================================================================

# 显示使用帮助
show_help() {
    echo -e "${GREEN}用法: $SCRIPT_NAME [选项]${NC}"
    echo
    echo "选项:"
    echo "  -a, --all          显示所有网络接口的MAC地址"
    echo "  -i, --interface    指定网络接口(如: eth0, wlan0)"
    echo "  -f, --first        只显示第一个活动接口的MAC地址"
    echo "  -c, --no-color     禁用颜色输出"
    echo "  -v, --version      显示版本信息"
    echo "  -h, --help         显示此帮助信息"
    echo
    echo "示例:"
    echo "  $SCRIPT_NAME -a                    # 显示所有接口"
    echo "  $SCRIPT_NAME -i eth0               # 显示eth0的MAC地址"
    echo "  $SCRIPT_NAME -f                    # 显示第一个活动接口"
    echo "  $SCRIPT_NAME --no-color -a         # 无颜色显示所有接口"
    echo
}

# 显示版本信息
show_version() {
    echo -e "${GREEN}$SCRIPT_NAME 版本 $VERSION${NC}"
    echo "Linux MAC地址读取工具"
}

# 检查命令是否存在
check_command() {
    if ! command -v "$1" &> /dev/null; then
        echo -e "${RED}错误: 未找到命令 '$1'，请确保已安装${NC}" >&2
        return 1
    fi
    return 0
}

# 颜色输出函数
color_echo() {
    if [ "$SHOW_COLORS" = true ]; then
        echo -e "$1$2${NC}"
    else
        echo "$2"
    fi
}

# 获取所有网络接口列表
get_network_interfaces() {
    local interfaces=()

    # 方法1: 使用ip命令（推荐）
    if check_command ip; then
        while IFS= read -r interface; do
            if [[ -n "$interface" && "$interface" != "lo" ]]; then
                interfaces+=("$interface")
            fi
        done < <(ip -o link show | awk -F': ' '{print $2}' | grep -v lo)
    # 方法2: 使用ls /sys/class/net
    elif [ -d "/sys/class/net" ]; then
        while IFS= read -r interface; do
            if [[ -n "$interface" && "$interface" != "lo" ]]; then
                interfaces+=("$interface")
            fi
        done < <(ls /sys/class/net | grep -v lo)
    # 方法3: 使用ifconfig（旧系统）
    elif check_command ifconfig; then
        while IFS= read -r interface; do
            if [[ -n "$interface" && "$interface" != "lo" ]]; then
                interfaces+=("$interface")
            fi
        done < <(ifconfig -a | grep -o '^[^ ]*' | grep -v lo)
    else
        color_echo "$RED" "错误: 无法获取网络接口列表"
        return 1
    fi

    printf '%s\n' "${interfaces[@]}"
    return 0
}

# 获取指定接口的MAC地址
get_interface_mac() {
    local interface="$1"
    local mac_address=""

    # 检查接口是否存在
    if [ ! -e "/sys/class/net/$interface" ]; then
        color_echo "$RED" "错误: 网络接口 '$interface' 不存在"
        return 1
    fi

    # 方法1: 从/sys/class/net读取（最可靠）
    if [ -f "/sys/class/net/$interface/address" ]; then
        mac_address=$(cat "/sys/class/net/$interface/address" 2>/dev/null | tr '[:lower:]' '[:upper:]')
    fi

    # 方法2: 使用ip命令
    if [ -z "$mac_address" ] && check_command ip; then
        mac_address=$(ip link show "$interface" 2>/dev/null | grep -oP 'link/ether \K[0-9a-f:]+' | head -1 | tr '[:lower:]' '[:upper:]')
    fi

    # 方法3: 使用ifconfig
    if [ -z "$mac_address" ] && check_command ifconfig; then
        mac_address=$(ifconfig "$interface" 2>/dev/null | grep -oP 'ether \K[0-9a-f:]+' | head -1 | tr '[:lower:]' '[:upper:]')
    fi

    # 验证MAC地址格式
    if [[ -n "$mac_address" && "$mac_address" =~ ^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$ ]]; then
        echo "$mac_address"
        return 0
    else
        color_echo "$YELLOW" "警告: 接口 '$interface' 的MAC地址无效或不存在"
        return 1
    fi
}

# 获取接口状态
get_interface_status() {
    local interface="$1"
    local status="未知"

    if [ -f "/sys/class/net/$interface/operstate" ]; then
        status=$(cat "/sys/class/net/$interface/operstate")
        case "$status" in
            "up") status="已启动" ;;
            "down") status="已关闭" ;;
            "unknown") status="未知" ;;
        esac
    fi

    echo "$status"
}

# 获取接口类型
get_interface_type() {
    local interface="$1"
    local type="未知"

    if [ -d "/sys/class/net/$interface" ]; then
        if [ -d "/sys/class/net/$interface/wireless" ]; then
            type="无线"
        elif [ -f "/sys/class/net/$interface/type" ]; then
            local if_type=$(cat "/sys/class/net/$interface/type")
            case "$if_type" in
                "1") type="以太网" ;;
                "772") type="回环" ;;
                *) type="其他" ;;
            esac
        fi
    fi

    echo "$type"
}

# 显示单个接口的详细信息
show_interface_details() {
    local interface="$1"
    local mac_address=$(get_interface_mac "$interface")
    local status=$(get_interface_status "$interface")
    local type=$(get_interface_type "$interface")

    if [ -n "$mac_address" ]; then
        color_echo "$CYAN" "接口: $interface"
        echo "  MAC地址: $mac_address"
        echo "  状态: $status"
        echo "  类型: $type"

        # 显示IP地址（如果存在）
        if check_command ip; then
            local ip_addr=$(ip -4 addr show "$interface" 2>/dev/null | grep -oP 'inet \K[0-9.]+')
            if [ -n "$ip_addr" ]; then
                echo "  IP地址: $ip_addr"
            fi
        fi
        echo
    fi
}

# 显示所有网络接口的MAC地址
show_all_interfaces() {
    color_echo "$GREEN" "=== 系统网络接口MAC地址 ==="
    echo

    local interfaces
    interfaces=$(get_network_interfaces)

    if [ -z "$interfaces" ]; then
        color_echo "$YELLOW" "未找到可用的网络接口"
        return 1
    fi

    local count=0
    while IFS= read -r interface; do
        if [ -n "$interface" ]; then
            show_interface_details "$interface"
            ((count++))
        fi
    done <<< "$interfaces"

    color_echo "$BLUE" "找到 $count 个网络接口"
    echo
}

# 显示第一个活动接口的MAC地址
show_first_active_interface() {
    color_echo "$GREEN" "=== 第一个活动网络接口 ==="
    echo

    local interfaces
    interfaces=$(get_network_interfaces)

    if [ -z "$interfaces" ]; then
        color_echo "$YELLOW" "未找到可用的网络接口"
        return 1
    fi

    while IFS= read -r interface; do
        if [ -n "$interface" ]; then
            local status=$(get_interface_status "$interface")
            if [ "$status" = "已启动" ]; then
                show_interface_details "$interface"
                return 0
            fi
        fi
    done <<< "$interfaces"

    # 如果没有活动接口，显示第一个接口
    color_echo "$YELLOW" "未找到活动接口，显示第一个可用接口:"
    local first_interface=$(echo "$interfaces" | head -1)
    show_interface_details "$first_interface"
}

# 显示系统信息
show_system_info() {
    color_echo "$PURPLE" "=== 系统信息 ==="

    # 获取系统信息
    if [ -f "/etc/os-release" ]; then
        source /etc/os-release
        echo "操作系统: $PRETTY_NAME"
    fi

    if check_command uname; then
        echo "内核版本: $(uname -r)"
        echo "主机名: $(uname -n)"
    fi

    echo
}

# =============================================================================
# 主程序
# =============================================================================

main() {
    local show_all=false
    local show_first=false
    local specific_interface=""

    # 解析命令行参数
    while [[ $# -gt 0 ]]; do
        case $1 in
            -a|--all)
                show_all=true
                shift
                ;;
            -f|--first)
                show_first=true
                shift
                ;;
            -i|--interface)
                if [ -n "$2" ]; then
                    specific_interface="$2"
                    shift 2
                else
                    color_echo "$RED" "错误: --interface 需要指定接口名称"
                    exit 1
                fi
                ;;
            -c|--no-color)
                SHOW_COLORS=false
                shift
                ;;
            -v|--version)
                show_version
                exit 0
                ;;
            -h|--help)
                show_help
                exit 0
                ;;
            *)
                color_echo "$RED" "错误: 未知选项 '$1'"
                echo "使用 '$SCRIPT_NAME -h' 查看帮助"
                exit 1
                ;;
        esac
    done

    # 显示系统信息
    show_system_info

    # 根据参数执行相应操作
    if [ -n "$specific_interface" ]; then
        color_echo "$GREEN" "=== 指定接口信息: $specific_interface ==="
        echo
        show_interface_details "$specific_interface"
    elif [ "$show_all" = true ]; then
        show_all_interfaces
    elif [ "$show_first" = true ]; then
        show_first_active_interface
    else
        # 默认行为：显示所有接口
        show_all_interfaces
    fi

    return 0
}

# 脚本入口点
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    # 检查是否在Linux系统上运行
    if [[ "$(uname -s)" != "Linux" ]]; then
        echo -e "${RED}错误: 此脚本只能在Linux系统上运行${NC}" >&2
        exit 1
    fi

    # 检查root权限（非必须，但提示）
    if [[ $EUID -eq 0 ]]; then
        color_echo "$YELLOW" "警告: 以root权限运行脚本"
    fi

    # 运行主程序
    main "$@"
fi
