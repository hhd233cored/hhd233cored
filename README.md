# 软件工程作业仓库

自我介绍（已省略），软件工程作业要求，出于隐私保护以及已在博文写过，这里就不再写了。

---

## 项目导航

1.个人项目——**论文查重系统**，包含以下模块：

- [核心项目：Ratcliff/Obershelp 论文查重程序](./3123006078/plagiarism_checker_ratcliff_obershelp/)  
  最终提交的主版本。基于改进的格式塔模式匹配算法（Ratcliff/Obershelp），结合字符倒排索引与高频字剪枝，具备较优的连续匹配保序性与精度。

- [对比项目：3-gram 论文查重程序](./3123006078/plagiarism_checker/)  
  项目初期的基线版本，基于 3-gram 特征集合与 Jaccard 相似度，作为技术演进的对照组保留。

- [自动化单元测试模块 (UnitTest1)](./3123006078/UnitTest1/)  
  基于 Visual Studio Native C++ 单元测试框架编写的 10 组全覆盖自动化测试套件，测试通过率 100%，代码行覆盖率达 98.7%（包含工程级动态插桩配置 `test.runsettings`）。
