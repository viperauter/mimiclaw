#!/bin/bash
# Ceedling 环境配置脚本
export HOME=/tmp
export GEM_HOME=/tmp/gems
export PATH=$GEM_HOME/bin:$PATH

echo "Ceedling 环境已配置！"
echo "使用方法："
echo "  ceedling version   - 查看版本"
echo "  ceedling new       - 创建新项目"
echo "  ceedling test:all  - 运行所有测试"
