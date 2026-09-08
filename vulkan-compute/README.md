# Vulkan 计算着色器示例 - 向量加法

这是一个使用Vulkan API进行GPU并行计算的入门示例程序。它演示了如何在GPU上执行计算着色器，对两个向量进行元素级加法运算。

### 功能
- 创建两个长度为16的浮点向量
- 使用GPU计算着色器执行向量加法
- 显示计算前后的数据对比

# 基础开发工具
sudo apt update
sudo apt install build-essential

# Vulkan开发包
sudo apt install vulkan-tools libvulkan-dev

# 着色器编译器
sudo apt install glslang-tools

# 快速开始
1. glslangValidator -V add.comp.glsl -o add.spv # 编译着色器

2. make # 编译主程序

3. ./vulkan-compute # 运行程序