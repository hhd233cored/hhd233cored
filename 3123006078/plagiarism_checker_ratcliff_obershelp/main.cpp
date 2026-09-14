#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

// 使用 Unicode 码点保存字符，避免直接按 UTF-8 字节比较中文。
using CodePoint = std::uint32_t;

// 判断一个字节是否是 UTF-8 多字节字符的后续字节。
bool isContinuationByte(unsigned char byte) {
    return (byte & 0xC0U) == 0x80U;
}

/*
HTML 清洗发生在 UTF-8 解码之前，清洗结果只保存在内存中，不会生成
中间文件。普通文本只有在文件开头明显符合 HTML 特征时才会进入这里。
*/

// 将 ASCII 字母转换为小写，用于不区分大小写地判断 HTML 标签和属性。
std::string asciiLowerHtml(const std::string& text) {
    std::string result = text;
    for (char& character : result) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (byte >= static_cast<unsigned char>('A') &&
            byte <= static_cast<unsigned char>('Z')) {
            character = static_cast<char>(byte + ('a' - 'A'));
        }
    }
    return result;
}

// 判断 ASCII 空白字符，用于扫描 HTML 标签。
bool isAsciiSpaceHtml(char character) {
    return std::isspace(static_cast<unsigned char>(character)) != 0;
}

// 查找不区分大小写的字符串。只用于 HTML 文件，数据量通常不大。
bool containsIgnoreCaseHtml(const std::string& text,
                            const std::string& pattern) {
    return asciiLowerHtml(text).find(asciiLowerHtml(pattern)) !=
           std::string::npos;
}

/*
判断输入是否可能是 HTML。
只检查开头最多 8192 个字节，并跳过 BOM 和空白。因此普通论文中
出现数学符号“小于号”时，不会仅因为一个字符而触发 HTML 清洗。
 */
bool looksLikeHtml(const std::string& text) {
    std::size_t start = 0;
    if (text.size() >= 3 &&
        static_cast<unsigned char>(text[0]) == 0xEFU &&
        static_cast<unsigned char>(text[1]) == 0xBBU &&
        static_cast<unsigned char>(text[2]) == 0xBFU) {
        start = 3;
    }

    while (start < text.size() && isAsciiSpaceHtml(text[start])) {
        ++start;
    }

    const std::size_t inspectLength =
        text.size() - start < 8192 ? text.size() - start : 8192;
    const std::string beginning =
        asciiLowerHtml(text.substr(start, inspectLength));
    return beginning.find("<!doctype html") != std::string::npos ||
           beginning.find("<html") != std::string::npos;
}

/*
查找 HTML 标签的结束位置。
属性值中可能出现“>”，所以不能直接使用第一次出现的“>”，而要
记录当前是否位于单引号或双引号中。
 */
std::size_t findHtmlTagEnd(const std::string& text, std::size_t start) {
    char quote = '\0';
    for (std::size_t index = start; index < text.size(); ++index) {
        const char character = text[index];
        if (quote != '\0') {
            if (character == quote) {
                quote = '\0';
            }
        } else if (character == '\'' || character == '"') {
            quote = character;
        } else if (character == '>') {
            return index;
        }
    }
    return std::string::npos;
}

// 保存一个 HTML 标签的基本信息，供清洗状态机使用。
struct HtmlTagInfo {
    bool valid = false;
    bool closing = false;
    bool selfClosing = false;
    std::string name;
    std::string lowerText;
};

