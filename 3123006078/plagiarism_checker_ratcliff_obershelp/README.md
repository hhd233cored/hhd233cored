# Ratcliff/Obershelp 论文查重版本

## 运行

    main.exe [原文文件] [抄袭版文件] [答案文件]

例如：

    main.exe C:\tests\orig.txt C:\tests\orig_0.8_add.txt C:\tests\ans.txt

答案文件只输出一个固定保留两位小数的相似度，不输出百分号或其他文字。

## 算法说明

程序先按 UTF-8 解码文本，统一大小写和全角/半角形式，并去除空白和常见标点。
如果输入开头符合 '<!DOCTYPE html>' 或 '<html>'，程序会先在内存中清洗 HTML：

- 删除标签、注释、script、style 等网页代码；
- 对 GitHub 页面优先提取 blob-code 正文区域；
- 解码常见 HTML 实体。

清洗结果不会写入额外文件，普通文本不会进行 HTML 清洗。

正文预处理后，使用 Ratcliff/Obershelp 算法寻找最长连续公共片段，并递归处理
公共片段左右两侧。最终使用标准对称相似度公式：

    similarity = 2 * matchedLength / (originalLength + plagiarizedLength)

代码按 Unicode 码点比较中文，而不是把中文的 UTF-8 字节拆开比较。
位置索引和高频字符过滤用于降低长文本和重复字符造成的时间、内存开销。
