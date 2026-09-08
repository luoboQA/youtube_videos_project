# MNIST 神经网络训练器
一个用纯C语言实现的神经网络训练和推理项目，用于识别MNIST手写数字。项目实现了完整的前向传播、反向传播和梯度下降训练流程。

## 项目特点

- **纯C实现**：不依赖任何第三方机器学习库
- **自定义内存管理**：使用arena分配器管理内存
- **自动微分**：实现了计算图和反向传播
- **模块化设计**：矩阵运算、神经网络层、训练流程分离


## 快速开始
1. uv venv

2. source .venv/bin/activate  # Linux/WSL2

3. uv pip install tensorflow tensorflow-datasets numpy

4. python mnist.py # 生成MNIST数据

                   #train_images.mat：训练集图像（60000×784，float32）

                   #train_labels.mat：训练集标签（60000×10，one-hot编码）

                   #test_images.mat：测试集图像（10000×784，float32）

                   #test_labels.mat：测试集标签（10000×10，one-hot编码）
6. gcc -o mnist main.c -lm -O2 -Wall -Wextra