/*
解析标签名称和标签类型。
例如：
  <td class="blob-code">  -> name=td，closing=false
  </td>                   -> name=td，closing=true
*/
HtmlTagInfo parseHtmlTag(const std::string& tag) {
    HtmlTagInfo result;
    result.lowerText = asciiLowerHtml(tag);

    if (tag.size() < 3 || tag.front() != '<' || tag.back() != '>') {
        return result;
    }

    std::size_t index = 1;
    while (index + 1 < tag.size() && isAsciiSpaceHtml(tag[index])) {
        ++index;
    }

    // DOCTYPE、注释和处理指令不是普通的开始/结束标签。
    if (index + 1 >= tag.size() || tag[index] == '!' || tag[index] == '?') {
        return result;
    }

    if (tag[index] == '/') {
        result.closing = true;
        ++index;
        while (index + 1 < tag.size() && isAsciiSpaceHtml(tag[index])) {
            ++index;
        }
    }

    const std::size_t nameStart = index;
    while (index + 1 < tag.size() &&
           !isAsciiSpaceHtml(tag[index]) &&
           tag[index] != '/' && tag[index] != '>') {
        ++index;
    }
    if (index == nameStart) {
        return result;
    }

    result.name = asciiLowerHtml(tag.substr(nameStart, index - nameStart));
    result.valid = true;

    std::size_t beforeEnd = tag.size() - 1;
    while (beforeEnd > 0 && isAsciiSpaceHtml(tag[beforeEnd - 1])) {
        --beforeEnd;
    }
    result.selfClosing = beforeEnd > 0 && tag[beforeEnd - 1] == '/';
    return result;
}

// 在 script/style 等原始文本元素中寻找对应的结束标签。
std::size_t findHtmlClosingTag(const std::string& text,
                               std::size_t start,
                               const std::string& tagName) {
    const std::string marker = "</" + asciiLowerHtml(tagName);
    const std::string lowerText = asciiLowerHtml(text);
    std::size_t position = lowerText.find(marker, start);

    while (position != std::string::npos) {
        const std::size_t afterName = position + marker.size();
        if (afterName >= text.size() ||
            isAsciiSpaceHtml(text[afterName]) ||
            text[afterName] == '>') {
            return position;
        }
        position = lowerText.find(marker, position + 1);
    }
    return std::string::npos;
}

