# gunzip —— 从零手写的 gzip 压缩 / 解压工具

单文件 C 实现(仅用 C 标准库,不依赖 zlib 等第三方库)。解压端支持完整的
deflate(fixed / dynamic / stored 三种块、gzip 头部全部可选字段、CRC32 与
大小校验);压缩端为自写的 deflate 编码器(LZ77 + 动态 Huffman,码长超限
自动退化为固定 Huffman,不可压数据用 stored),压缩率大致与 `gzip -6` 相当。

## 构建

```bash
gcc main.c -o gunzip        # 建议加 -O2;-Wall -Wextra 下无警告
```

## 用法

### 演示模式(无参数)

解压并打印 `test0.gz` / `test1.gz` 的内容(即 RFC 1951 / RFC 1952 原文),
随后把解出的内容重新压缩,打印体积对比并做压缩→解压往返校验:

```bash
./gunzip
```

### 压缩

```bash
./gunzip -c <file>                 # 生成 <file>.gz
./gunzip -c <file> -o out.gz       # 指定输出文件名
```

### 解压

```bash
./gunzip -d <file.gz>              # 去掉 .gz 后缀还原,需以 .gz 结尾
./gunzip -d <file.gz> -o <out>     # 指定输出文件名
```

解压前校验 gzip 头(magic、压缩方法、保留位),解压后校验 CRC32 与原始
大小,文件损坏或截断时报错并退出码 1,不会产出错误文件。

## 示例

```bash
$ echo "hello gunzip" > demo.txt
$ ./gunzip -c demo.txt
12 bytes -> 35 bytes: demo.txt.gz     # 极小文本反而变大,属正常

$ ./gunzip -d demo.txt.gz
35 bytes -> 12 bytes: demo.txt

# 与系统 gzip 双向兼容
$ gzip -9 -k big.txt && ./gunzip -d big.txt.gz -o big.restored && cmp big.txt big.restored  # 解系统的
$ ./gunzip -c big.txt && gzip -d -c big.txt.gz | cmp - big.txt                              # 系统解我们的
```

## 文件布局(main.c)

```
头:    fatal / read_le16/32 / crc32
解压:  gunzip()          —— 解析 gzip 头、校验 trailer
       inflate()         —— 块循环(BTYPE 00/01/10)
       _fixed_block      —— 固定 Huffman
       _dynamic_block    —— 动态 Huffman(含 code-length RLE)
       _huff_impl        —— LUT 解码主循环
压缩:  lz_parse()        —— LZ77 贪心匹配(哈希链,窗口 32K)
       huff_build_lengths—— 堆建 Huffman 树 → 码长(超深返回失败)
       deflate()         —— 按体积选 dynamic / fixed / stored,写位流
       gzip()            —— 封装 gzip 头 + trailer
```

解码端与编码端共用 `build_huff_tree`(canonical 码)与
`ll_bases/ll_extras/dist_bases/dist_extras/cl_order` 等码表,天然对称。
`deflate.txt` / `gzip.txt` 为 RFC 1951 / RFC 1952 原文,供对照阅读。

## 设计取舍与已知边界

- 单块压缩:整个输入作为一个 deflate 块(极端情况下内存为输入的若干倍,
  适合 MB 级文件;大文件分块留作后续)。
- 贪婪 LZ77 解析(非 lazy/optimal);哈希链上限 128。
- 不支持多成员(拼接)gzip —— 会因 CRC/大小校验失败而报错退出。
- 压缩端不写 FNAME/mtime 等头字段(FLG=0),解压端对 FEXTRA/FNAME/
  FCOMMENT/FHCRC 均可读。
- stored 块 > 65535 字节时自动拆成多个 stored 块(规范上限)。

## 验证(测试脚本在仓库外)

交叉验证过的内容:`gzip` CLI 各级(0/1/6/9)输出、python zlib 各级输出、
含 FNAME 头与 stored 块的 18 个文件,全部与 python `gzip` 逐字节一致;
损坏输入(截断 / 翻位)82 例全部干净报错;ASAN 下无内存错误;压缩输出
可被系统 `gzip -d` 与 python `gzip` 正常解出。
