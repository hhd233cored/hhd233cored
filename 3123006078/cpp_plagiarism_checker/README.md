# C++ 轻量版论文查重程序

## 编译

使用支持 C++17 的编译器：

```text
g++ -std=c++17 -O2 main.cpp -o main.exe
```

使用 Visual Studio 打开 `cpp_plagiarism_checker.slnx` 后，选择 `Release` 和
`x64`，执行“生成解决方案”，程序目标名已经配置为 `main`，生成文件通常位于
`x64\Release\main.exe`。

## 运行

```text
main.exe [原文文件] [抄袭版文件] [答案文件]
```

例如：

```text
main.exe C:\tests\orig.txt C:\tests\orig_add.txt C:\tests\ans.txt
```

答案文件只包含一个 0 到 1 之间的小数，固定保留两位小数，例如：

```text
0.75
```

## 算法说明

程序将输入文件按 UTF-8 解码，去除 UTF-8 BOM、空白和常见标点，并将英文大写字母转换为小写。中文按照 Unicode 字符处理。

默认使用长度为 3 的字符 n-gram 作为特征。程序统计原文和抄袭版中每个特征的出现次数，然后计算：

```text
matched = Σ min(原文特征次数, 抄袭版特征次数)
重复率 = matched / 抄袭版特征总数
```

当文本长度不足 3 个字符时，特征长度自动调整为 1 或 2。该实现只使用 C++ 标准库，不连接网络，也不读取命令行参数以外的文件。

这个轻量版本适合处理增删改造成的近似重复，不保证识别语义相同但字面完全不同的表达，例如“星期天”和“周天”。