// 将 Unicode 码点重新编码为 UTF-8，用于解码数字 HTML 实体。
void appendUtf8CodePoint(CodePoint codePoint, std::string& output) {
    if (codePoint <= 0x7FU) {
        output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7FFU) {
        output.push_back(static_cast<char>(0xC0U | (codePoint >> 6U)));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
    } else if (codePoint <= 0xFFFFU) {
        output.push_back(static_cast<char>(0xE0U | (codePoint >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
    } else if (codePoint <= 0x10FFFFU) {
        output.push_back(static_cast<char>(0xF0U | (codePoint >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
    }
}

/*
解码正文中常见的 HTML 实体，例如：
  &lt;  -> <
  &gt;  -> >
  &amp; -> &
  &#20013; -> 中
未知实体保持原样，避免误删正文内容。
*/
std::string decodeHtmlEntities(const std::string& text) {
    std::string output;
    output.reserve(text.size());

    for (std::size_t index = 0; index < text.size();) {
        if (text[index] != '&') {
            output.push_back(text[index++]);
            continue;
        }

        const std::size_t semicolon = text.find(';', index + 1);
        if (semicolon == std::string::npos || semicolon - index > 16) {
            output.push_back(text[index++]);
            continue;
        }

        const std::string entity = text.substr(index + 1,
                                               semicolon - index - 1);
        const std::string lowerEntity = asciiLowerHtml(entity);

        if (lowerEntity == "amp") {
            output.push_back('&');
        } else if (lowerEntity == "lt") {
            output.push_back('<');
        } else if (lowerEntity == "gt") {
            output.push_back('>');
        } else if (lowerEntity == "quot") {
            output.push_back('"');
        } else if (lowerEntity == "apos") {
            output.push_back('\'');
        } else if (lowerEntity == "nbsp") {
            output.push_back(' ');
        } else if (!entity.empty() && entity[0] == '#') {
            int base = 10;
            std::size_t digitStart = 1;
            if (digitStart < entity.size() &&
                (entity[digitStart] == 'x' || entity[digitStart] == 'X')) {
                base = 16;
                ++digitStart;
            }

            CodePoint value = 0;
            bool valid = digitStart < entity.size();
            for (std::size_t digit = digitStart;
                 valid && digit < entity.size(); ++digit) {
                const unsigned char character =
                    static_cast<unsigned char>(entity[digit]);
                int digitValue = -1;

                if (character >= '0' && character <= '9') {
                    digitValue = character - '0';
                } else if (base == 16 && character >= 'a' && character <= 'f') {
                    digitValue = character - 'a' + 10;
                } else if (base == 16 && character >= 'A' && character <= 'F') {
                    digitValue = character - 'A' + 10;
                }

                if (digitValue < 0 || digitValue >= base ||
                    value > (0x10FFFFU - static_cast<CodePoint>(digitValue)) /
                                static_cast<CodePoint>(base)) {
                    valid = false;
                    break;
                }
                value = value * static_cast<CodePoint>(base) +
                        static_cast<CodePoint>(digitValue);
            }

            if (valid && value <= 0x10FFFFU &&
                !(value >= 0xD800U && value <= 0xDFFFU)) {
                appendUtf8CodePoint(value, output);
            } else {
                output.append(text, index, semicolon - index + 1);
            }
        } else {
            output.append(text, index, semicolon - index + 1);
        }

        index = semicolon + 1;
    }
    return output;
}

// 这些标签通常只表示网页布局，不承载论文正文。
bool isHtmlSkippedBlock(const std::string& tagName) {
    return tagName == "script" || tagName == "style" ||
           tagName == "head" || tagName == "nav" ||
           tagName == "header" || tagName == "footer" ||
           tagName == "form" || tagName == "svg" ||
           tagName == "noscript" || tagName == "template";
}

// 这些标签结束时补一个换行，避免相邻段落被错误拼接。
bool isHtmlBlockTag(const std::string& tagName) {
    return tagName == "p" || tagName == "div" ||
           tagName == "section" || tagName == "article" ||
           tagName == "li" || tagName == "tr" ||
           tagName == "h1" || tagName == "h2" ||
           tagName == "h3" || tagName == "h4" ||
           tagName == "h5" || tagName == "h6";
}

/*
清洗一份已经确认是 HTML 的文件。
当前测试文件是 GitHub 页面，真正的文本位于 class 属性包含
“blob-code”的 <td> 中。因此检测到 blob-code 时，只提取这些单元格，
避免把 GitHub 菜单、按钮和脚本内容当成论文正文。
对普通网页则保留可见文字，并跳过 script/style 等非正文区域。
*/
std::string cleanHtmlDocument(const std::string& html) {
    const bool onlyBlobCode = containsIgnoreCaseHtml(html, "blob-code");
    std::string output;
    output.reserve(html.size());

    bool inBlobCode = false;
    std::string skippedTag;

    for (std::size_t index = 0; index < html.size();) {
        if (!skippedTag.empty()) {
            const std::size_t closing =
                findHtmlClosingTag(html, index, skippedTag);
            if (closing == std::string::npos) {
                break;
            }

            const std::size_t closingEnd = findHtmlTagEnd(html, closing);
            if (closingEnd == std::string::npos) {
                break;
            }

            skippedTag.clear();
            index = closingEnd + 1;
            continue;
        }

        if (html.compare(index, 4, "<!--") == 0) {
            const std::size_t commentEnd = html.find("-->", index + 4);
            index = commentEnd == std::string::npos
                ? html.size() : commentEnd + 3;
            continue;
        }

        if (html[index] != '<') {
            const std::size_t nextTag = html.find('<', index);
            const std::size_t textEnd =
                nextTag == std::string::npos ? html.size() : nextTag;
            if (!onlyBlobCode || inBlobCode) {
                output += decodeHtmlEntities(
                    html.substr(index, textEnd - index));
            }
            index = textEnd;
            continue;
        }

        const std::size_t tagEnd = findHtmlTagEnd(html, index);
        if (tagEnd == std::string::npos) {
            if (!onlyBlobCode || inBlobCode) {
                output.push_back(html[index]);
            }
            ++index;
            continue;
        }

        const HtmlTagInfo tag =
            parseHtmlTag(html.substr(index, tagEnd - index + 1));
        if (tag.valid) {
            if (!tag.closing && !tag.selfClosing &&
                isHtmlSkippedBlock(tag.name)) {
                skippedTag = tag.name;
            } else if (onlyBlobCode) {
                if (!tag.closing && tag.name == "td" &&
                    tag.lowerText.find("blob-code") != std::string::npos) {
                    inBlobCode = true;
                } else if (tag.closing && tag.name == "td" && inBlobCode) {
                    inBlobCode = false;
                    output.push_back('\n');
                }
            } else if (tag.name == "br" || isHtmlBlockTag(tag.name)) {
                output.push_back('\n');
            }
        }
        index = tagEnd + 1;
    }

    return output;
}

/*
UTF-8 和文本归一化

*/

/*
将 UTF-8 字节串解码成 Unicode 码点数组。
遇到 BOM、非法编码或不完整编码时，程序不会异常退出，而是跳过
BOM，或使用 U+FFFD 继续处理。
*/
std::vector<CodePoint> decodeUtf8(const std::string& text) {
    std::vector<CodePoint> codePoints;
    codePoints.reserve(text.size());

    std::size_t index = 0;
    if (text.size() >= 3 &&
        static_cast<unsigned char>(text[0]) == 0xEFU &&
        static_cast<unsigned char>(text[1]) == 0xBBU &&
        static_cast<unsigned char>(text[2]) == 0xBFU) {
        index = 3;
    }

    while (index < text.size()) {
        const unsigned char first =
            static_cast<unsigned char>(text[index]);

        if (first <= 0x7FU) {
            codePoints.push_back(first);
            ++index;
            continue;
        }

        std::size_t length = 0;
        CodePoint codePoint = 0;
        CodePoint minimumValue = 0;

        if ((first & 0xE0U) == 0xC0U) {
            length = 2;
            codePoint = first & 0x1FU;
            minimumValue = 0x80U;
        } else if ((first & 0xF0U) == 0xE0U) {
            length = 3;
            codePoint = first & 0x0FU;
            minimumValue = 0x800U;
        } else if ((first & 0xF8U) == 0xF0U) {
            length = 4;
            codePoint = first & 0x07U;
            minimumValue = 0x10000U;
        } else {
            codePoints.push_back(0xFFFDU);
            ++index;
            continue;
        }

        if (index + length > text.size()) {
            codePoints.push_back(0xFFFDU);
            ++index;
            continue;
        }

        bool valid = true;
        for (std::size_t offset = 1; offset < length; ++offset) {
            const unsigned char continuation =
                static_cast<unsigned char>(text[index + offset]);
            if (!isContinuationByte(continuation)) {
                valid = false;
                break;
            }
            codePoint = (codePoint << 6U) | (continuation & 0x3FU);
        }

        if (!valid || codePoint < minimumValue ||
            codePoint > 0x10FFFFU ||
            (codePoint >= 0xD800U && codePoint <= 0xDFFFU)) {
            codePoints.push_back(0xFFFDU);
            ++index;
            continue;
        }

        codePoints.push_back(codePoint);
        index += length;
    }

    return codePoints;
}

// 统一全角/半角形式，并把英文大写转成小写。
CodePoint normalizeCodePoint(CodePoint codePoint) {
    if (codePoint == 0x3000U) {
        return 0x20U;
    }
    if (codePoint >= 0xFF10U && codePoint <= 0xFF19U) {
        return codePoint - 0xFEE0U;
    }
    if (codePoint >= 0xFF21U && codePoint <= 0xFF3AU) {
        codePoint -= 0xFEE0U;
    }
    if (codePoint >= 0xFF41U && codePoint <= 0xFF5AU) {
        return codePoint - 0xFEE0U;
    }
    if (codePoint >= static_cast<CodePoint>('A') &&
        codePoint <= static_cast<CodePoint>('Z')) {
        return codePoint + static_cast<CodePoint>('a' - 'A');
    }
    return codePoint;
}

// 判断 Unicode 空白字符。
bool isWhitespace(CodePoint codePoint) {
    if (codePoint <= 0x20U || codePoint == 0x00A0U ||
        codePoint == 0x1680U || codePoint == 0x2028U ||
        codePoint == 0x2029U || codePoint == 0x202FU ||
        codePoint == 0x205FU || codePoint == 0x3000U) {
        return true;
    }
    return codePoint >= 0x2000U && codePoint <= 0x200AU;
}

// 判断 ASCII 和常见中文标点，避免排版差异影响匹配。
bool isPunctuation(CodePoint codePoint) {
    if (codePoint < 0x80U) {
        return std::ispunct(static_cast<unsigned char>(codePoint)) != 0;
    }
    return (codePoint >= 0x2000U && codePoint <= 0x206FU) ||
           (codePoint >= 0x2E00U && codePoint <= 0x2E7FU) ||
           (codePoint >= 0x3000U && codePoint <= 0x303FU) ||
           (codePoint >= 0xFE10U && codePoint <= 0xFE1FU) ||
           (codePoint >= 0xFE30U && codePoint <= 0xFE4FU) ||
           (codePoint >= 0xFF01U && codePoint <= 0xFF65U);
}

/*
文本预处理流程：
  UTF-8 解码 -> 大小写/全角归一化 -> 删除空白和标点
*/
std::vector<CodePoint> normalizeText(const std::string& text) {
    const std::vector<CodePoint> decoded = decodeUtf8(text);
    std::vector<CodePoint> normalized;
    normalized.reserve(decoded.size());

    for (CodePoint codePoint : decoded) {
        codePoint = normalizeCodePoint(codePoint);
        if (!isWhitespace(codePoint) && !isPunctuation(codePoint)) {
            normalized.push_back(codePoint);
        }
    }
    return normalized;
}

// 先做 HTML 正文提取，再做统一的文本归一化。
std::vector<CodePoint> prepareInputText(std::string& content) {
    if (!looksLikeHtml(content)) {
        std::vector<CodePoint> result = normalizeText(content);
        content.clear();
        content.shrink_to_fit();
        return result;
    }

    std::string cleanedHtml = cleanHtmlDocument(content);
    content.clear();
    content.shrink_to_fit();

    std::vector<CodePoint> result = normalizeText(cleanedHtml);
    cleanedHtml.clear();
    cleanedHtml.shrink_to_fit();
    return result;
}


// Ratcliff/Obershelp 算法

// 一个待处理的匹配区间，表示两段序列的左右边界。
struct MatchRange {
    std::size_t originalBegin;
    std::size_t originalEnd;
    std::size_t plagiarizedBegin;
    std::size_t plagiarizedEnd;
};

// 一个最长连续公共片段的位置和长度。
struct MatchBlock {
    std::size_t originalBegin;
    std::size_t plagiarizedBegin;
    std::size_t length;
};

/*
位置索引：
  key   = 一个 Unicode 字符
  value = 该字符在第二篇文本中出现的所有下标

寻找公共子串时，只需要访问和当前字符相同的位置，不必扫描第二篇
文本的每一个字符，从而比完整的二维动态规划更节省内存。
*/
using PositionIndex =
    std::unordered_map<CodePoint, std::vector<std::size_t>>;

/*
为第二篇文本建立字符位置索引。
对长度至少 200 的文本，出现次数超过约 1% 的字符会被暂时视为
“高频字符”，不作为匹配片段的起点。这样可以避免两篇文本包含大量
重复字符时产生近似 O(nm) 的候选比较；高频字符仍然可以向已找到的
长匹配片段两侧扩展。
 */
PositionIndex buildPositionIndex(const std::vector<CodePoint>& text) {
    PositionIndex index;
    index.reserve(text.size());

    for (std::size_t position = 0; position < text.size(); ++position) {
        index[text[position]].push_back(position);
    }

    if (text.size() >= 200) {
        const std::size_t popularThreshold = text.size() / 100 + 1;
        for (auto iterator = index.begin(); iterator != index.end();) {
            if (iterator->second.size() > popularThreshold) {
                iterator = index.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }
    return index;
}

/*
在两个指定区间内寻找最长连续公共子串。
使用两行滚动数组记录“以当前字符结尾的公共后缀长度”，额外空间
为 O(第二篇区间长度)，避免分配 n*m 的二维数组。
*/
MatchBlock findLongestMatch(const std::vector<CodePoint>& original,
                            const std::vector<CodePoint>& plagiarized,
                            const PositionIndex& positionIndex,
                            const MatchRange& range) {
    MatchBlock best{
        range.originalBegin,
        range.plagiarizedBegin,
        0
    };

    if (range.originalBegin >= range.originalEnd ||
        range.plagiarizedBegin >= range.plagiarizedEnd) {
        return best;
    }

    const std::size_t plagiarizedLength =
        range.plagiarizedEnd - range.plagiarizedBegin;
    std::vector<std::size_t> previousLengths(plagiarizedLength, 0);
    std::vector<std::size_t> currentLengths(plagiarizedLength, 0);
    std::vector<std::size_t> previousTouched;
    std::vector<std::size_t> currentTouched;
    previousTouched.reserve(64);
    currentTouched.reserve(64);

    for (std::size_t originalPosition = range.originalBegin;
         originalPosition < range.originalEnd;
         ++originalPosition) {
        // 清除当前滚动数组上一轮留下的非零位置。
        for (const std::size_t relative : currentTouched) {
            currentLengths[relative] = 0;
        }
        currentTouched.clear();

        const auto iterator = positionIndex.find(original[originalPosition]);
        if (iterator != positionIndex.end()) {
            for (const std::size_t plagiarizedPosition : iterator->second) {
                if (plagiarizedPosition < range.plagiarizedBegin) {
                    continue;
                }
                if (plagiarizedPosition >= range.plagiarizedEnd) {
                    break;
                }

                const std::size_t relative =
                    plagiarizedPosition - range.plagiarizedBegin;
                const std::size_t previousLength =
                    relative == 0 ? 0 : previousLengths[relative - 1];
                const std::size_t currentLength = previousLength + 1;
                currentLengths[relative] = currentLength;
                currentTouched.push_back(relative);

                if (currentLength > best.length) {
                    best.originalBegin =
                        originalPosition - currentLength + 1;
                    best.plagiarizedBegin =
                        plagiarizedPosition - currentLength + 1;
                    best.length = currentLength;
                }
            }
        }

        // 下一轮用本轮结果作为 previous，用旧的 previous 作为工作数组。
        previousLengths.swap(currentLengths);
        previousTouched.swap(currentTouched);
    }

    /*
    位置索引会过滤高频字符。找到一个稀有字符作为锚点后，再向左右
    扩展，可以把锚点旁边连续的普通字符也纳入匹配片段。
     */
    if (best.length > 0) {
        while (best.originalBegin > range.originalBegin &&
               best.plagiarizedBegin > range.plagiarizedBegin &&
               original[best.originalBegin - 1] ==
                   plagiarized[best.plagiarizedBegin - 1]) {
            --best.originalBegin;
            --best.plagiarizedBegin;
            ++best.length;
        }

        while (best.originalBegin + best.length < range.originalEnd &&
               best.plagiarizedBegin + best.length < range.plagiarizedEnd &&
               original[best.originalBegin + best.length] ==
                   plagiarized[best.plagiarizedBegin + best.length]) {
            ++best.length;
        }
    }

    return best;
}

/*
计算 Ratcliff/Obershelp 的匹配字符数。
找到一个最长公共片段后，把它左右两侧拆成两个待处理区间，使用
显式栈代替递归，避免文本很长或片段很多时发生栈溢出。
 */
std::size_t calculateMatchedLength(const std::vector<CodePoint>& original,
                                   const std::vector<CodePoint>& plagiarized) {
    if (original.empty() || plagiarized.empty()) {
        return 0;
    }
    if (original == plagiarized) {
        return original.size();
    }

    const PositionIndex positionIndex = buildPositionIndex(plagiarized);
    std::vector<MatchRange> pending;
    pending.push_back({
        0,
        original.size(),
        0,
        plagiarized.size()
    });

    std::size_t matchedLength = 0;
    while (!pending.empty()) {
        const MatchRange range = pending.back();
        pending.pop_back();

        const MatchBlock block =
            findLongestMatch(original, plagiarized, positionIndex, range);
        if (block.length == 0) {
            continue;
        }

        matchedLength += block.length;

        // 处理最长片段左边的两段序列。
        if (block.originalBegin > range.originalBegin &&
            block.plagiarizedBegin > range.plagiarizedBegin) {
            pending.push_back({
                range.originalBegin,
                block.originalBegin,
                range.plagiarizedBegin,
                block.plagiarizedBegin
            });
        }

        // 处理最长片段右边的两段序列。
        const std::size_t originalAfter =
            block.originalBegin + block.length;
        const std::size_t plagiarizedAfter =
            block.plagiarizedBegin + block.length;
        if (originalAfter < range.originalEnd &&
            plagiarizedAfter < range.plagiarizedEnd) {
            pending.push_back({
                originalAfter,
                range.originalEnd,
                plagiarizedAfter,
                range.plagiarizedEnd
            });
        }
    }

    return matchedLength;
}

/*
Ratcliff/Obershelp 标准相似度：

  similarity = 2 * matchedLength /
               (originalLength + plagiarizedLength)

这是对称相似度：原文增加内容和抄袭版增加内容都会降低分数。
 */
double calculateSimilarity(const std::vector<CodePoint>& original,
                           const std::vector<CodePoint>& plagiarized) {
    if (original.empty() || plagiarized.empty()) {
        return 0.0;
    }

    const std::size_t matchedLength =
        calculateMatchedLength(original, plagiarized);
    const std::size_t totalLength = original.size() + plagiarized.size();
    if (totalLength == 0) {
        return 0.0;
    }

    const double similarity =
        2.0 * static_cast<double>(matchedLength) /
        static_cast<double>(totalLength);
    return similarity < 0.0 ? 0.0 : (similarity > 1.0 ? 1.0 : similarity);
}

/*
读取指定路径的整个文件。

使用二进制模式保留原始 UTF-8 字节。函数只访问命令行传入的 path，
不扫描目录，也不读取其他文件。
 */
bool readFile(const std::filesystem::path& path, std::string& content) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }

    input.seekg(0, std::ios::end);
    const std::streampos endPosition = input.tellg();
    if (endPosition < 0) {
        return false;
    }
    input.seekg(0, std::ios::beg);

    const auto size = static_cast<std::uintmax_t>(endPosition);
    if (size > static_cast<std::uintmax_t>(
                   std::numeric_limits<std::size_t>::max()) ||
        size > static_cast<std::uintmax_t>(
                   std::numeric_limits<std::streamsize>::max())) {
        return false;
    }

    content.resize(static_cast<std::size_t>(size));
    if (!content.empty()) {
        input.read(&content[0],
                   static_cast<std::streamsize>(content.size()));
        if (!input) {
            return false;
        }
    }
    return true;
}

} // namespace

/*
 * 评测时要求传入三个参数：
 *   argv[1]：原文文件路径
 *   argv[2]：抄袭版论文文件路径
 *   argv[3]：答案文件路径
 */
int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::cerr << "Usage: main.exe <original> <plagiarized> <answer>\n";
        return 1;
    }

    try {
        const std::filesystem::path originalPath =
            std::filesystem::path(argv[1]);
        const std::filesystem::path plagiarizedPath =
            std::filesystem::path(argv[2]);
        const std::filesystem::path answerPath =
            std::filesystem::path(argv[3]);

        std::string originalContent;
        std::string plagiarizedContent;

        // 先读取两个输入文件，再打开输出文件，避免路径相同时破坏输入。
        if (!readFile(originalPath, originalContent)) {
            std::cerr << "Cannot read original file.\n";
            return 2;
        }
        if (!readFile(plagiarizedPath, plagiarizedContent)) {
            std::cerr << "Cannot read plagiarized file.\n";
            return 2;
        }

        const std::vector<CodePoint> original =
            prepareInputText(originalContent);
        const std::vector<CodePoint> plagiarized =
            prepareInputText(plagiarizedContent);

        const double similarity =
            calculateSimilarity(original, plagiarized);

        std::ofstream answer(answerPath, std::ios::binary | std::ios::trunc);
        if (!answer) {
            std::cerr << "Cannot write answer file.\n";
            return 3;
        }

        answer << std::fixed << std::setprecision(2) << similarity << '\n';
        if (!answer) {
            std::cerr << "Cannot finish writing answer file.\n";
            return 3;
        }
    }
    catch (const std::bad_alloc&) {
        std::cerr << "Not enough memory.\n";
        return 4;
    }
    catch (const std::exception& exception) {
        std::cerr << "Processing failed: " << exception.what() << '\n';
        return 4;
    }
    catch (...) {
        std::cerr << "Processing failed.\n";
        return 4;
    }

    return 0;
}
