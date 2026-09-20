# 小学四则运算题目生成与批改程序

## 编译

使用 CMake：

```text
cmake -S . -B build
cmake --build build --config Release
```

也可以使用 Visual Studio：双击 `FourArithmetic.sln` 打开解决方案，选择 `Release` 和 `x64`，然后生成解决方案。生成的程序位于 `x64\Release\Myapp.exe`。

也可以直接使用支持 C++17 的编译器：

```text
g++ -std=c++17 -O2 -o Myapp main.cpp
```

## 使用

在希望保存输出文件的目录中运行：

```text
Myapp.exe -n 10 -r 10
```

`-r` 必须给出，表示生成范围中的自然数为 `0` 到 `r-1`，分母为 `2` 到 `r-1`。`-n` 默认为 10。程序会在当前目录生成 `Exercises.txt` 和 `Answers.txt`。

批改时运行：

```text
Myapp.exe -e Exercises.txt -a Answers.txt
```

批改结果会写入当前目录的 `Grade.txt`。

批改器兼容带英文句点的编号（`1. ...`）、中文顿号编号（`1、...`）以及不带编号的题目和答案。

程序使用有理数进行精确计算，支持自然数、普通分数和混合分数（例如 `1’3/8`），并且生成时保证：

- 每题包含 1 到 3 个运算符；
- 减法过程中不会产生负数；
- 除法结果是非零真分数；
- 同一次运行中不会出现可通过交换加法或乘法左右子表达式得到的重复题目。
